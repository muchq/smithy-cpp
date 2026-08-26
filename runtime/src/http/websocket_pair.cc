#include "smithy/http/websocket_pair.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "smithy/eventstream/frame.h"

namespace smithy::http {
namespace {

// The Beast session's receive-side buffer bound (beast_transport.cc),
// published on the class so backpressure tests derive from it.
constexpr std::size_t kQueueDepth = InMemoryWebSocketPair::kQueueDepth;

// A parked SendAsync: the message it will queue and the completion to fire.
struct PendingSend {
  eventstream::Message message;
  WebSocket::SendCallback callback;
};

// The session both ends share: one queue per direction, one session-wide
// closed flag (either end's Close ends the session for both), one mutex and
// condition variable for all four blocking call sites — contention is
// bounded by the contract's one sender + one receiver per end. The ADR-0019
// async slots park at most one receive and one send per end; completions
// fire after the lock is released, on whichever peer thread completed the
// operation (the pair has no executor to post to — unlike the wire
// transports, a synchronous ping-pong of inline completions recurses, so
// keep coroutine test volleys modest).
struct PairState {
  std::mutex mutex;
  std::condition_variable changed;
  std::array<std::deque<eventstream::Message>, 2> queues;
  // Indexed by the owning end's send index: an end's parked receive, its
  // parked send, and its counts of receivers/senders blocked in the
  // blocking calls (the async twin refuses while one waits, and a blocking
  // call waits behind a parked async op — the one-outstanding contract's
  // two halves, same shape as the Beast session).
  std::array<WebSocket::ReceiveCallback, 2> pending_receive;
  std::array<std::optional<PendingSend>, 2> pending_send;
  // Bumped on every pending_receive park; a receive-deadline watchdog fires
  // only for the generation it was armed with, so a stale deadline can
  // never time out a later receive (#130).
  std::array<std::uint64_t, 2> receive_park_generation{};
  std::array<int, 2> blocked_receivers{};
  std::array<int, 2> blocked_senders{};
  bool closed = false;
};

class PairEnd final : public WebSocket {
 public:
  PairEnd(std::shared_ptr<PairState> state, std::size_t send_index)
      : state_(std::move(state)), send_index_(send_index) {}

  Outcome<std::optional<eventstream::Message>> Receive() override {
    return ReceiveWithin(std::nullopt);
  }

  Outcome<std::optional<eventstream::Message>> Receive(std::chrono::milliseconds timeout) override {
    return ReceiveWithin(timeout);
  }

  Outcome<Unit> Send(const eventstream::Message& message) override {
    // Encode-validation parity with the wire transports: what the codec
    // refuses never enters the session, and the session stays usable. The
    // bytes themselves are dropped — the peer receives the message value,
    // which the codec's round-trip guarantees is the same thing.
    if (auto frame = eventstream::EncodeMessage(message); !frame.ok()) {
      return std::move(frame).error();  // the codec's Validation, verbatim
    }
    WebSocket::ReceiveCallback deliver;
    eventstream::Message delivered;
    {
      std::unique_lock<std::mutex> lock(state_->mutex);
      std::deque<eventstream::Message>& outbound = state_->queues[send_index_];
      // A parked async send goes first (FIFO across both send APIs).
      ++state_->blocked_senders[send_index_];
      state_->changed.wait(lock, [&] {
        return (outbound.size() < kQueueDepth && !state_->pending_send[send_index_]) ||
               state_->closed;
      });
      --state_->blocked_senders[send_index_];
      if (state_->closed) {
        return Error::Transport("websocket pair: session is closed");
      }
      deliver = TakePeerReceiverLocked(message, delivered);
      if (!deliver) {
        outbound.push_back(message);
        state_->changed.notify_all();
      }
    }
    if (deliver) deliver(std::optional<eventstream::Message>(std::move(delivered)));
    return Unit{};
  }

  void Close() override {
    // One close ends the session for both ends, so both ends' waiters come
    // out here — one TerminalWaiters each, since the send-before-receive
    // rule it enforces is per-session-state: what an end's receive
    // completion can tear down waits only on that end's pins.
    std::array<WebSocket::TerminalWaiters, 2> waiters;
    {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      state_->closed = true;
      for (std::size_t end = 0; end < 2; ++end) {
        WebSocket::SendCallback send;
        std::optional<PendingSend>& parked = state_->pending_send[end];
        if (parked.has_value()) {
          send = std::move(parked->callback);
          parked.reset();  // emptied here, as the slot is an optional, not a callback
        }
        // std::exchange, never a bare std::move: libc++'s small-buffer
        // std::function move leaves the source engaged, and these slots'
        // emptiness is the one-outstanding busy signal.
        waiters[end] = WebSocket::TerminalWaiters(
            std::exchange(state_->pending_receive[end], nullptr), std::move(send));
      }
      state_->changed.notify_all();
    }
    // A parked receive implies its queue was empty (any push completes it
    // immediately), so the clean end is the honest outcome. Nothing to
    // contain here: the pair has no io thread to protect, so the invoker
    // just calls through.
    for (auto& end : waiters) {
      std::move(end).Fire(
          Error::Transport("websocket pair: session is closed"),
          std::optional<eventstream::Message>(),
          [](const char*, const auto& callback, auto outcome) { callback(std::move(outcome)); });
    }
  }

  void ReceiveAsync(WebSocket::ReceiveCallback callback) override {
    ReceiveAsyncWithin(std::nullopt, std::move(callback));
  }

  // The deadline overload (#130): same immediate paths, and a parked
  // receive races its deadline instead of waiting forever.
  void ReceiveAsync(std::chrono::milliseconds timeout,
                    WebSocket::ReceiveCallback callback) override {
    ReceiveAsyncWithin(timeout, std::move(callback));
  }

  void SendAsync(const eventstream::Message& message, WebSocket::SendCallback callback) override {
    if (auto frame = eventstream::EncodeMessage(message); !frame.ok()) {
      callback(std::move(frame).error());  // the codec's Validation, inline
      return;
    }
    WebSocket::ReceiveCallback deliver;
    eventstream::Message delivered;
    {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->closed) {
        callback(Error::Transport("websocket pair: session is closed"));
        return;
      }
      if (state_->pending_send[send_index_].has_value() ||
          state_->blocked_senders[send_index_] > 0) {
        callback(Error::Validation("websocket pair: a send is already in flight"));
        return;
      }
      std::deque<eventstream::Message>& outbound = state_->queues[send_index_];
      if (outbound.size() >= kQueueDepth) {
        // Backpressure without blocking: park until the receiver drains.
        state_->pending_send[send_index_] = PendingSend{message, std::move(callback)};
        return;
      }
      deliver = TakePeerReceiverLocked(message, delivered);
      if (!deliver) {
        outbound.push_back(message);
        state_->changed.notify_all();
      }
    }
    if (deliver) deliver(std::optional<eventstream::Message>(std::move(delivered)));
    callback(Unit{});
  }

  bool SupportsAsync() const override { return true; }

 private:
  // Both async receives: `timeout` engaged bounds the park, disengaged
  // parks until a send or the close completes it. The pair has no executor
  // to run a timer on, so a timed park arms a detached watchdog thread —
  // short-lived (it exits at the deadline or as soon as the park
  // completes), and it owns nothing but the shared state it sleeps on. The
  // park generation settles the watchdog/delivery race exactly once under
  // the state mutex, so a stale deadline can never time out a later
  // receive (timed or not).
  void ReceiveAsyncWithin(std::optional<std::chrono::milliseconds> timeout,
                          WebSocket::ReceiveCallback callback) {
    WebSocket::SendCallback absorbed;
    Outcome<std::optional<eventstream::Message>> immediate = std::optional<eventstream::Message>();
    // 0 = not parked (the generation counter starts at 1 on the first park).
    std::uint64_t parked_generation = 0;
    {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->pending_receive[send_index_] || state_->blocked_receivers[send_index_] > 0) {
        callback(Error::Validation("websocket pair: a receive is already outstanding"));
        return;
      }
      std::deque<eventstream::Message>& inbound = state_->queues[1 - send_index_];
      if (!inbound.empty()) {
        eventstream::Message message = std::move(inbound.front());
        inbound.pop_front();
        absorbed = AbsorbPeerPendingSendLocked();
        state_->changed.notify_all();
        immediate = std::optional<eventstream::Message>(std::move(message));
      } else if (state_->closed) {
        immediate = std::optional<eventstream::Message>();
      } else if (timeout.has_value() && *timeout <= std::chrono::milliseconds::zero()) {
        // The blocking overload's poll shape: nothing already in hand is a
        // timeout, completed inline like the other immediates.
        immediate = Error::Timeout("websocket pair: no message within the receive deadline");
      } else {
        state_->pending_receive[send_index_] = std::move(callback);
        parked_generation = ++state_->receive_park_generation[send_index_];
      }
    }
    if (parked_generation != 0) {
      // A send, the deadline, or the close completes the park.
      if (timeout.has_value()) ArmReceiveDeadline(parked_generation, *timeout);
      return;
    }
    if (absorbed) absorbed(Unit{});
    callback(std::move(immediate));
  }

  // Spawns the watchdog for the park `generation` — outside the lock, so
  // both ends' traffic never stalls behind pthread_create. It sleeps on
  // the shared condition variable, so a completed park (delivery, close,
  // or a fresh park's bumped generation) releases it early; at the
  // deadline, a park still bearing its generation is timed out — the slot
  // released exactly as a delivery releases it, the session untouched. A
  // spawn that fails must not leave the park unbounded: the park is taken
  // back (unless delivery already beat us to it) and refused, keeping the
  // callback exactly-once with no exception escaping.
  void ArmReceiveDeadline(std::uint64_t generation, std::chrono::milliseconds timeout) {
    // Saturate far-future deadlines (milliseconds::max() as "practically
    // forever") instead of overflowing now + timeout into the past.
    constexpr std::chrono::milliseconds kMaxWait = std::chrono::hours(24 * 365);
    const auto deadline = std::chrono::steady_clock::now() + std::min(timeout, kMaxWait);
    const auto watchdog = [state = state_, end = send_index_, generation, deadline] {
      WebSocket::ReceiveCallback expired;
      {
        std::unique_lock<std::mutex> lock(state->mutex);
        state->changed.wait_until(lock, deadline, [&] {
          return !state->pending_receive[end] || state->receive_park_generation[end] != generation;
        });
        if (!state->pending_receive[end] || state->receive_park_generation[end] != generation) {
          return;  // the park this deadline bounded already completed
        }
        expired = std::exchange(state->pending_receive[end], nullptr);
        // A blocking receiver may be waiting behind the parked slot; the
        // freed slot is part of its wake condition.
        state->changed.notify_all();
      }
      expired(Error::Timeout("websocket pair: no message within the receive deadline"));
    };
    // Under -fno-exceptions a failed thread spawn terminates (nothing can
    // throw), which is the fail-fast posture; with exceptions on, contain
    // it here so the caller never sees a throw beside a still-armed park.
#if defined(__cpp_exceptions)
    WebSocket::ReceiveCallback refused;
    try {
      std::thread(watchdog).detach();
      return;
    } catch (...) {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      if (state_->pending_receive[send_index_] &&
          state_->receive_park_generation[send_index_] == generation) {
        refused = std::exchange(state_->pending_receive[send_index_], nullptr);
        state_->changed.notify_all();
      }
    }
    if (refused) {
      refused(Error::Transport("websocket pair: cannot arm the receive deadline"));
    }
#else
    std::thread(watchdog).detach();
#endif
  }

  // Both receive overloads: `timeout` engaged bounds the wait, disengaged
  // is the unbounded blocking call.
  Outcome<std::optional<eventstream::Message>> ReceiveWithin(
      std::optional<std::chrono::milliseconds> timeout) {
    WebSocket::SendCallback absorbed;
    Outcome<std::optional<eventstream::Message>> result = std::optional<eventstream::Message>();
    {
      std::unique_lock<std::mutex> lock(state_->mutex);
      std::deque<eventstream::Message>& inbound = state_->queues[1 - send_index_];
      // A parked async receive owns the next arrival (the peer's send hands
      // it over directly), so a blocking receiver waits behind it — the
      // serialize-by-waiting half of the one-outstanding contract.
      auto arrived = [&] {
        return (!inbound.empty() && !state_->pending_receive[send_index_]) || state_->closed;
      };
      ++state_->blocked_receivers[send_index_];
      bool ready = true;
      if (timeout.has_value()) {
        ready = state_->changed.wait_for(lock, *timeout, arrived);
      } else {
        state_->changed.wait(lock, arrived);
      }
      // Dropping out of the count is all a timed-out receiver owes the
      // session: nobody waits on this counter (the async twin only reads it,
      // to refuse), so there is nothing to notify and nothing to unwind.
      --state_->blocked_receivers[send_index_];
      if (!ready) {
        return Error::Timeout("websocket pair: no message within the receive deadline");
      }
      if (inbound.empty()) {
        return std::optional<eventstream::Message>();  // the stream's clean end
      }
      // Messages queued before a close still belong to the application, in
      // order (the wire session's drain behavior).
      eventstream::Message message = std::move(inbound.front());
      inbound.pop_front();
      absorbed = AbsorbPeerPendingSendLocked();
      state_->changed.notify_all();  // wake a sender blocked on the bound
      result = std::optional<eventstream::Message>(std::move(message));
    }
    if (absorbed) absorbed(Unit{});
    return result;
  }

  // With the lock held: if the peer parked an async receive, hand it this
  // message directly (its queue is empty by the parked-receive invariant);
  // the caller fires the returned callback after unlocking.
  WebSocket::ReceiveCallback TakePeerReceiverLocked(const eventstream::Message& message,
                                                    eventstream::Message& delivered) {
    WebSocket::ReceiveCallback& parked = state_->pending_receive[1 - send_index_];
    if (!parked) return nullptr;
    delivered = message;
    // The emptied slot releases a deadline watchdog sleeping on it (#130);
    // harmless for everyone else.
    state_->changed.notify_all();
    return std::exchange(parked, nullptr);
  }

  // With the lock held: after this end freed queue space by receiving, the
  // peer's parked async send (if any) takes the slot — FIFO order for its
  // direction. Returns its completion for the caller to fire.
  WebSocket::SendCallback AbsorbPeerPendingSendLocked() {
    std::optional<PendingSend>& parked = state_->pending_send[1 - send_index_];
    if (!parked.has_value() || state_->queues[1 - send_index_].size() >= kQueueDepth) {
      return nullptr;
    }
    state_->queues[1 - send_index_].push_back(std::move(parked->message));
    WebSocket::SendCallback callback = std::move(parked->callback);
    parked.reset();
    return callback;
  }

  std::shared_ptr<PairState> state_;
  std::size_t send_index_;
};

}  // namespace

std::pair<std::shared_ptr<WebSocket>, std::shared_ptr<WebSocket>> InMemoryWebSocketPair::Create() {
  auto state = std::make_shared<PairState>();
  return {std::make_shared<PairEnd>(state, 0), std::make_shared<PairEnd>(state, 1)};
}

}  // namespace smithy::http
