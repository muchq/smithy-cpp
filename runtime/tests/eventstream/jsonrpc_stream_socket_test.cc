// JsonRpcStreamSocket over the in-memory pair (ADR-0023): the translation
// wrapper runs ABOVE the transport, so unlike ADR-0018's negotiated mode
// the pair exercises the actual JSON-RPC text — one end unwrapped IS the
// wire view. Covers the event round trip through both facades (blocking
// and async), the terminal-response classification (result = clean end,
// error = exception message), the exception-send refusal, and the
// per-role violation policing: both ends fail closed on malformed and
// mis-framed inbound traffic, and the server end answers the reserved-code
// terminal for the opening id before its close.

#include "smithy/eventstream/jsonrpc_stream_socket.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <utility>

#include "smithy/core/document.h"
#include "smithy/eventstream/envelope.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket_pair.h"
#include "smithy/testing/websocket_contract_test.h"

namespace smithy::eventstream {
namespace {

// A headerless raw-text message: what the wire (and the unwrapped pair
// end) carries — the payload is one JSON-RPC envelope.
Message Raw(std::string text) {
  Message message;
  message.payload = Blob::FromString(std::move(text));
  return message;
}

std::shared_ptr<JsonRpcStreamSocket> Wrap(std::shared_ptr<http::WebSocket> inner,
                                          JsonRpcStreamSocket::Role role) {
  return std::make_shared<JsonRpcStreamSocket>(std::move(inner), Document(1), role);
}

TEST(JsonRpcStreamSocketTest, EventsRoundTripBetweenTwoWrappedEnds) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left, JsonRpcStreamSocket::Role::kClient);
  auto server = Wrap(right, JsonRpcStreamSocket::Role::kServer);

  const Message event =
      MakeEventMessage("message", "application/json", Blob::FromString(R"({"text":"hi"})"));
  ASSERT_TRUE(client->Send(event).ok());
  auto received = server->Receive();
  ASSERT_TRUE(received.ok()) << received.error().message();
  ASSERT_TRUE(received->has_value());
  EXPECT_EQ(**received, event);

  // Full duplex: the same translation runs the other way.
  ASSERT_TRUE(server->Send(event).ok());
  auto echoed = client->Receive();
  ASSERT_TRUE(echoed.ok());
  ASSERT_TRUE(echoed->has_value());
  EXPECT_EQ(**echoed, event);
}

TEST(JsonRpcStreamSocketTest, TheWireCarriesThePinnedNotificationText) {
  // One end unwrapped: what it receives is the wire — a headerless
  // message whose payload is the notification text, byte-pinned.
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left, JsonRpcStreamSocket::Role::kClient);

  ASSERT_TRUE(
      client->Send(MakeEventMessage("message", "application/json", Blob::FromString(R"({"n":1})")))
          .ok());
  auto raw = right->Receive();
  ASSERT_TRUE(raw.ok());
  ASSERT_TRUE(raw->has_value());
  EXPECT_TRUE((**raw).headers.empty());
  EXPECT_EQ((**raw).payload.ToString(),
            R"({"jsonrpc":"2.0","method":"message","params":{"id":1,"payload":{"n":1}}})");
}

TEST(JsonRpcStreamSocketTest, TheTerminalResultIsTheCleanEnd) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left, JsonRpcStreamSocket::Role::kClient);

  ASSERT_TRUE(right->Send(Raw(R"({"jsonrpc":"2.0","result":{},"id":1})")).ok());
  auto received = client->Receive();
  ASSERT_TRUE(received.ok()) << received.error().message();
  // The terminal result never surfaces as a message — the stream ends the
  // way a peer close ends it, and the server's close follows right behind.
  EXPECT_FALSE(received->has_value());
}

TEST(JsonRpcStreamSocketTest, TheTerminalErrorArrivesAsTheExceptionMessage) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left, JsonRpcStreamSocket::Role::kClient);

  ASSERT_TRUE(right
                  ->Send(Raw(R"({"jsonrpc":"2.0","error":{"code":409,"message":"kicked",)"
                             R"("data":{"__type":"Kicked","by":"mod"}},"id":1})"))
                  .ok());
  auto received = client->Receive();
  ASSERT_TRUE(received.ok());
  ASSERT_TRUE(received->has_value());
  // The exception Message shape ADR-0016's downstream contract expects;
  // the __type spelling rides the wire verbatim (this peer sent the short
  // name, generated jsonRpc2 terminals stamp the fully-qualified shape ID
  // — SanitizeErrorCode in the generated decoders resolves either).
  ASSERT_NE((**received).FindString(":exception-type"), nullptr);
  EXPECT_EQ(*(**received).FindString(":exception-type"), "Kicked");
  EXPECT_EQ((**received).payload.ToString(),
            R"({"__type":"Kicked","by":"mod","message":"kicked"})");
}

TEST(JsonRpcStreamSocketTest, ExceptionSendsAreRefusedAndTheSessionSurvives) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto server = Wrap(right, JsonRpcStreamSocket::Role::kServer);

  auto refused = server->Send(
      MakeExceptionMessage("Kicked", "application/json", Blob::FromString(R"({"by":"mod"})")));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);
  // The refusal is the Send contract's Validation: session untouched —
  // the terminal error envelope is sent raw by the generated serve path.
  EXPECT_TRUE(server->Send(MakeEventMessage("ping", "", Blob::FromString("{}"))).ok());
}

TEST(JsonRpcStreamSocketTest, MalformedInboundTextIsSerializationTerminal) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left, JsonRpcStreamSocket::Role::kClient);

  ASSERT_TRUE(right->Send(Raw("not json")).ok());
  auto received = client->Receive();
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().kind(), ErrorKind::kSerialization);
  // The client end fails closed and answers nothing — terminal envelopes
  // are server-minted, so the peer just sees the close.
  auto peer = right->Receive();
  ASSERT_TRUE(peer.ok());
  EXPECT_FALSE(peer->has_value());
}

TEST(JsonRpcStreamSocketTest, EnvelopeFramedInboundMessagesAreRefused) {
  // A peer that speaks the eventstream envelope wire into a JSON-RPC
  // stream (a mis-wired test, a wrong-mode transport) is a protocol
  // violation, not a message.
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left, JsonRpcStreamSocket::Role::kClient);

  ASSERT_TRUE(
      right->Send(MakeEventMessage("ping", "application/json", Blob::FromString("{}"))).ok());
  auto received = client->Receive();
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().kind(), ErrorKind::kSerialization);
  auto peer = right->Receive();
  ASSERT_TRUE(peer.ok());
  EXPECT_FALSE(peer->has_value());  // fail-closed, no terminal from a client
}

TEST(JsonRpcStreamSocketTest, TheServerRoleAnswersAViolationThenCloses) {
  // The server end's half of the policing contract: the reserved-code
  // terminal error for the opening id goes out BEFORE the close, so a
  // conforming peer learns why the stream died.
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto server = Wrap(right, JsonRpcStreamSocket::Role::kServer);

  ASSERT_TRUE(left->Send(Raw("not json")).ok());
  auto received = server->Receive();
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().kind(), ErrorKind::kSerialization);

  auto terminal = left->Receive();
  ASSERT_TRUE(terminal.ok());
  ASSERT_TRUE(terminal->has_value());
  EXPECT_TRUE((**terminal).headers.empty());
  EXPECT_EQ((**terminal).payload.ToString(),
            R"({"error":{"code":-32700,"data":{"__type":"SerializationException"},)"
            R"("message":"text frame is not JSON"},"id":1,"jsonrpc":"2.0"})");
  auto closed = left->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());  // nothing follows the terminal
}

TEST(JsonRpcStreamSocketTest, TheServerRoleRefusesAClientResponseEnvelope) {
  // Terminal response envelopes are server-minted: a client sending one
  // must not read as the peer's clean close (or as its terminal error).
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto server = Wrap(right, JsonRpcStreamSocket::Role::kServer);

  ASSERT_TRUE(left->Send(Raw(R"({"jsonrpc":"2.0","result":{},"id":1})")).ok());
  auto received = server->Receive();
  ASSERT_FALSE(received.ok());
  EXPECT_EQ(received.error().kind(), ErrorKind::kSerialization);
  EXPECT_NE(received.error().message().find("a response envelope from the client"),
            std::string::npos);

  auto terminal = left->Receive();
  ASSERT_TRUE(terminal.ok());
  ASSERT_TRUE(terminal->has_value());
  EXPECT_EQ((**terminal).payload.ToString(),
            R"({"error":{"code":-32600,"data":{"__type":"SerializationException"},)"
            R"("message":"a response envelope from the client"},"id":1,"jsonrpc":"2.0"})");
  auto closed = left->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());
}

TEST(JsonRpcStreamSocketTest, TheServerRoleAnswersAViolationOnTheAsyncPathToo) {
  // The async twin polices identically, and the close rides the terminal
  // send's completion — the write cannot be cancelled by the close (the
  // ADR-0021 ordering lesson).
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto server = Wrap(right, JsonRpcStreamSocket::Role::kServer);

  ASSERT_TRUE(left->Send(Raw(R"({"jsonrpc":"2.0","method":"","params":{}})")).ok());
  bool observed = false;
  server->ReceiveAsync([&observed](Outcome<std::optional<Message>> received) {
    ASSERT_FALSE(received.ok());
    EXPECT_EQ(received.error().kind(), ErrorKind::kSerialization);
    observed = true;
  });
  EXPECT_TRUE(observed);  // the pair completes inline

  auto terminal = left->Receive();
  ASSERT_TRUE(terminal.ok());
  ASSERT_TRUE(terminal->has_value());
  EXPECT_EQ((**terminal).payload.ToString(),
            R"({"error":{"code":-32600,"data":{"__type":"SerializationException"},)"
            R"("message":"\"method\" is not a non-empty string"},"id":1,"jsonrpc":"2.0"})");
  auto closed = left->Receive();
  ASSERT_TRUE(closed.ok());
  EXPECT_FALSE(closed->has_value());
}

TEST(JsonRpcStreamSocketTest, TheAsyncTwinsTranslateTheSameWay) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left, JsonRpcStreamSocket::Role::kClient);
  auto server = Wrap(right, JsonRpcStreamSocket::Role::kServer);
  ASSERT_TRUE(client->SupportsAsync());

  // The pair completes inline; the assertions run before the calls return.
  bool sent = false;
  client->SendAsync(MakeEventMessage("message", "application/json", Blob::FromString(R"({"n":1})")),
                    [&sent](Outcome<Unit> outcome) {
                      EXPECT_TRUE(outcome.ok()) << outcome.error().message();
                      sent = true;
                    });
  EXPECT_TRUE(sent);

  bool received = false;
  server->ReceiveAsync([&received](Outcome<std::optional<Message>> message) {
    ASSERT_TRUE(message.ok()) << message.error().message();
    ASSERT_TRUE(message->has_value());
    ASSERT_NE((**message).FindString(":event-type"), nullptr);
    EXPECT_EQ(*(**message).FindString(":event-type"), "message");
    received = true;
  });
  EXPECT_TRUE(received);

  // Terminal classification runs on the async path too.
  ASSERT_TRUE(right->Send(Raw(R"({"jsonrpc":"2.0","result":{},"id":1})")).ok());
  bool ended = false;
  client->ReceiveAsync([&ended](Outcome<std::optional<Message>> message) {
    ASSERT_TRUE(message.ok());
    EXPECT_FALSE(message->has_value());
    ended = true;
  });
  EXPECT_TRUE(ended);
}

TEST(JsonRpcStreamSocketTest, AnAsyncEncodeRefusalCompletesInline) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left, JsonRpcStreamSocket::Role::kClient);

  bool refused = false;
  client->SendAsync(MakeExceptionMessage("Kicked", "", Blob::FromString("{}")),
                    [&refused](Outcome<Unit> outcome) {
                      ASSERT_FALSE(outcome.ok());
                      EXPECT_EQ(outcome.error().kind(), ErrorKind::kValidation);
                      refused = true;
                    });
  EXPECT_TRUE(refused);
}

TEST(JsonRpcStreamSocketTest, TheBorrowingFormTranslatesLikeTheOwningOne) {
  // The blocking serve seam's shape: the route borrows its socket, so the
  // wrapper borrows too (EventStream's borrowed-constructor mirror).
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  JsonRpcStreamSocket client(*left, Document(1), JsonRpcStreamSocket::Role::kClient);

  const Message event = MakeEventMessage("ping", "application/json", Blob::FromString("{}"));
  ASSERT_TRUE(client.Send(event).ok());
  auto raw = right->Receive();
  ASSERT_TRUE(raw.ok() && raw->has_value());
  EXPECT_EQ((**raw).payload.ToString(),
            R"({"jsonrpc":"2.0","method":"ping","params":{"id":1,"payload":{}}})");

  ASSERT_TRUE(right->Send(Raw(R"({"jsonrpc":"2.0","result":{},"id":1})")).ok());
  auto received = client.Receive();
  ASSERT_TRUE(received.ok());
  EXPECT_FALSE(received->has_value());
}

TEST(JsonRpcStreamSocketTest, ADeadlineDelegatesAndATimeoutIsNotAViolation) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left, JsonRpcStreamSocket::Role::kClient);
  auto server = Wrap(right, JsonRpcStreamSocket::Role::kServer);

  auto nothing = server->Receive(std::chrono::milliseconds(50));
  ASSERT_FALSE(nothing.ok());
  EXPECT_EQ(nothing.error().code(), "TimeoutError");

  // Nothing was policed and nothing was closed — an envelope violation
  // ends the session, a deadline does not.
  const Message event =
      MakeEventMessage("message", "application/json", Blob::FromString(R"({"text":"late"})"));
  ASSERT_TRUE(client->Send(event).ok());
  auto received = server->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(received.ok()) << received.error().message();
  ASSERT_TRUE(received->has_value());
  EXPECT_EQ(**received, event);
}

TEST(JsonRpcStreamSocketTest, TheAsyncDeadlineDelegatesAndATimeoutIsNotAViolation) {
  // The #130 twin of the blocking case above: the timeout rides through
  // the classifier untouched, and the envelope that arrives after it is
  // policed for the next receive exactly as if no deadline had passed.
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left, JsonRpcStreamSocket::Role::kClient);
  auto server = Wrap(right, JsonRpcStreamSocket::Role::kServer);

  std::promise<Outcome<std::optional<Message>>> timed;
  server->ReceiveAsync(std::chrono::milliseconds(50), [&timed](Outcome<std::optional<Message>> m) {
    timed.set_value(std::move(m));
  });
  auto timed_future = timed.get_future();
  ASSERT_EQ(timed_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  auto nothing = timed_future.get();
  ASSERT_FALSE(nothing.ok());
  EXPECT_EQ(nothing.error().code(), "TimeoutError");

  const Message event =
      MakeEventMessage("message", "application/json", Blob::FromString(R"({"text":"late"})"));
  ASSERT_TRUE(client->Send(event).ok());
  std::promise<Outcome<std::optional<Message>>> next;
  server->ReceiveAsync(std::chrono::seconds(30), [&next](Outcome<std::optional<Message>> m) {
    next.set_value(std::move(m));
  });
  auto next_future = next.get_future();
  ASSERT_EQ(next_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  auto received = next_future.get();
  ASSERT_TRUE(received.ok()) << received.error().message();
  ASSERT_TRUE(received->has_value());
  EXPECT_EQ(**received, event);
}

TEST(JsonRpcStreamSocketTest, CloseAndThePeersCleanCloseDelegate) {
  auto [left, right] = http::InMemoryWebSocketPair::Create();
  auto client = Wrap(left, JsonRpcStreamSocket::Role::kClient);
  auto server = Wrap(right, JsonRpcStreamSocket::Role::kServer);

  client->Close();
  auto received = server->Receive();
  ASSERT_TRUE(received.ok());
  EXPECT_FALSE(received->has_value());  // the peer's clean close, untranslated
}

// ---------------------------------------------------------------------------
// The shared WebSocket contract (websocket_contract_test.h), decorator half.
// ---------------------------------------------------------------------------

// This wrapper parks nothing of its own — both async twins forward to the
// socket underneath — which is why the #173 terminal-ordering fix had
// nothing to change here. That was a claim about this file's code, checked
// by reading; this instantiation is what keeps it true. If the wrapper ever
// grows slots of its own, the ordering rule starts applying to it, and the
// suite says so instead of the next reader having to notice.
struct JsonRpcContractDriver {
  static constexpr int kWedgeAttempts =
      static_cast<int>(http::InMemoryWebSocketPair::kQueueDepth) + 4;

  JsonRpcContractDriver() {
    auto [peer, wrapped] = http::InMemoryWebSocketPair::Create();
    peer_ = peer;  // never receives, so the wire behind the wrapper wedges
    socket_ = Wrap(wrapped, JsonRpcStreamSocket::Role::kServer);
  }

  std::shared_ptr<http::WebSocket> Socket() { return socket_; }

  Message BulkMessage(int n) {
    return MakeEventMessage("message", "application/json",
                            Blob::FromString(R"({"n":)" + std::to_string(n) + "}"));
  }

  void EndSessionFromPeer() { peer_->Close(); }

  std::shared_ptr<http::WebSocket> peer_;
  std::shared_ptr<JsonRpcStreamSocket> socket_;
};

}  // namespace
}  // namespace smithy::eventstream

// gtest builds the registration symbols from the bare suite name, so the
// instantiation lives in the namespace the suite was registered in.
namespace smithy::testing {
INSTANTIATE_TYPED_TEST_SUITE_P(JsonRpcStreamSocket, WebSocketContractTest,
                               eventstream::JsonRpcContractDriver);
}  // namespace smithy::testing
