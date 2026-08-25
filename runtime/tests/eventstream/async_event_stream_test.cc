// Pins ADR-0019 over the in-memory pair: the completion-driven socket
// primitives (park/complete, one-outstanding refusals, close semantics),
// the coroutine adapter (co_await Receive/Send with EventStream's exact
// terminal behaviors), and the handle's async send with its
// revocation-spanning pin. The Beast halves of the same contracts live in
// beast_websocket_test.cc; the registry's async delivery in
// session_registry_test.cc.

#include "smithy/eventstream/async_event_stream.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "smithy/eventstream/event_stream.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"
#include "smithy/http/websocket_pair.h"
#include "smithy/testing/websocket_contract_test.h"

namespace smithy::eventstream {
namespace {

constexpr std::size_t kWireDepth = http::InMemoryWebSocketPair::kQueueDepth;

// The event_stream_test codec shapes, minimal: distinct directions so a
// swapped parameter cannot compile away.
struct Ping {
  int number = 0;
};
struct Pong {
  std::string text;
};

Outcome<Message> EncodePing(const Ping& ping) {
  return Message{.headers = {{":event-type", "ping"}},
                 .payload = Blob::FromString(std::to_string(ping.number))};
}

Outcome<Ping> DecodePing(const Message& message) {
  const std::string* type = message.FindString(":event-type");
  if (type == nullptr || *type != "ping") {
    return Error::Serialization("not a ping");
  }
  return Ping{std::stoi(message.payload.ToString())};
}

Outcome<Message> EncodePong(const Pong& pong) {
  return Message{.headers = {{":event-type", "pong"}}, .payload = Blob::FromString(pong.text)};
}

Outcome<Pong> DecodePong(const Message& message) { return Pong{message.payload.ToString()}; }

Message RawPing(int number) {
  return Message{.headers = {{":event-type", "ping"}},
                 .payload = Blob::FromString(std::to_string(number))};
}

// A one-shot completion mailbox: tests park an async op and assert on what
// (and that exactly one thing) arrived.
template <typename T>
class Mailbox {
 public:
  void Post(T value) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      ASSERT_FALSE(value_.has_value()) << "completion fired twice";
      value_.emplace(std::move(value));
    }
    ready_.notify_all();
  }

  T Wait() {
    std::unique_lock<std::mutex> lock(mutex_);
    // Deadlined so a completion that regresses to never-firing fails the
    // test legibly instead of hanging out the whole bazel timeout.
    if (!ready_.wait_for(lock, std::chrono::seconds(30), [this] { return value_.has_value(); })) {
      ADD_FAILURE() << "mailbox: no completion arrived within the deadline";
      std::abort();  // T (an Outcome) has no default value to limp on with
    }
    T value = std::move(*value_);
    value_.reset();
    return value;
  }

  bool Empty() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return !value_.has_value();
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::optional<T> value_;
};

// ---------------------------------------------------------------------------
// The raw pair primitives.
// ---------------------------------------------------------------------------

TEST(PairAsyncTest, AParkedReceiveCompletesOnThePeersSend) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> received;
  a->ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { received.Post(std::move(message)); });
  EXPECT_TRUE(received.Empty());  // parked: nothing sent yet

  ASSERT_TRUE(b->Send(RawPing(7)).ok());
  auto outcome = received.Wait();
  ASSERT_TRUE(outcome.ok() && outcome->has_value());
  EXPECT_EQ((*outcome)->payload.ToString(), "7");
}

TEST(PairAsyncTest, AReadyMessageCompletesImmediately) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  ASSERT_TRUE(b->Send(RawPing(1)).ok());
  Mailbox<Outcome<std::optional<Message>>> received;
  a->ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { received.Post(std::move(message)); });
  auto outcome = received.Wait();
  ASSERT_TRUE(outcome.ok() && outcome->has_value());
  EXPECT_EQ((*outcome)->payload.ToString(), "1");
}

// The #130 deadline over the pair: what the transport-neutral contract
// suite cannot pin because its driver has no "peer sends" hook.

TEST(PairAsyncTest, ATimedReceiveDeliversAMessageThatBeatsTheDeadline) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> received;
  a->ReceiveAsync(std::chrono::seconds(30), [&](Outcome<std::optional<Message>> message) {
    received.Post(std::move(message));
  });
  EXPECT_TRUE(received.Empty());  // parked: nothing sent yet

  ASSERT_TRUE(b->Send(RawPing(7)).ok());
  auto outcome = received.Wait();
  ASSERT_TRUE(outcome.ok() && outcome->has_value());
  EXPECT_EQ((*outcome)->payload.ToString(), "7");
}

TEST(PairAsyncTest, ATimedReceiveTimesOutAndALaterMessageWaitsForTheNextReceive) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> timed;
  a->ReceiveAsync(std::chrono::milliseconds(50),
                  [&](Outcome<std::optional<Message>> message) { timed.Post(std::move(message)); });
  auto expired = timed.Wait();
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().code(), "TimeoutError");

  // Nothing was lost to the deadline: the message the peer sends after it
  // waits in the session for whoever receives next.
  ASSERT_TRUE(b->Send(RawPing(9)).ok());
  Mailbox<Outcome<std::optional<Message>>> next;
  a->ReceiveAsync([&](Outcome<std::optional<Message>> message) { next.Post(std::move(message)); });
  auto outcome = next.Wait();
  ASSERT_TRUE(outcome.ok() && outcome->has_value());
  EXPECT_EQ((*outcome)->payload.ToString(), "9");
}

TEST(PairAsyncTest, AStaleDeadlineNeverFiresALaterReceive) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  // Park with a short deadline and complete it by delivery well inside it.
  Mailbox<Outcome<std::optional<Message>>> first;
  a->ReceiveAsync(std::chrono::milliseconds(200),
                  [&](Outcome<std::optional<Message>> message) { first.Post(std::move(message)); });
  ASSERT_TRUE(b->Send(RawPing(1)).ok());
  ASSERT_TRUE(first.Wait().ok());

  // A fresh (untimed) park now occupies the slot the expired deadline was
  // armed for. The generation guard makes the stale watchdog a no-op: well
  // past the original deadline, the new park is still waiting.
  Mailbox<Outcome<std::optional<Message>>> second;
  a->ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { second.Post(std::move(message)); });
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  EXPECT_TRUE(second.Empty()) << "a stale deadline timed out a receive it never bounded";

  ASSERT_TRUE(b->Send(RawPing(2)).ok());
  auto outcome = second.Wait();
  ASSERT_TRUE(outcome.ok() && outcome->has_value());
  EXPECT_EQ((*outcome)->payload.ToString(), "2");
}

TEST(PairAsyncTest, AZeroTimeoutReceivePollsWhatIsAlreadyInHand) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  ASSERT_TRUE(b->Send(RawPing(3)).ok());
  Mailbox<Outcome<std::optional<Message>>> polled;
  a->ReceiveAsync(std::chrono::milliseconds(0), [&](Outcome<std::optional<Message>> message) {
    polled.Post(std::move(message));
  });
  // Inline: the pair has no executor, so the poll completed before the call
  // returned — with the queued message, not a timeout.
  ASSERT_FALSE(polled.Empty());
  auto outcome = polled.Wait();
  ASSERT_TRUE(outcome.ok() && outcome->has_value());
  EXPECT_EQ((*outcome)->payload.ToString(), "3");
}

TEST(PairAsyncTest, ASecondOutstandingReceiveIsRefused) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> first;
  Mailbox<Outcome<std::optional<Message>>> second;
  a->ReceiveAsync([&](Outcome<std::optional<Message>> message) { first.Post(std::move(message)); });
  a->ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { second.Post(std::move(message)); });

  auto refused = second.Wait();  // inline refusal, the one-outstanding rule
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);

  ASSERT_TRUE(b->Send(RawPing(2)).ok());  // the first still completes
  auto outcome = first.Wait();
  ASSERT_TRUE(outcome.ok() && outcome->has_value());
}

TEST(PairAsyncTest, ABlockingReceiveWaitsBehindAParkedAsyncReceive) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> parked;
  a->ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { parked.Post(std::move(message)); });

  // The mixed-API half of the one-outstanding contract (websocket.h): a
  // blocking receive behind a parked async receive serializes by WAITING,
  // never by refusing — the parked receive owns the first message, the
  // waiter gets the second.
  Outcome<std::optional<Message>> second = std::optional<Message>();
  std::thread waiter([&] { second = a->Receive(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let it park

  ASSERT_TRUE(b->Send(RawPing(1)).ok());
  auto first = parked.Wait();
  ASSERT_TRUE(first.ok() && first->has_value());
  EXPECT_EQ((*first)->payload.ToString(), "1");

  ASSERT_TRUE(b->Send(RawPing(2)).ok());
  waiter.join();
  ASSERT_TRUE(second.ok()) << second.error().message();
  ASSERT_TRUE(second->has_value());
  EXPECT_EQ((*second)->payload.ToString(), "2");
}

TEST(PairAsyncTest, ReceiveAsyncBehindABlockedReceiverIsRefused) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  Outcome<std::optional<Message>> blocked = std::optional<Message>();
  std::thread waiter([&] { blocked = a->Receive(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let it park

  // The other mixed-API half: an async receive while a blocking receiver
  // waits is the second outstanding receive-class op — refused inline.
  Mailbox<Outcome<std::optional<Message>>> refused;
  a->ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { refused.Post(std::move(message)); });
  const bool refused_inline = !refused.Empty();

  // Unblock and join the waiter before asserting, so a regression fails
  // the test instead of terminating it on a joinable thread.
  if (refused_inline) {
    ASSERT_TRUE(b->Send(RawPing(3)).ok());  // the blocked receiver still wins its message
  } else {
    a->Close();  // regression: the async op parked; end the session to free the waiter
  }
  waiter.join();

  ASSERT_TRUE(refused_inline) << "the refusal must complete inline, not park";
  auto refusal = refused.Wait();
  ASSERT_FALSE(refusal.ok());
  EXPECT_EQ(refusal.error().kind(), ErrorKind::kValidation);
  ASSERT_TRUE(blocked.ok() && blocked->has_value());
  EXPECT_EQ((*blocked)->payload.ToString(), "3");
}

TEST(PairAsyncTest, AsyncSendParksOnTheFullWireAndDrainsInOrder) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  for (std::size_t i = 0; i < kWireDepth; ++i) {
    ASSERT_TRUE(a->Send(RawPing(static_cast<int>(i))).ok());
  }
  Mailbox<Outcome<Unit>> sent;
  a->SendAsync(RawPing(99), [&](Outcome<Unit> outcome) { sent.Post(std::move(outcome)); });
  EXPECT_TRUE(sent.Empty());  // parked behind the bound

  // A second send-class op while one is parked is refused.
  Mailbox<Outcome<Unit>> refused;
  a->SendAsync(RawPing(100), [&](Outcome<Unit> outcome) { refused.Post(std::move(outcome)); });
  auto refusal = refused.Wait();
  ASSERT_FALSE(refusal.ok());
  EXPECT_EQ(refusal.error().kind(), ErrorKind::kValidation);

  // Draining one message absorbs the parked send; FIFO holds end to end.
  for (std::size_t i = 0; i <= kWireDepth; ++i) {
    auto message = b->Receive();
    ASSERT_TRUE(message.ok() && message->has_value());
    const int expected = i < kWireDepth ? static_cast<int>(i) : 99;
    EXPECT_EQ((*message)->payload.ToString(), std::to_string(expected));
  }
  EXPECT_TRUE(sent.Wait().ok());
}

TEST(PairAsyncTest, CloseCompletesParkedOperations) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> received;
  a->ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { received.Post(std::move(message)); });
  for (std::size_t i = 0; i < kWireDepth; ++i) {
    ASSERT_TRUE(a->Send(RawPing(static_cast<int>(i))).ok());
  }
  Mailbox<Outcome<Unit>> sent;
  a->SendAsync(RawPing(99), [&](Outcome<Unit> outcome) { sent.Post(std::move(outcome)); });

  b->Close();
  // The parked receive gets the clean end (its queue was empty — the
  // parked-receive invariant); the parked send gets the transport error.
  auto end = received.Wait();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
  auto dead = sent.Wait();
  ASSERT_FALSE(dead.ok());
  EXPECT_EQ(dead.error().kind(), ErrorKind::kTransport);
}

TEST(PairAsyncTest, ThePairReportsAsyncSupportAndDefaultsRefuse) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  EXPECT_TRUE(a->SupportsAsync());

  // A WebSocket that overrides nothing keeps compiling and refuses politely
  // — the fallback every layer above keys on.
  class BlockingOnly final : public http::WebSocket {
   public:
    Outcome<std::optional<Message>> Receive() override { return std::optional<Message>(); }
    Outcome<std::optional<Message>> Receive(std::chrono::milliseconds) override {
      return std::optional<Message>();
    }
    Outcome<Unit> Send(const Message&) override { return Unit{}; }
    void Close() override {}
  };
  BlockingOnly plain;
  EXPECT_FALSE(plain.SupportsAsync());
  Mailbox<Outcome<Unit>> sent;
  plain.SendAsync(RawPing(1), [&](Outcome<Unit> outcome) { sent.Post(std::move(outcome)); });
  auto refused = sent.Wait();
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);
  Mailbox<Outcome<std::optional<Message>>> received;
  plain.ReceiveAsync(
      [&](Outcome<std::optional<Message>> message) { received.Post(std::move(message)); });
  auto refused_receive = received.Wait();
  ASSERT_FALSE(refused_receive.ok());
  EXPECT_EQ(refused_receive.error().kind(), ErrorKind::kValidation);
}

TEST(PairAsyncTest, SendAsyncAfterCloseRefusesInlineWithTransport) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  b->Close();
  Mailbox<Outcome<Unit>> sent;
  a->SendAsync(RawPing(1), [&](Outcome<Unit> outcome) { sent.Post(std::move(outcome)); });
  auto dead = sent.Wait();
  ASSERT_FALSE(dead.ok());
  EXPECT_EQ(dead.error().kind(), ErrorKind::kTransport);
}

TEST(PairAsyncTest, AnUnencodableSendAsyncRefusesInlineAndSparesTheSession) {
  auto [a, b] = http::InMemoryWebSocketPair::Create();
  // A header value past the codec's bound: EncodeMessage refuses, so the
  // message never enters the session.
  Message hostile = RawPing(1);
  // Brace-init, not emplace_back: Header is an aggregate, and Apple clang
  // lacks C++20 parenthesized aggregate initialization (P0960).
  hostile.headers.push_back({":poison", std::string(1 << 16, 'x')});
  Mailbox<Outcome<Unit>> sent;
  a->SendAsync(hostile, [&](Outcome<Unit> outcome) { sent.Post(std::move(outcome)); });
  auto refused = sent.Wait();
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);

  // The session is untouched: a well-formed send still round-trips.
  ASSERT_TRUE(a->Send(RawPing(2)).ok());
  auto message = b->Receive();
  ASSERT_TRUE(message.ok() && message->has_value());
  EXPECT_EQ((*message)->payload.ToString(), "2");
}

// ---------------------------------------------------------------------------
// The coroutine adapter.
// ---------------------------------------------------------------------------

using AsyncServer = AsyncEventStream<Pong, Ping>;

// The canonical Detached echo loop: pongs every ping's number back as text,
// ends on the client's close (or any terminal outcome), flags its exit.
Detached EchoLoop(std::shared_ptr<http::WebSocket> socket, std::atomic<bool>* done) {
  AsyncServer stream(std::move(socket), EncodePong, DecodePing);
  while (true) {
    auto ping = co_await stream.Receive();
    if (!ping.ok() || !ping->has_value()) break;
    auto sent = co_await stream.Send(Pong{"pong-" + std::to_string((*ping)->number)});
    if (!sent.ok()) break;
  }
  *done = true;
}

TEST(AsyncEventStreamTest, ADetachedLoopEchoesAndEndsOnTheCleanClose) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  std::atomic<bool> done{false};
  EchoLoop(server_socket, &done);

  EventStream<Ping, Pong> client(client_socket, EncodePing, DecodePong);
  for (int i = 0; i < 5; ++i) {
    ASSERT_TRUE(client.Send(Ping{i}).ok());
    auto pong = client.Receive();
    ASSERT_TRUE(pong.ok() && pong->has_value());
    EXPECT_EQ((*pong)->text, "pong-" + std::to_string(i));
  }
  EXPECT_FALSE(done.load());  // the loop is parked in co_await, not gone

  client.Close();
  // The parked receive completes with the clean end on this thread (the
  // pair's completion context), so the loop has unwound by the time Close
  // returns — but don't rely on that: poll briefly for the flag.
  for (int i = 0; i < 100 && !done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(done.load());
}

// The #130 watchdog shape: a loop that bounds each await, treats the
// timeout as "nothing yet" rather than an end, and keeps serving. The
// deadline is not terminal — the same stream receives the ping that
// arrives after a timeout and answers it.
TEST(AsyncEventStreamTest, AwaitedReceiveTimesOutAndTheSessionStaysUsable) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  std::atomic<int> timeouts{0};
  std::atomic<bool> done{false};

  [](std::shared_ptr<http::WebSocket> socket, std::atomic<int>* timeouts,
     std::atomic<bool>* done) -> Detached {
    AsyncServer stream(std::move(socket), EncodePong, DecodePing);
    while (true) {
      auto ping = co_await stream.Receive(std::chrono::milliseconds(50));
      if (!ping.ok()) {
        if (ping.error().code() == "TimeoutError") {
          ++*timeouts;  // not terminal: the watchdog tick, then wait again
          continue;
        }
        break;
      }
      if (!ping->has_value()) break;
      auto sent = co_await stream.Send(Pong{"pong-" + std::to_string((*ping)->number)});
      if (!sent.ok()) break;
    }
    *done = true;
  }(server_socket, &timeouts, &done);

  // Let at least one deadline expire before the first ping.
  for (int i = 0; i < 100 && timeouts.load() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_GT(timeouts.load(), 0) << "the awaited deadline never fired";
  EXPECT_FALSE(done.load()) << "a timeout must not end the loop";

  EventStream<Ping, Pong> client(client_socket, EncodePing, DecodePong);
  ASSERT_TRUE(client.Send(Ping{42}).ok());
  auto pong = client.Receive();
  ASSERT_TRUE(pong.ok() && pong->has_value());
  EXPECT_EQ((*pong)->text, "pong-42");

  client.Close();
  for (int i = 0; i < 100 && !done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(done.load());
}

// The raw awaitable's deadline (#130): ReceiveMessage with a timeout — the
// ADR-0023 launch-body shape, where a client that never sends its opening
// envelope must not park the serve coroutine forever.
TEST(AsyncEventStreamTest, ReceiveMessageHonorsItsDeadline) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> first;
  Mailbox<Outcome<std::optional<Message>>> second;

  [](std::shared_ptr<http::WebSocket> socket, Mailbox<Outcome<std::optional<Message>>>* first,
     Mailbox<Outcome<std::optional<Message>>>* second) -> Detached {
    first->Post(co_await ReceiveMessage(socket, std::chrono::milliseconds(50)));
    // The timeout released the slot: the same socket awaits again and gets
    // the envelope that arrives late.
    second->Post(co_await ReceiveMessage(socket, std::chrono::seconds(30)));
  }(server_socket, &first, &second);

  auto expired = first.Wait();
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().code(), "TimeoutError");

  ASSERT_TRUE(client_socket->Send(RawPing(5)).ok());
  auto arrived = second.Wait();
  ASSERT_TRUE(arrived.ok() && arrived->has_value());
  EXPECT_EQ((*arrived)->payload.ToString(), "5");
}

TEST(AsyncEventStreamTest, AwaitedSendBackpressuresWithoutAThread) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  std::atomic<int> sent_count{0};
  std::atomic<bool> done{false};

  // A pusher loop: fires kWireDepth + 2 sends; the wire bound parks it
  // after kWireDepth — with no thread blocked anywhere.
  [](std::shared_ptr<http::WebSocket> socket, std::atomic<int>* sent_count,
     std::atomic<bool>* done) -> Detached {
    AsyncServer stream(std::move(socket), EncodePong, DecodePing);
    for (std::size_t i = 0; i < kWireDepth + 2; ++i) {
      auto sent = co_await stream.Send(Pong{"p-" + std::to_string(i)});
      if (!sent.ok()) break;
      sent_count->fetch_add(1);
    }
    *done = true;
  }(server_socket, &sent_count, &done);

  EXPECT_EQ(sent_count.load(), static_cast<int>(kWireDepth));  // parked at the bound
  EXPECT_FALSE(done.load());

  // Draining resumes the coroutine on this thread; FIFO order holds.
  EventStream<Ping, Pong> client(client_socket, EncodePing, DecodePong);
  for (std::size_t i = 0; i < kWireDepth + 2; ++i) {
    auto pong = client.Receive();
    ASSERT_TRUE(pong.ok() && pong->has_value());
    EXPECT_EQ((*pong)->text, "p-" + std::to_string(i));
  }
  EXPECT_TRUE(done.load());
}

TEST(AsyncEventStreamTest, AnUndecodableMessageIsTerminalThroughTheAdapter) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Ping>>> received;

  [](std::shared_ptr<http::WebSocket> socket,
     Mailbox<Outcome<std::optional<Ping>>>* received) -> Detached {
    AsyncServer stream(std::move(socket), EncodePong, DecodePing);
    auto ping = co_await stream.Receive();
    received->Post(std::move(ping));
  }(server_socket, &received);

  ASSERT_TRUE(client_socket->Send(Message{.headers = {{":event-type", "not-a-ping"}}}).ok());
  auto outcome = received.Wait();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kSerialization);
  // Terminal, like EventStream (ADR-0016): the adapter closed the session.
  auto closed = client_socket->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());
}

TEST(AsyncEventStreamTest, AnEncoderFailureSurfacesWithoutSuspendingOrEndingTheSession) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<Unit>> first;
  Mailbox<Outcome<Unit>> second;

  [](std::shared_ptr<http::WebSocket> socket, Mailbox<Outcome<Unit>>* first,
     Mailbox<Outcome<Unit>>* second) -> Detached {
    AsyncEventStream<Ping, Pong> stream(
        std::move(socket),
        [](const Ping& ping) -> Outcome<Message> {
          if (ping.number < 0) return Error::Validation("negative ping");
          return EncodePing(ping);
        },
        DecodePong);
    first->Post(co_await stream.Send(Ping{-1}));
    second->Post(co_await stream.Send(Ping{5}));
  }(client_socket, &first, &second);

  auto refused = first.Wait();
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);
  EXPECT_TRUE(second.Wait().ok());  // the session was never touched
  auto at_peer = server_socket->Receive();
  ASSERT_TRUE(at_peer.ok() && at_peer->has_value());
  EXPECT_EQ((*at_peer)->payload.ToString(), "5");
}

TEST(AsyncEventStreamTest, SharedHandlesOutliveTheLoopAndItsStream) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<EventStreamHandle<Pong>> minted;
  std::atomic<bool> done{false};

  [](std::shared_ptr<http::WebSocket> socket, Mailbox<EventStreamHandle<Pong>>* minted,
     std::atomic<bool>* done) -> Detached {
    AsyncServer stream(std::move(socket), EncodePong, DecodePing);
    minted->Post(stream.Share());
    (void)co_await stream.Receive();  // parked until the client closes
    *done = true;
  }(server_socket, &minted, &done);

  EventStreamHandle<Pong> handle = minted.Wait();
  ASSERT_TRUE(handle.Send(Pong{"from-outside-the-loop"}).ok());
  auto at_client = client_socket->Receive();
  ASSERT_TRUE(at_client.ok() && at_client->has_value());
  EXPECT_EQ((*at_client)->payload.ToString(), "from-outside-the-loop");

  // The loop ends, its frame — and the stream inside it — die, and the
  // handle degrades to the stale-handle contract instead of dangling.
  client_socket->Close();
  for (int i = 0; i < 100 && !done.load(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(done.load());
  const auto stale = handle.Send(Pong{"too-late"});
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().kind(), ErrorKind::kTransport);
}

TEST(AsyncEventStreamTest, HandleSendAsyncCompletesAndFailsSoftlyAfterRevocation) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<Unit>> live;
  Mailbox<Outcome<Unit>> stale;
  std::optional<EventStreamHandle<Pong>> handle;
  {
    AsyncServer stream(server_socket, EncodePong, DecodePing);
    handle = stream.Share();
    handle->SendAsync(Pong{"async"}, [&](Outcome<Unit> sent) { live.Post(std::move(sent)); });
    EXPECT_TRUE(live.Wait().ok());
    auto at_client = client_socket->Receive();
    ASSERT_TRUE(at_client.ok() && at_client->has_value());
  }  // ~AsyncEventStream revokes: close, drain pins, null the socket

  handle->SendAsync(Pong{"late"}, [&](Outcome<Unit> sent) { stale.Post(std::move(sent)); });
  auto dead = stale.Wait();
  ASSERT_FALSE(dead.ok());
  EXPECT_EQ(dead.error().kind(), ErrorKind::kTransport);
}

TEST(AsyncEventStreamTest, DestructionCompletesAParkedHandleSendWithoutHanging) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<Unit>> parked;
  {
    AsyncServer stream(server_socket, EncodePong, DecodePing);
    auto handle = stream.Share();
    for (std::size_t i = 0; i < kWireDepth; ++i) {
      ASSERT_TRUE(handle.Send(Pong{"fill"}).ok());
    }
    handle.SendAsync(Pong{"parked"}, [&](Outcome<Unit> sent) { parked.Post(std::move(sent)); });
    ASSERT_TRUE(parked.Empty());  // in flight behind the full wire
    // ~AsyncEventStream now: the revocation pin spans issue to completion
    // (ADR-0017), so the destructor's drain must complete the parked send
    // — a missing pin release here is a deadlock, not a leak.
  }
  auto dead = parked.Wait();
  ASSERT_FALSE(dead.ok());
  EXPECT_EQ(dead.error().kind(), ErrorKind::kTransport);
}

TEST(AsyncEventStreamTest, AHandleEncoderFailureCompletesInlineAndSparesTheSession) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  AsyncEventStream<Ping, Pong> stream(
      server_socket,
      [](const Ping& ping) -> Outcome<Message> {
        if (ping.number < 0) return Error::Validation("negative ping");
        return EncodePing(ping);
      },
      DecodePong);
  auto handle = stream.Share();

  Mailbox<Outcome<Unit>> refused;
  handle.SendAsync(Ping{-1}, [&](Outcome<Unit> sent) { refused.Post(std::move(sent)); });
  auto outcome = refused.Wait();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kValidation);

  ASSERT_TRUE(handle.Send(Ping{5}).ok());  // the session was never touched
  auto at_client = client_socket->Receive();
  ASSERT_TRUE(at_client.ok() && at_client->has_value());
  EXPECT_EQ((*at_client)->payload.ToString(), "5");
}

TEST(AsyncEventStreamTest, AReadyMessageResumesTheReceiveWithoutSuspending) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ASSERT_TRUE(client_socket->Send(RawPing(5)).ok());  // queued before the loop exists

  // The awaitable's synchronous path: the pair completes a ready receive
  // inline, so await_suspend never suspends — by the time the launch
  // returns, the loop has consumed the message on this thread.
  std::atomic<bool> got{false};
  [](std::shared_ptr<http::WebSocket> socket, std::atomic<bool>* got) -> Detached {
    AsyncServer stream(std::move(socket), EncodePong, DecodePing);
    auto ping = co_await stream.Receive();
    *got = ping.ok() && ping->has_value() && (*ping)->number == 5;
  }(server_socket, &got);
  EXPECT_TRUE(got.load());
}

TEST(AsyncEventStreamTest, ADetachedLoopContainsItsExceptions) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  // The loop throws after a resumed co_await — on the completion context,
  // where an escaping exception would take down a wire thread. Detached's
  // promise contains it to a log line; surviving the Send IS the assert.
  [](std::shared_ptr<http::WebSocket> socket) -> Detached {
    AsyncServer stream(std::move(socket), EncodePong, DecodePing);
    (void)co_await stream.Receive();
    throw std::runtime_error("handler bug");
  }(server_socket);

  ASSERT_TRUE(client_socket->Send(RawPing(1)).ok());  // resumes the loop; it throws, contained
  client_socket->Close();
}

// ---------------------------------------------------------------------------
// StreamTask (ADR-0021): the generated async handler's return shape.
// ---------------------------------------------------------------------------

// A Detached driver awaiting one task into a mailbox — the generated
// wrapper's skeleton.
Detached AwaitInto(StreamTask task, Mailbox<Outcome<Unit>>& outcome) {
  outcome.Post(co_await task);
}

TEST(StreamTaskTest, IsLazyAndDeliversTheHandlersOutcome) {
  std::atomic<bool> started{false};
  auto handler = [&started]() -> StreamTask {
    started = true;
    co_return Unit{};
  };
  StreamTask task = handler();
  EXPECT_FALSE(started.load());  // lazy: nothing runs before the await

  Mailbox<Outcome<Unit>> outcome;
  AwaitInto(std::move(task), outcome);
  EXPECT_TRUE(started.load());
  EXPECT_TRUE(outcome.Wait().ok());
}

TEST(StreamTaskTest, ATypedErrorRidesTheOutcomeToTheAwaiter) {
  auto handler = []() -> StreamTask { co_return Error::Validation("seat taken"); };
  Mailbox<Outcome<Unit>> outcome;
  AwaitInto(handler(), outcome);
  auto result = outcome.Wait();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().kind(), ErrorKind::kValidation);
  EXPECT_EQ(result.error().message(), "seat taken");
}

TEST(StreamTaskTest, AThrowingHandlerCompletesWithErrorUnknownNotTerminate) {
  auto handler = []() -> StreamTask {
    throw std::runtime_error("handler bug");
    co_return Unit{};  // unreachable; makes the lambda a coroutine
  };
  Mailbox<Outcome<Unit>> outcome;
  AwaitInto(handler(), outcome);
  auto result = outcome.Wait();
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().kind(), ErrorKind::kUnknown);
  EXPECT_NE(result.error().message().find("handler bug"), std::string::npos);
}

TEST(StreamTaskTest, ANeverAwaitedTaskDestroysItsFrameCleanly) {
  std::atomic<bool> started{false};
  {
    auto handler = [&started]() -> StreamTask {
      started = true;
      co_return Unit{};
    };
    StreamTask task = handler();  // dropped without an await
  }
  EXPECT_FALSE(started.load());  // never ran, and (under ASan) never leaked
}

TEST(StreamTaskTest, SubTasksComposeAcrossARealSuspension) {
  // Handlers may factor their logic into StreamTask-returning helpers and
  // co_await each exactly once (results beyond the Outcome travel by
  // out-parameter). Pinned: a sub-task that completes inline and one that
  // parks in Receive, both resuming through two task layers.
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  auto inner = [](AsyncEventStream<Pong, Ping>& stream) -> StreamTask {
    auto ping = co_await stream.Receive();
    if (!ping.ok() || !ping->has_value()) co_return Error::Unknown("no ping");
    co_return Unit{};
  };
  auto outer = [&inner](std::shared_ptr<http::WebSocket> socket) -> StreamTask {
    AsyncEventStream<Pong, Ping> stream(std::move(socket), EncodePong, DecodePing);
    auto first = co_await []() -> StreamTask { co_return Unit{}; }();
    if (!first.ok()) co_return first;
    co_return co_await inner(stream);
  };
  Mailbox<Outcome<Unit>> outcome;
  AwaitInto(outer(server_socket), outcome);
  EXPECT_TRUE(outcome.Empty());  // parked in Receive, two task frames deep
  ASSERT_TRUE(client_socket->Send(RawPing(1)).ok());
  EXPECT_TRUE(outcome.Wait().ok());
  client_socket->Close();
}

// ---------------------------------------------------------------------------
// ReceiveMessage (ADR-0023): the session route's first-message read.
// ---------------------------------------------------------------------------

// The generated jsonRpc2 session route's skeleton: one raw message awaited
// before any stream exists.
Detached ReceiveOneInto(std::shared_ptr<http::WebSocket> socket,
                        Mailbox<Outcome<std::optional<Message>>>& received) {
  received.Post(co_await ReceiveMessage(std::move(socket)));
}

TEST(ReceiveMessageTest, AwaitsOneRawMessageBeforeAnyStreamExists) {
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> received;
  ReceiveOneInto(server_socket, received);
  EXPECT_TRUE(received.Empty());  // parked: the opening envelope is not here yet

  ASSERT_TRUE(client_socket->Send(RawPing(7)).ok());
  auto outcome = received.Wait();
  ASSERT_TRUE(outcome.ok());
  ASSERT_TRUE(outcome->has_value());
  EXPECT_EQ(**outcome, RawPing(7));  // raw: no decode, no stream semantics
}

TEST(ReceiveMessageTest, CompletesWhenAMessageAlreadyWaits) {
  // The pair completes inline — the second-arrival race's synchronous arm,
  // where await_suspend never suspends.
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  ASSERT_TRUE(client_socket->Send(RawPing(1)).ok());
  Mailbox<Outcome<std::optional<Message>>> received;
  ReceiveOneInto(server_socket, received);
  auto outcome = received.Wait();
  ASSERT_TRUE(outcome.ok() && outcome->has_value());
}

TEST(ReceiveMessageTest, ObservesThePeersCleanCloseAsNullopt) {
  // A peer that connects and closes without an opening envelope is a
  // non-event for the route: nullopt, not an error — WebSocket::Receive's
  // contract, untouched.
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  Mailbox<Outcome<std::optional<Message>>> received;
  ReceiveOneInto(server_socket, received);
  client_socket->Close();
  auto outcome = received.Wait();
  ASSERT_TRUE(outcome.ok());
  EXPECT_FALSE(outcome->has_value());
}

TEST(StreamTaskTest, ResumesTheAwaiterAfterARealSuspensionOnTheCompletionThread) {
  // The production shape: the handler parks in co_await Receive, the peer
  // completes it later, and the completion resumes the handler and then —
  // by symmetric transfer at its end — the wrapper.
  auto [client_socket, server_socket] = http::InMemoryWebSocketPair::Create();
  auto handler = [](std::shared_ptr<http::WebSocket> socket) -> StreamTask {
    AsyncEventStream<Pong, Ping> stream(std::move(socket), EncodePong, DecodePing);
    auto ping = co_await stream.Receive();
    if (!ping.ok() || !ping->has_value()) co_return Error::Unknown("no ping");
    co_return Unit{};
  };
  Mailbox<Outcome<Unit>> outcome;
  AwaitInto(handler(server_socket), outcome);
  EXPECT_TRUE(outcome.Empty());  // parked in Receive; the wrapper is suspended
  ASSERT_TRUE(client_socket->Send(RawPing(7)).ok());
  EXPECT_TRUE(outcome.Wait().ok());
  client_socket->Close();
}

// ---------------------------------------------------------------------------
// The shared WebSocket contract (websocket_contract_test.h), pair half.
// ---------------------------------------------------------------------------

// The pair's driver: the server end is under test and the client end never
// receives, so the direction's queue fills and the next send parks. The
// pair's Close runs the whole teardown on the calling thread, which is
// exactly the shape the suite's ender thread exists for.
struct PairContractDriver {
  // kQueueDepth fills the wire; the margin covers the parking send itself.
  static constexpr int kWedgeAttempts = static_cast<int>(kWireDepth) + 4;

  std::shared_ptr<http::WebSocket> Socket() { return server_; }

  Message BulkMessage(int n) {
    // Nothing has to be big here: the bound is the queue's depth, not bytes.
    return RawPing(n);
  }

  void EndSessionFromPeer() { client_->Close(); }

  std::shared_ptr<http::WebSocket> client_;
  std::shared_ptr<http::WebSocket> server_;

  PairContractDriver() { std::tie(client_, server_) = http::InMemoryWebSocketPair::Create(); }
};

}  // namespace
}  // namespace smithy::eventstream

// The instantiation lives where the suite was registered: gtest builds the
// registration symbols from the bare suite name, so a qualified one does
// not resolve.
namespace smithy::testing {
INSTANTIATE_TYPED_TEST_SUITE_P(InMemoryPair, WebSocketContractTest,
                               eventstream::PairContractDriver);
}  // namespace smithy::testing
