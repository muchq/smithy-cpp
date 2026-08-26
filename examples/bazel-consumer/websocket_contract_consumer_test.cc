// Out-of-tree proof for the ADR-0019 implementor contract (#173): a
// WebSocket written entirely in consumer code adopts the async primitives,
// runs its terminal transition through WebSocket::TerminalWaiters, and is
// held to the same shared contract suite the in-repo transports are.
//
// This is what makes the seam's claim real rather than aspirational. The
// runtime says the async methods are public virtuals a third party may
// override, and that overriding them accepts the send-before-receive rule;
// this test is the only place that a third party actually does so — across
// the module boundary, against the published targets alone, with no
// in-repo transport in the loop.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>

#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"
#include "smithy/testing/websocket_contract_test.h"

namespace {

using smithy::Outcome;
using smithy::Unit;
using smithy::eventstream::Message;
using smithy::http::WebSocket;

// A third-party session: a bounded outbound wire nobody drains, one parked
// receive, one parked send. Deliberately minimal — the point is not the
// wire but that the terminal transition is expressed with TerminalWaiters,
// so this implementation inherits the ordering rule without its author
// having to rediscover why the rule exists.
class ConsumerSocket final : public WebSocket, public std::enable_shared_from_this<ConsumerSocket> {
 public:
  // Small on purpose: the contract suite wedges the wire by sending, and a
  // shallow queue gets there in a few messages.
  static constexpr std::size_t kDepth = 4;

  Outcome<std::optional<Message>> Receive() override {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return closed_; });
    return std::optional<Message>();  // this socket's peer only ever ends it
  }

  Outcome<std::optional<Message>> Receive(std::chrono::milliseconds timeout) override {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!changed_.wait_for(lock, timeout, [this] { return closed_; })) {
      return smithy::Error::Timeout("consumer socket: no message within the deadline");
    }
    return std::optional<Message>();
  }

  Outcome<Unit> Send(const Message& message) override {
    std::unique_lock<std::mutex> lock(mutex_);
    changed_.wait(lock, [this] { return queued_ < kDepth || closed_; });
    if (closed_) return smithy::Error::Transport("consumer socket: session is closed");
    ++queued_;
    (void)message;
    return Unit{};
  }

  void Close() override { EndSession(); }

  bool SupportsAsync() const override { return true; }

  // Both async twins: park under the lock, or complete inline once the
  // lock is released — the seam's documented shapes, nothing more.
  void ReceiveAsync(ReceiveCallback callback) override {
    Outcome<std::optional<Message>> immediate = std::optional<Message>();  // the clean end
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!closed_ && !pending_receive_) {
        pending_receive_ = std::move(callback);
        ++receive_park_generation_;  // a stale deadline must not fire this park
        return;                      // EndSession completes it
      }
      if (!closed_) {
        immediate = smithy::Error::Validation("consumer socket: a receive is already outstanding");
      }
    }
    callback(std::move(immediate));
  }

  // The deadline overload (#130): the same park, bounded by a watchdog
  // thread racing the terminal transition — the executor-less shape. The
  // park generation settles the race exactly once under the lock, so a
  // stale deadline can never time out a later receive; the watchdog holds
  // the socket alive (shared_from_this) for at most its own deadline, and
  // it is spawned outside the lock so a park never stalls the session
  // behind pthread_create.
  void ReceiveAsync(std::chrono::milliseconds timeout, ReceiveCallback callback) override {
    Outcome<std::optional<Message>> immediate = std::optional<Message>();  // the clean end
    // The completion this call still owes; parking hands it to the session
    // and leaves this empty, so the slot itself says which happened.
    ReceiveCallback deliver = std::move(callback);
    std::uint64_t parked_generation = 0;  // 0 = not parked (the counter starts at 1)
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!closed_ && !pending_receive_ && timeout > std::chrono::milliseconds::zero()) {
        pending_receive_ = std::exchange(deliver, nullptr);
        parked_generation = ++receive_park_generation_;
      } else if (!closed_ && pending_receive_) {
        immediate = smithy::Error::Validation("consumer socket: a receive is already outstanding");
      } else if (!closed_) {
        // The non-positive poll — this peer never sends, so nothing is
        // ever already in hand.
        immediate = smithy::Error::Timeout("consumer socket: no message within the deadline");
      }
    }
    if (!deliver) {
      ArmDeadline(parked_generation, timeout);  // EndSession or the deadline completes it
      return;
    }
    deliver(std::move(immediate));
  }

  void SendAsync(const Message& message, SendCallback callback) override {
    (void)message;
    Outcome<Unit> immediate = Unit{};
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) {
        immediate = smithy::Error::Transport("consumer socket: session is closed");
      } else if (pending_send_) {
        immediate = smithy::Error::Validation("consumer socket: a send is already in flight");
      } else if (queued_ >= kDepth) {
        pending_send_ = std::move(callback);  // parked on the full wire
        return;
      } else {
        ++queued_;
      }
    }
    callback(std::move(immediate));
  }

  // The far side ending the session — a peer close, a reset, whatever this
  // implementation's wire calls it. Takes both parked completions under the
  // lock, releases it, and fires through TerminalWaiters, which is what
  // puts the send ahead of the receive.
  void EndSession() {
    WebSocket::TerminalWaiters waiters;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (closed_) return;
      closed_ = true;
      waiters = WebSocket::TerminalWaiters(std::exchange(pending_receive_, nullptr),
                                           std::exchange(pending_send_, nullptr));
      changed_.notify_all();
    }
    std::move(waiters).Fire(
        smithy::Error::Transport("consumer socket: session is closed"), std::optional<Message>(),
        [](const char*, const auto& callback, auto outcome) { callback(std::move(outcome)); });
  }

 private:
  // Arms the watchdog for the park `generation`. It sleeps on the shared
  // condition variable, so a completed park (EndSession, or a fresh park's
  // bumped generation) releases it early; at the deadline, a park still
  // bearing its generation is timed out — the slot released exactly as a
  // completion releases it. A spawn that fails must not leave the park
  // unbounded: it is taken back (unless the session ended it first) and
  // refused, keeping the callback exactly-once with nothing thrown at the
  // caller. The saturation guards milliseconds::max()-style "practically
  // forever" deadlines from overflowing into the past.
  void ArmDeadline(std::uint64_t generation, std::chrono::milliseconds timeout) {
    constexpr std::chrono::milliseconds kMaxWait = std::chrono::hours(24 * 365);
    const auto deadline = std::chrono::steady_clock::now() + std::min(timeout, kMaxWait);
    const auto watchdog = [self = shared_from_this(), generation, deadline] {
      ReceiveCallback expired;
      {
        std::unique_lock<std::mutex> lock(self->mutex_);
        self->changed_.wait_until(lock, deadline, [&] {
          return !self->pending_receive_ || self->receive_park_generation_ != generation;
        });
        if (!self->pending_receive_ || self->receive_park_generation_ != generation) {
          return;  // the park this deadline bounded already completed
        }
        expired = std::exchange(self->pending_receive_, nullptr);
        self->changed_.notify_all();
      }
      expired(smithy::Error::Timeout("consumer socket: no message within the deadline"));
    };
    ReceiveCallback refused;
    try {
      std::thread(watchdog).detach();
      return;
    } catch (...) {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (pending_receive_ && receive_park_generation_ == generation) {
        refused = std::exchange(pending_receive_, nullptr);
        changed_.notify_all();
      }
    }
    if (refused) {
      refused(smithy::Error::Transport("consumer socket: cannot arm the receive deadline"));
    }
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  ReceiveCallback pending_receive_;
  SendCallback pending_send_;
  std::uint64_t receive_park_generation_ = 0;
  std::size_t queued_ = 0;
  bool closed_ = false;
};

struct ConsumerContractDriver {
  static constexpr int kWedgeAttempts = static_cast<int>(ConsumerSocket::kDepth) + 4;

  std::shared_ptr<WebSocket> Socket() { return socket_; }

  Message BulkMessage(int n) {
    return Message{.headers = {{":event-type", "bulk"}},
                   .payload = smithy::Blob::FromString(std::to_string(n))};
  }

  void EndSessionFromPeer() { socket_->EndSession(); }

  std::shared_ptr<ConsumerSocket> socket_ = std::make_shared<ConsumerSocket>();
};

}  // namespace

// gtest builds the registration symbols from the bare suite name, so the
// instantiation lives in the namespace the suite was registered in.
namespace smithy::testing {
INSTANTIATE_TYPED_TEST_SUITE_P(ConsumerSocket, WebSocketContractTest, ConsumerContractDriver);
}  // namespace smithy::testing
