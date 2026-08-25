// Pins ADR-0015's WebSocket transports end to end over real loopback
// sockets: the upgrade path (gate refusals, handshake failures, plain-HTTP
// coexistence), the session contract (full duplex, backpressure,
// close-vs-error surfaces, protocol violations), TLS, and Stop() semantics
// with serve callbacks blocked mid-Receive.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/websocket.hpp>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "smithy/eventstream/async_event_stream.h"
#include "smithy/eventstream/envelope.h"
#include "smithy/eventstream/json_frame.h"
#include "smithy/http/beast_transport.h"
#include "smithy/http/websocket.h"
#include "smithy/testing/connection_event_recorder.h"
#include "smithy/testing/tls_test_identity.h"
#include "smithy/testing/websocket_contract_test.h"

namespace smithy::http {
namespace {

using eventstream::Message;
using smithy::testing::ConnectionEventRecorder;

Message Text(const std::string& kind, const std::string& body) {
  return Message{.headers = {{":event-type", kind}}, .payload = Blob::FromString(body)};
}

HttpResponse PlainResponse(int status, const std::string& body) {
  HttpResponse response;
  response.status = status;
  response.body = body;
  return response;
}

// A server whose serve loop echoes every message back with an "echo:"
// prefix on the payload, then closes when the client does.
BeastServerTransport::Options EchoOptions() {
  BeastServerTransport::Options options;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) {
        return;
      }
      Message reply = **message;
      reply.payload = Blob::FromString("echo:" + (*message)->payload.ToString());
      if (!socket.Send(reply).ok()) {
        return;
      }
    }
  };
  return options;
}

RequestHandler NotFoundHandler() {
  return [](const HttpRequest&) { return PlainResponse(404, "no such route"); };
}

TEST(BeastWebSocketTest, MessagesRoundTripBothWaysOverTheUpgrade) {
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  for (int i = 0; i < 10; ++i) {
    const Message sent = Text("chat", "message-" + std::to_string(i));
    ASSERT_TRUE(socket->Send(sent).ok());
    auto received = socket->Receive();
    ASSERT_TRUE(received.ok()) << received.error().message();
    ASSERT_TRUE(received->has_value());
    EXPECT_EQ((**received).headers, sent.headers);
    EXPECT_EQ((**received).payload.ToString(), "echo:message-" + std::to_string(i));
  }

  // The client's close surfaces server-side as Receive's nullopt (the echo
  // loop returns), and the acknowledging close ends the client cleanly.
  socket->Close();
  auto after_close = socket->Receive();
  ASSERT_TRUE(after_close.ok()) << after_close.error().message();
  EXPECT_FALSE(after_close->has_value());
  server.Stop();
}

TEST(BeastWebSocketTest, AReceiveDeadlineExpiresOnAQuietWireAndSparesTheSession) {
  // The echo server says nothing unsolicited, so a receive with no deadline
  // would park here forever — the hang this overload exists to prevent.
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  const auto started = std::chrono::steady_clock::now();
  auto nothing = socket->Receive(std::chrono::milliseconds(75));
  const auto waited = std::chrono::steady_clock::now() - started;
  ASSERT_FALSE(nothing.ok());
  EXPECT_EQ(nothing.error().code(), "TimeoutError");
  EXPECT_EQ(nothing.error().kind(), ErrorKind::kTransport);
  EXPECT_GE(waited, std::chrono::milliseconds(60));  // it really waited (loose: see the pair's)

  // The wire is untouched: the same session still round-trips, and the
  // read pump kept its place (nothing was consumed or dropped).
  ASSERT_TRUE(socket->Send(Text("chat", "after the deadline")).ok());
  auto echo = socket->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(echo.ok()) << echo.error().message();
  ASSERT_TRUE(echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:after the deadline");

  // And the close still reads as the clean end, never as a deadline.
  socket->Close();
  auto end = socket->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
  server.Stop();
}

TEST(BeastWebSocketTest, ATimedOutReceiveReleasesTheOneOutstandingSlotOnTheWire) {
  // The pair pins the same rule; the Beast session counts its blocked
  // receivers in its own code, so the wire gets its own proof: a receive
  // that gave up is not still holding the session's one receive slot.
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  auto nothing = socket->Receive(std::chrono::milliseconds(50));
  ASSERT_FALSE(nothing.ok());
  EXPECT_EQ(nothing.error().code(), "TimeoutError");

  // The async twin arms instead of refusing with "a receive is already
  // outstanding" — which is what a leaked slot would produce.
  std::promise<Outcome<std::optional<Message>>> armed;
  socket->ReceiveAsync(
      [&armed](Outcome<std::optional<Message>> message) { armed.set_value(std::move(message)); });
  ASSERT_TRUE(socket->Send(Text("chat", "for the parked receive")).ok());
  auto future = armed.get_future();
  ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  auto delivered = future.get();
  ASSERT_TRUE(delivered.ok()) << delivered.error().message();
  ASSERT_TRUE(delivered->has_value());
  EXPECT_EQ((**delivered).payload.ToString(), "echo:for the parked receive");

  socket->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, AnAsyncReceiveDeadlineExpiresAndDeliveryStillBeatsALaterOne) {
  // The #130 async twin over the real wire: the deadline timer lives on
  // the connection's executor, so the timer/delivery race the pair pins in
  // memory gets its own proof against Beast's io machinery.
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  // Quiet wire: the parked timed receive completes with the timeout, on
  // the executor, and the session is untouched.
  std::promise<Outcome<std::optional<Message>>> timed;
  socket->ReceiveAsync(std::chrono::milliseconds(75), [&timed](Outcome<std::optional<Message>> m) {
    timed.set_value(std::move(m));
  });
  auto timed_future = timed.get_future();
  ASSERT_EQ(timed_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  auto expired = timed_future.get();
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().code(), "TimeoutError");

  // Same session, fresh deadline: the echo arrives well inside it, the
  // delivery wins the race, and the stale first timer never fires into
  // this park (set_value would abort on a second completion).
  std::promise<Outcome<std::optional<Message>>> delivered;
  socket->ReceiveAsync(std::chrono::seconds(30), [&delivered](Outcome<std::optional<Message>> m) {
    delivered.set_value(std::move(m));
  });
  ASSERT_TRUE(socket->Send(Text("chat", "beats the deadline")).ok());
  auto delivered_future = delivered.get_future();
  ASSERT_EQ(delivered_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  auto echo = delivered_future.get();
  ASSERT_TRUE(echo.ok()) << echo.error().message();
  ASSERT_TRUE(echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:beats the deadline");

  socket->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, AThrowingAsyncReceiveCallbackDoesNotKillTheSession) {
  // ADR-0003/#109: an application completion callback that throws runs on the
  // connection's io thread. It must be contained there — never unwind
  // io_context::run() and terminate the process, and never abandon the read
  // pump — so the session keeps working after the throw.
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  std::promise<void> fired;
  socket->ReceiveAsync([&fired](Outcome<std::optional<Message>> message) {
    ASSERT_TRUE(message.ok()) << message.error().message();
    fired.set_value();
    throw std::runtime_error("application receive callback blew up");
  });
  ASSERT_TRUE(socket->Send(Text("chat", "first")).ok());

  // The echo delivered to the async callback, which threw; the callback still
  // ran (the promise was set) and the io thread survived the throw.
  auto future = fired.get_future();
  ASSERT_EQ(future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  // The read pump re-armed and the session is still usable: a fresh message
  // round-trips over the same connection.
  ASSERT_TRUE(socket->Send(Text("chat", "second")).ok());
  auto again = socket->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(again.ok()) << again.error().message();
  ASSERT_TRUE(again->has_value());
  EXPECT_EQ((**again).payload.ToString(), "echo:second");

  socket->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, AServeLoopBoundsItsReceiveAndKeepsWorkingBetweenMessages) {
  // The server-side shape the deadline unlocks: a serve loop that gets
  // control back on a quiet stream (here to push a nudge) instead of
  // parking until the peer speaks or the idle timeout fires.
  BeastServerTransport::Options options;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    for (int nudges = 0; nudges < 200;) {
      auto message = socket.Receive(std::chrono::milliseconds(20));
      if (!message.ok()) {
        if (message.error().code() != "TimeoutError") return;  // the wire is gone
        ++nudges;
        if (!socket.Send(Text("nudge", std::to_string(nudges))).ok()) return;
        continue;
      }
      if (!message->has_value()) return;  // the client's clean close
      Message reply = **message;
      reply.payload = Blob::FromString("echo:" + (*message)->payload.ToString());
      if (!socket.Send(reply).ok()) return;
    }
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  // A nudge arrives without the client having sent anything: the server's
  // deadline fired and its session survived it.
  auto nudge = socket->Receive(std::chrono::seconds(5));
  ASSERT_TRUE(nudge.ok()) << nudge.error().message();
  ASSERT_TRUE(nudge->has_value());
  const std::string* kind = (**nudge).FindString(":event-type");
  ASSERT_NE(kind, nullptr);
  EXPECT_EQ(*kind, "nudge");

  // The loop still serves real traffic between its deadlines.
  ASSERT_TRUE(socket->Send(Text("chat", "hello")).ok());
  bool echoed = false;
  for (int i = 0; i < 100 && !echoed; ++i) {
    auto message = socket->Receive(std::chrono::seconds(5));
    ASSERT_TRUE(message.ok()) << message.error().message();
    ASSERT_TRUE(message->has_value());
    echoed = (**message).payload.ToString() == "echo:hello";
  }
  EXPECT_TRUE(echoed);
  socket->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, ServerInitiatedCloseSurfacesAsNulloptClientSide) {
  BeastServerTransport::Options options;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    (void)socket.Send(Text("hello", "one message, then goodbye"));
    // Returning ends the session with a close handshake.
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  auto first = (*dialed)->Receive();
  ASSERT_TRUE(first.ok() && first->has_value());
  EXPECT_EQ((**first).payload.ToString(), "one message, then goodbye");
  auto second = (*dialed)->Receive();
  ASSERT_TRUE(second.ok()) << second.error().message();
  EXPECT_FALSE(second->has_value());
  server.Stop();
}

TEST(BeastWebSocketTest, FullDuplexSendsAndReceivesConcurrently) {
  // The server pushes its own stream while echoing nothing; the client
  // sends concurrently from a second thread. Neither direction may wait
  // for the other.
  constexpr int kEach = 50;
  std::atomic<int> server_received{0};
  BeastServerTransport::Options options;
  options.on_websocket = [&server_received](const HttpRequest&, WebSocket& socket) {
    std::thread pusher([&socket] {
      for (int i = 0; i < kEach; ++i) {
        if (!socket.Send(Text("push", "server-" + std::to_string(i))).ok()) {
          return;
        }
      }
    });
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) {
        break;
      }
      ++server_received;
    }
    pusher.join();
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  std::thread sender([&socket] {
    for (int i = 0; i < kEach; ++i) {
      ASSERT_TRUE(socket->Send(Text("send", "client-" + std::to_string(i))).ok());
    }
  });
  int client_received = 0;
  while (client_received < kEach) {
    auto message = socket->Receive();
    ASSERT_TRUE(message.ok() && message->has_value());
    ++client_received;
  }
  sender.join();
  socket->Close();
  auto end = socket->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
  EXPECT_EQ(client_received, kEach);
  // The server's receive loop sees every client message before the close.
  for (int i = 0; i < 200 && server_received.load() < kEach; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(server_received.load(), kEach);
  server.Stop();
}

TEST(BeastWebSocketTest, AReceiverThatDrainsLateStillGetsEveryMessageInOrder) {
  // Push well past the internal receive-queue depth while the client sits,
  // then drain: backpressure pauses the wire, resumes it, and loses
  // nothing.
  constexpr int kBurst = 40;
  std::promise<void> burst_sent;
  BeastServerTransport::Options options;
  options.on_websocket = [&burst_sent](const HttpRequest&, WebSocket& socket) {
    for (int i = 0; i < kBurst; ++i) {
      if (!socket.Send(Text("burst", std::to_string(i))).ok()) {
        return;
      }
    }
    // Every Send returned with the client not receiving — the receive
    // queue plus kernel buffers absorbed the burst without deadlocking
    // blocking sends against backpressure. Now let the client drain.
    burst_sent.set_value();
    // Hold the session open until the peer leaves.
    (void)socket.Receive();
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  ASSERT_EQ(burst_sent.get_future().wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "the burst wedged before the client received anything";
  for (int i = 0; i < kBurst; ++i) {
    auto message = (*dialed)->Receive();
    ASSERT_TRUE(message.ok() && message->has_value()) << "message " << i;
    EXPECT_EQ((**message).payload.ToString(), std::to_string(i));
  }
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, TheGateRefusesBeforeAnyUpgradeExists) {
  BeastServerTransport::Options options = EchoOptions();
  options.websocket_gate = [](const HttpRequest& request) -> std::optional<HttpResponse> {
    if (request.headers.Get("authorization").value_or("") != "Bearer let-me-in") {
      return PlainResponse(401, "who are you?");
    }
    return std::nullopt;
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  // No credentials: the dial fails — the server answered 401, never 101 —
  // and the refusal error names the HTTP status the server actually sent
  // (the response-capturing handshake), not just "declined".
  auto refused = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_FALSE(refused.ok());
  EXPECT_NE(refused.error().message().find("refused: HTTP 401"), std::string::npos)
      << refused.error().message();

  // Credentials ride the upgrade request's headers and the gate sees them.
  Headers credentials;
  credentials.Add("authorization", "Bearer let-me-in");
  auto admitted = BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = server.port(), .headers = credentials});
  ASSERT_TRUE(admitted.ok()) << admitted.error().message();
  ASSERT_TRUE((*admitted)->Send(Text("chat", "hi")).ok());
  auto echo = (*admitted)->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:hi");
  server.Stop();
}

TEST(BeastWebSocketTest, PlainHttpRequestsStillServeNextToTheUpgradePath) {
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    return PlainResponse(200, "plain:" + request.target);
                  })
                  .ok());
  BeastHttpClient client({.host = "127.0.0.1", .port = server.port()});
  HttpRequest request;
  request.method = "GET";
  request.target = "/health";
  auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->body, "plain:/health");
  server.Stop();
}

TEST(BeastWebSocketTest, WithoutAServeCallbackUpgradesFlowToTheHttpHandler) {
  std::atomic<bool> handler_saw_upgrade{false};
  BeastServerTransport server(BeastServerTransport::Options{});  // no on_websocket
  ASSERT_TRUE(server
                  .Start([&handler_saw_upgrade](const HttpRequest& request) {
                    if (request.headers.Get("upgrade").has_value()) {
                      handler_saw_upgrade = true;
                      return PlainResponse(426, "upgrade not served here");
                    }
                    return PlainResponse(200, "ok");
                  })
                  .ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  EXPECT_FALSE(dialed.ok());  // the 426 is not a 101
  EXPECT_TRUE(handler_saw_upgrade.load());
  server.Stop();
}

// Sends raw bytes and returns whatever the server answers (bounded read).
std::string RawExchange(int port, const std::string& bytes) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  timeval timeout{.tv_sec = 5, .tv_usec = 0};
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  std::string response;
  if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) == 1 &&
      ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0 &&
      ::send(fd, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size())) {
    char scratch[2048];
    while (true) {
      const ssize_t n = ::recv(fd, scratch, sizeof(scratch), 0);
      if (n <= 0) break;
      response.append(scratch, static_cast<std::size_t>(n));
      if (response.find("\r\n\r\n") != std::string::npos) break;
    }
  }
  ::close(fd);
  return response;
}

TEST(BeastWebSocketTest, AFailedUpgradeHandshakeIsObservedAsUpgradeFailure) {
  ConnectionEventRecorder recorder;
  BeastServerTransport::Options options = EchoOptions();
  options.on_connection_event = recorder.Hook();
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  // An upgrade-shaped request without Sec-WebSocket-Key: is_upgrade
  // matches, the gate admits, the handshake then refuses.
  (void)RawExchange(server.port(),
                    "GET /stream HTTP/1.1\r\n"
                    "Host: test\r\n"
                    "Connection: Upgrade\r\n"
                    "Upgrade: websocket\r\n"
                    "\r\n");
  using Kind = BeastServerTransport::ConnectionEvent::Kind;
  ASSERT_TRUE(recorder.WaitFor(1));
  server.Stop();  // io joined: the event log is final, so double-reporting shows
  {
    const std::lock_guard<std::mutex> lock(recorder.mutex);
    ASSERT_EQ(recorder.events.size(), 1u);
    EXPECT_EQ(recorder.events[0].kind, Kind::kUpgradeFailure);
    EXPECT_FALSE(recorder.events[0].peer_address.empty());
  }
}

// A raw Beast websocket peer, for protocol-violation tests the typed
// client cannot produce — and, given a subprotocol `offer`, for
// browser-fidelity tests of the negotiated JSON-text wire (ADR-0018): what
// it sends and reads is exactly what a page's `new WebSocket(url, offer)`
// plus JSON.parse would.
class RawWsPeer {
 public:
  explicit RawWsPeer(int port, const std::string& offer = "") : ws_(io_) {
    boost::asio::ip::tcp::resolver resolver(io_);
    boost::asio::connect(ws_.next_layer(), resolver.resolve("127.0.0.1", std::to_string(port)));
    if (!offer.empty()) {
      ws_.set_option(boost::beast::websocket::stream_base::decorator(
          [offer](boost::beast::websocket::request_type& req) {
            req.set(boost::beast::http::field::sec_websocket_protocol, offer);
          }));
    }
    ws_.handshake(response_, "127.0.0.1", "/");
  }

  // What the 101 selected — empty when the server chose no subprotocol
  // (a browser that offered one would fail the connection itself here).
  std::string selected_subprotocol() const {
    return std::string(response_[boost::beast::http::field::sec_websocket_protocol]);
  }

  // One received message, asserted to be a text frame (the JSON wire's
  // only legal kind).
  std::string ReadText() {
    boost::beast::flat_buffer buffer;
    ws_.read(buffer);
    EXPECT_FALSE(ws_.got_binary()) << "the JSON wire carries text frames";
    return boost::beast::buffers_to_string(buffer.data());
  }

  // One received message, asserted to be a binary frame (the default
  // wire's only legal kind).
  std::string ReadBinary() {
    boost::beast::flat_buffer buffer;
    ws_.read(buffer);
    EXPECT_TRUE(ws_.got_binary()) << "the binary wire carries binary frames";
    return boost::beast::buffers_to_string(buffer.data());
  }
  void SendText(const std::string& text) {
    ws_.text(true);
    ws_.write(boost::asio::buffer(text));
  }
  void SendBinary(const std::string& bytes) {
    ws_.binary(true);
    boost::beast::error_code ec;
    ws_.write(boost::asio::buffer(bytes), ec);  // the server may drop us mid-write
  }
  // Reads until close/error and reports how the stream ended — the caller
  // asserts websocket::error::closed (a real close frame arrived) and can
  // inspect reason() for the close code, so "the peer was told" is pinned
  // rather than assumed.
  boost::beast::error_code DrainToCloseCode() {
    boost::beast::flat_buffer buffer;
    boost::beast::error_code ec;
    while (!ec) {
      ws_.read(buffer, ec);
      buffer.consume(buffer.size());
    }
    return ec;
  }
  boost::beast::websocket::close_reason reason() const { return ws_.reason(); }

 private:
  boost::asio::io_context io_;
  boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws_;
  boost::beast::websocket::response_type response_;
};

TEST(BeastWebSocketTest, ATextMessageFailsTheSessionAsAProtocolError) {
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port());
  peer.SendText("this wire speaks event-stream frames, not text");
  auto result = serve_saw.get_future().get();
  ASSERT_FALSE(result.ok());  // an error, never nullopt: the peer misbehaved
  EXPECT_NE(result.error().message().find("text message"), std::string::npos);
  // And the peer was told why: a real close frame, protocol_error.
  EXPECT_EQ(peer.DrainToCloseCode(), boost::beast::websocket::error::closed);
  EXPECT_EQ(peer.reason().code, boost::beast::websocket::close_code::protocol_error);
  server.Stop();
}

TEST(BeastWebSocketTest, ABinaryMessageThatIsNotOneFrameFailsTheSession) {
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port());
  peer.SendBinary("garbage that is not an event-stream frame");
  auto result = serve_saw.get_future().get();
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("eventstream"), std::string::npos)
      << result.error().message();
  EXPECT_EQ(peer.DrainToCloseCode(), boost::beast::websocket::error::closed);
  EXPECT_EQ(peer.reason().code, boost::beast::websocket::close_code::protocol_error);
  server.Stop();
}

TEST(BeastWebSocketTest, ATrailingByteAfterTheFrameFailsTheSession) {
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto frame = eventstream::EncodeMessage(Text("chat", "valid"));
  ASSERT_TRUE(frame.ok());
  RawWsPeer peer(server.port());
  peer.SendBinary(*frame + std::string(1, '\0'));  // one frame plus one byte
  auto result = serve_saw.get_future().get();
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("exactly one"), std::string::npos);
  server.Stop();
}

// --- The negotiated JSON-text wire (ADR-0018) ---

const std::string kJsonToken(eventstream::kJsonFramesSubprotocol);

// A JSON-mode serve loop must echo envelope-bearing messages verbatim:
// the JSON wire refuses payloads that are not JSON objects, so the
// "echo:"-prefix server above would fail its own Send.
BeastServerTransport::Options VerbatimEchoOptions() {
  BeastServerTransport::Options options;
  options.websocket_accept_json_frames = true;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) {
        return;
      }
      if (!socket.Send(**message).ok()) {
        return;
      }
    }
  };
  return options;
}

TEST(BeastWebSocketTest, NegotiatedJsonFramesCarryMessagesTransparently) {
  // Both ends typed: the offer is echoed, the wire flips to text, and
  // above the session nothing changes — the same Messages round-trip.
  BeastServerTransport server(VerbatimEchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = server.port(), .offer_json_frames = true});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();

  const Message sent = eventstream::MakeEventMessage("chat", "application/json",
                                                     Blob::FromString(R"({"text":"hello"})"));
  ASSERT_TRUE((*dialed)->Send(sent).ok());
  auto received = (*dialed)->Receive();
  ASSERT_TRUE(received.ok()) << received.error().message();
  ASSERT_TRUE(received->has_value());
  EXPECT_EQ(**received, sent);
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, ABrowserFidelityPeerSpeaksTheJsonWire) {
  // The issue-#113 story at the transport layer: a peer doing exactly
  // what a page does — offer the subprotocol, JSON.stringify out,
  // JSON.parse in — while the serve callback speaks eventstream::Message,
  // unaware of the wire mode.
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.websocket_accept_json_frames = true;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    (void)socket.Send(eventstream::MakeEventMessage("greeting", "application/json",
                                                    Blob::FromString(R"({"text":"hi"})")));
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port(), kJsonToken);
  EXPECT_EQ(peer.selected_subprotocol(), kJsonToken);
  // Byte-pinned: this is the text a browser's onmessage handler receives.
  EXPECT_EQ(peer.ReadText(), R"({"event":"greeting","payload":{"text":"hi"}})");

  peer.SendText(R"({"event":"chat","payload":{"text":"from a browser"}})");
  auto result = serve_saw.get_future().get();
  ASSERT_TRUE(result.ok()) << result.error().message();
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ(**result,
            eventstream::MakeEventMessage("chat", "application/json",
                                          Blob::FromString(R"({"text":"from a browser"})")));
  server.Stop();
}

TEST(BeastWebSocketTest, TheOfferFallsBackToBinaryWhenTheServerDoesNotAccept) {
  // Flag off: the offer is simply not selected (headerless 101, a
  // pre-ADR-0018 server byte for byte) and the typed client silently
  // keeps the binary wire — both modes carry the same messages.
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer raw(server.port(), kJsonToken);
  EXPECT_EQ(raw.selected_subprotocol(), "");

  auto dialed = BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = server.port(), .offer_json_frames = true});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const Message sent = Text("chat", "still binary");
  ASSERT_TRUE((*dialed)->Send(sent).ok());
  auto received = (*dialed)->Receive();
  ASSERT_TRUE(received.ok() && received->has_value());
  EXPECT_EQ((**received).payload.ToString(), "echo:still binary");
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, ClientsThatDoNotOfferAreUntouchedByTheServerFlag) {
  // Native clients never negotiate: with the flag on and no offer, the
  // 101 carries no subprotocol and the binary wire serves as always.
  BeastServerTransport server(VerbatimEchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port());
  EXPECT_EQ(peer.selected_subprotocol(), "");
  const Message sent = Text("chat", "binary as ever");
  auto frame = eventstream::EncodeMessage(sent);
  ASSERT_TRUE(frame.ok());
  peer.SendBinary(*frame);
  // The verbatim echo comes back as one binary event-stream frame.
  EXPECT_EQ(peer.ReadBinary(), *frame);
  server.Stop();
}

TEST(BeastWebSocketTest, TheOfferIsFoundAmongMultipleSubprotocols) {
  // Real pages offer lists — new WebSocket(url, ["chat.v2", token]) — and
  // RFC 6455 lets the whole list ride one comma-separated header. The
  // token is found wherever it sits; only the token is echoed.
  BeastServerTransport server(VerbatimEchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port(), "chat.v2, " + kJsonToken + ", chat.v1");
  EXPECT_EQ(peer.selected_subprotocol(), kJsonToken);
  peer.SendText(R"({"event":"chat","payload":{"n":1}})");
  EXPECT_EQ(peer.ReadText(), R"({"event":"chat","payload":{"n":1}})");
  server.Stop();
}

TEST(BeastWebSocketTest, UnknownSubprotocolOffersAreNotSelected) {
  // Tokens are case-sensitive and never fuzzy-matched: an unrecognized
  // offer gets a headerless 101 (the peer may then fail the connection
  // itself, as browsers do) and the session stays binary.
  BeastServerTransport server(VerbatimEchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port(), "chat.v2, SMITHY.EVENTSTREAM.V1+JSON");
  EXPECT_EQ(peer.selected_subprotocol(), "");
  auto frame = eventstream::EncodeMessage(Text("chat", "still binary"));
  ASSERT_TRUE(frame.ok());
  peer.SendBinary(*frame);
  EXPECT_EQ(peer.ReadBinary(), *frame);
  server.Stop();
}

TEST(BeastWebSocketTest, AJsonSendRefusalLeavesTheSessionUsable) {
  // The Send contract holds per wire mode: a message the JSON encoder
  // refuses (here: no envelope headers) is Error::Validation with nothing
  // written, and the session then carries the next, well-formed event.
  BeastServerTransport server(VerbatimEchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = server.port(), .offer_json_frames = true});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();

  auto refused = (*dialed)->Send(Text("chat", "no envelope"));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);

  const Message valid = eventstream::MakeEventMessage("chat", "application/json",
                                                      Blob::FromString(R"({"text":"next"})"));
  ASSERT_TRUE((*dialed)->Send(valid).ok());
  auto received = (*dialed)->Receive();
  ASSERT_TRUE(received.ok() && received->has_value());
  EXPECT_EQ(**received, valid);
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, ABinaryFrameOnAJsonSessionFailsTheSession) {
  // The fail-closed transpose: in JSON mode, *binary* is the protocol
  // violation — the exact mirror of binary mode's posture on text.
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.websocket_accept_json_frames = true;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto frame = eventstream::EncodeMessage(Text("chat", "valid binary frame"));
  ASSERT_TRUE(frame.ok());
  RawWsPeer peer(server.port(), kJsonToken);
  ASSERT_EQ(peer.selected_subprotocol(), kJsonToken);
  peer.SendBinary(*frame);
  auto result = serve_saw.get_future().get();
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("binary message on a JSON-text"), std::string::npos)
      << result.error().message();
  EXPECT_EQ(peer.DrainToCloseCode(), boost::beast::websocket::error::closed);
  EXPECT_EQ(peer.reason().code, boost::beast::websocket::close_code::protocol_error);
  server.Stop();
}

TEST(BeastWebSocketTest, AMalformedJsonEnvelopeFailsTheSession) {
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.websocket_accept_json_frames = true;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port(), kJsonToken);
  ASSERT_EQ(peer.selected_subprotocol(), kJsonToken);
  peer.SendText(R"({"event":"chat","payload":{},"extra":true})");  // unknown member
  auto result = serve_saw.get_future().get();
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("eventstream json frame"), std::string::npos)
      << result.error().message();
  EXPECT_EQ(peer.DrainToCloseCode(), boost::beast::websocket::error::closed);
  EXPECT_EQ(peer.reason().code, boost::beast::websocket::close_code::protocol_error);
  server.Stop();
}

TEST(BeastWebSocketTest, AServerSelectingAnUnofferedSubprotocolFailsTheDial) {
  // RFC 6455: a server may only select what was offered. A raw Beast
  // server that decorates a bogus selection onto the 101 is a peer the
  // typed client cannot trust to speak either wire.
  boost::asio::io_context io;
  boost::asio::ip::tcp::acceptor acceptor(io, {boost::asio::ip::address_v4::loopback(), 0});
  const int port = acceptor.local_endpoint().port();
  std::thread hostile([&acceptor, &io] {
    boost::asio::ip::tcp::socket socket(io);
    acceptor.accept(socket);
    boost::beast::websocket::stream<boost::asio::ip::tcp::socket> ws(std::move(socket));
    ws.set_option(boost::beast::websocket::stream_base::decorator(
        [](boost::beast::websocket::response_type& response) {
          response.set(boost::beast::http::field::sec_websocket_protocol, "bogus.proto");
        }));
    boost::beast::error_code ec;
    ws.accept(ec);
    if (!ec) {
      boost::beast::flat_buffer buffer;
      ws.read(buffer, ec);  // hold the session until the client tears down
    }
  });

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = port});
  ASSERT_FALSE(dialed.ok());
  EXPECT_NE(dialed.error().message().find("unoffered subprotocol"), std::string::npos)
      << dialed.error().message();
  hostile.join();
}

TEST(BeastWebSocketTest, StartRefusesJsonFramesWithoutAServeCallback) {
  BeastServerTransport::Options options;
  options.websocket_accept_json_frames = true;
  BeastServerTransport server(options);
  auto started = server.Start(NotFoundHandler());
  ASSERT_FALSE(started.ok());
  EXPECT_NE(started.error().message().find("websocket_accept_json_frames"), std::string::npos);
}

// --- The raw-text wire (ADR-0023) ---

// A headerless message whose payload is the frame — what the jsonRpc2
// stream layers trade below the envelope wrapper.
Message RawText(std::string text) {
  Message message;
  message.payload = Blob::FromString(std::move(text));
  return message;
}

BeastServerTransport::Options RawTextEchoOptions() {
  BeastServerTransport::Options options;
  options.websocket_raw_text_frames = true;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) {
        return;
      }
      if (!socket.Send(**message).ok()) {
        return;
      }
    }
  };
  return options;
}

TEST(BeastWebSocketTest, RawTextFramesCarryHeaderlessMessagesTransparently) {
  // Both ends typed, no negotiation anywhere: the server flag and the dial
  // flag each flip their own end, and headerless messages round-trip.
  BeastServerTransport server(RawTextEchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = server.port(), .raw_text_frames = true});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();

  const Message sent = RawText(R"({"jsonrpc":"2.0","method":"ping","params":{"id":1}})");
  ASSERT_TRUE((*dialed)->Send(sent).ok());
  auto received = (*dialed)->Receive();
  ASSERT_TRUE(received.ok()) << received.error().message();
  ASSERT_TRUE(received->has_value());
  EXPECT_EQ(**received, sent);
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, ABrowserFidelityPeerSpeaksTheRawTextWire) {
  // The issue-#123 story at the transport layer: a peer doing exactly what
  // a page does — new WebSocket(url) with no subprotocol, text out, text
  // in — while the serve callback trades headerless Messages.
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.websocket_raw_text_frames = true;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    (void)socket.Send(RawText(R"({"jsonrpc":"2.0","result":{},"id":1})"));
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port());
  EXPECT_EQ(peer.selected_subprotocol(), "");  // unnegotiated: headerless 101
  // Byte-pinned: this is the text a browser's onmessage handler receives.
  EXPECT_EQ(peer.ReadText(), R"({"jsonrpc":"2.0","result":{},"id":1})");

  peer.SendText(R"({"jsonrpc":"2.0","method":"Chat","params":{},"id":1})");
  auto result = serve_saw.get_future().get();
  ASSERT_TRUE(result.ok()) << result.error().message();
  ASSERT_TRUE(result->has_value());
  EXPECT_EQ(**result, RawText(R"({"jsonrpc":"2.0","method":"Chat","params":{},"id":1})"));
  server.Stop();
}

TEST(BeastWebSocketTest, ARawTextSendRefusesHeaderedMessagesAndSurvives) {
  // Encode refuses what the wire cannot represent (no header channel, no
  // envelope), Validation with nothing written — then the session still
  // carries the next headerless message.
  BeastServerTransport server(RawTextEchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = server.port(), .raw_text_frames = true});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();

  auto refused = (*dialed)->Send(
      eventstream::MakeEventMessage("chat", "application/json", Blob::FromString("{}")));
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().kind(), ErrorKind::kValidation);

  const Message valid = RawText(R"({"jsonrpc":"2.0","method":"m","params":{"id":1}})");
  ASSERT_TRUE((*dialed)->Send(valid).ok());
  auto received = (*dialed)->Receive();
  ASSERT_TRUE(received.ok() && received->has_value());
  EXPECT_EQ(**received, valid);
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, ABinaryFrameOnARawTextSessionFailsTheSession) {
  // The fail-closed transpose, third application: on the raw-text wire,
  // binary is the protocol violation.
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.websocket_raw_text_frames = true;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port());
  peer.SendBinary("any bytes at all");
  auto result = serve_saw.get_future().get();
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("binary message on a raw-text"), std::string::npos)
      << result.error().message();
  EXPECT_EQ(peer.DrainToCloseCode(), boost::beast::websocket::error::closed);
  EXPECT_EQ(peer.reason().code, boost::beast::websocket::close_code::protocol_error);
  server.Stop();
}

TEST(BeastWebSocketTest, StartRefusesRawTextWithoutAServeCallback) {
  BeastServerTransport::Options options;
  options.websocket_raw_text_frames = true;
  BeastServerTransport server(options);
  auto started = server.Start(NotFoundHandler());
  ASSERT_FALSE(started.ok());
  EXPECT_NE(started.error().message().find("websocket_raw_text_frames"), std::string::npos);
}

TEST(BeastWebSocketTest, StartRefusesRawTextBesideTheJsonFramesNegotiation) {
  // One listener speaks one wire family: raw text claims every upgrade,
  // so the negotiation could never win.
  BeastServerTransport::Options options = RawTextEchoOptions();
  options.websocket_accept_json_frames = true;
  BeastServerTransport server(options);
  auto started = server.Start(NotFoundHandler());
  ASSERT_FALSE(started.ok());
  EXPECT_NE(started.error().message().find("cannot both be set"), std::string::npos);
}

TEST(BeastWebSocketTest, DialRefusesRawTextBesideTheJsonOffer) {
  auto dialed = BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = 1, .offer_json_frames = true, .raw_text_frames = true});
  ASSERT_FALSE(dialed.ok());
  EXPECT_EQ(dialed.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(dialed.error().message().find("cannot both be set"), std::string::npos);
}

TEST(BeastWebSocketTest, SendRefusesWhatTheCodecRefusesWithoutTouchingTheWire) {
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();

  Message unencodable;
  unencodable.headers.push_back({"", true});  // empty names cannot encode
  const auto refused = (*dialed)->Send(unencodable);
  ASSERT_FALSE(refused.ok());
  // The session is untouched: a valid message still round-trips.
  ASSERT_TRUE((*dialed)->Send(Text("chat", "still alive")).ok());
  auto echo = (*dialed)->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:still alive");
  server.Stop();
}

TEST(BeastWebSocketTest, StopUnblocksAServeCallbackMidReceive) {
  std::promise<Outcome<std::optional<Message>>> serve_result;
  BeastServerTransport::Options options;
  options.drain_timeout_seconds = 1;
  options.on_websocket = [&serve_result](const HttpRequest&, WebSocket& socket) {
    serve_result.set_value(socket.Receive());  // blocks until Stop aborts it
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let serve block

  const auto stop_started = std::chrono::steady_clock::now();
  server.Stop();
  const auto stop_took = std::chrono::steady_clock::now() - stop_started;
  EXPECT_LT(stop_took, std::chrono::seconds(8)) << "Stop() must not wait for stream sessions";

  auto result = serve_result.get_future().get();
  ASSERT_FALSE(result.ok());
  EXPECT_NE(result.error().message().find("server stopping"), std::string::npos);
}

TEST(BeastWebSocketTest, TlsStreamsCarryMessagesEndToEnd) {
  BeastServerTransport::Options options = EchoOptions();
  options.tls_certificate_chain_pem = smithy::testing::kTestCertificatePem;
  options.tls_private_key_pem = smithy::testing::kTestPrivateKeyPem;
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  BeastWebSocketClient::Options dial;
  dial.host = "127.0.0.1";
  dial.port = server.port();
  dial.tls = true;
  dial.tls_options.ca_pem = smithy::testing::kTestCertificatePem;
  auto dialed = BeastWebSocketClient::Dial(dial);
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();

  ASSERT_TRUE((*dialed)->Send(Text("secure", "over wss")).ok());
  auto echo = (*dialed)->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:over wss");
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, ConcurrentSessionsServeIndependently) {
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  constexpr int kClients = 4;
  std::atomic<int> failures{0};
  std::vector<std::thread> clients;
  clients.reserve(kClients);
  for (int c = 0; c < kClients; ++c) {
    clients.emplace_back([&failures, c, port = server.port()] {
      auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = port});
      if (!dialed.ok()) {
        ++failures;
        return;
      }
      for (int i = 0; i < 20; ++i) {
        const std::string body = "client-" + std::to_string(c) + "-" + std::to_string(i);
        if (!(*dialed)->Send(Text("chat", body)).ok()) {
          ++failures;
          return;
        }
        auto echo = (*dialed)->Receive();
        if (!echo.ok() || !echo->has_value() || (**echo).payload.ToString() != "echo:" + body) {
          ++failures;
          return;
        }
      }
      (*dialed)->Close();
    });
  }
  for (std::thread& client : clients) {
    client.join();
  }
  EXPECT_EQ(failures.load(), 0);
  server.Stop();
}

TEST(BeastWebSocketTest, DialFailuresAreErrorsNotCrashes) {
  // A port nobody listens on.
  const auto no_listener =
      BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = 1, .handshake_timeout_ms = 2000});
  EXPECT_FALSE(no_listener.ok());
  // A host that does not resolve.
  const auto no_host = BeastWebSocketClient::Dial(
      {.host = "no-such-host.invalid", .port = 80, .handshake_timeout_ms = 2000});
  EXPECT_FALSE(no_host.ok());
  // No host at all.
  EXPECT_FALSE(BeastWebSocketClient::Dial({}).ok());
}

TEST(BeastWebSocketTest, AnInjectedUpgradeHeaderIsRefusedBeforeConnecting) {
  // The outbound injection defense (issue #109): Options::headers ride the
  // upgrade GET verbatim, so CR/LF there is request splitting. Dial refuses
  // with Validation before it connects — port 1 is never reached.
  Headers hostile;
  hostile.Add("x-token", "abc\r\nx-evil: 1");
  const auto dialed =
      BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = 1, .headers = hostile});
  ASSERT_FALSE(dialed.ok());
  EXPECT_EQ(dialed.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(dialed.error().message().find("x-token"), std::string::npos);
}

TEST(BeastWebSocketTest, AnInjectedUpgradeTargetIsRefusedBeforeConnecting) {
  // Request-line injection (issue #109): the target rides the upgrade GET's
  // request line — Beast's handshake target() no more rejects CR/LF than
  // the HTTP client's does — so a raw CR/LF splits the upgrade request.
  // Dial refuses with Validation before connecting; port 1 is never reached.
  const auto dialed =
      BeastWebSocketClient::Dial({.host = "127.0.0.1",
                                  .port = 1,
                                  .target = "/chat HTTP/1.1\r\nX-Smuggled: 1\r\n\r\nGET /admin"});
  ASSERT_FALSE(dialed.ok());
  EXPECT_EQ(dialed.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(dialed.error().message().find("target"), std::string::npos);
}

TEST(BeastWebSocketTest, StartRefusesAServeCallbackWithoutAHandlerPool) {
  BeastServerTransport::Options options = EchoOptions();
  options.handler_threads = 0;
  BeastServerTransport server(options);
  const auto started = server.Start(NotFoundHandler());
  ASSERT_FALSE(started.ok());
  EXPECT_NE(started.error().message().find("handler_threads"), std::string::npos);
}

TEST(BeastWebSocketTest, StartRefusesAGateWithoutAServeCallback) {
  // A gate alone would be silently dead config: the upgrade path only
  // exists with on_websocket set.
  BeastServerTransport::Options options;
  options.websocket_gate = [](const HttpRequest&) -> std::optional<HttpResponse> {
    return std::nullopt;
  };
  BeastServerTransport server(options);
  const auto started = server.Start(NotFoundHandler());
  ASSERT_FALSE(started.ok());
  EXPECT_NE(started.error().message().find("on_websocket"), std::string::npos);
}

TEST(BeastWebSocketTest, DialWithPortZeroUsesTheSchemeDefault) {
  // tls=false + port 0 dials 80; nothing listens there in this test
  // environment, so the dial errors — but the error must name port 80,
  // proving the scheme default resolved (a wss dial defaulting to 80 was
  // the reviewed footgun).
  const auto dialed =
      BeastWebSocketClient::Dial({.host = "127.0.0.1", .handshake_timeout_ms = 2000});
  ASSERT_FALSE(dialed.ok());
  EXPECT_NE(dialed.error().message().find(":80"), std::string::npos) << dialed.error().message();
}

constexpr const char* kValidUpgradeRequest =
    "GET /stream HTTP/1.1\r\n"
    "Host: test\r\n"
    "Connection: Upgrade\r\n"
    "Upgrade: websocket\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "\r\n";

// A raw connection that can do several request/response exchanges, for
// pinning the exact wire bytes of refusals and their keep-alive behavior.
class RawConnection {
 public:
  explicit RawConnection(int port) {
    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    timeval timeout{.tv_sec = 5, .tv_usec = 0};
    (void)::setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<std::uint16_t>(port));
    ::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    connected_ = ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0;
  }
  ~RawConnection() {
    if (fd_ >= 0) ::close(fd_);
  }
  bool connected() const { return connected_; }
  bool SendAll(const std::string& bytes) {
    return ::send(fd_, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size());
  }
  std::string ReadUntil(const std::string& sentinel) {
    std::string response;
    char scratch[4096];
    while (response.find(sentinel) == std::string::npos) {
      const ssize_t n = ::recv(fd_, scratch, sizeof(scratch), 0);
      if (n <= 0) break;
      response.append(scratch, static_cast<std::size_t>(n));
    }
    return response;
  }

 private:
  int fd_ = -1;
  bool connected_ = false;
};

TEST(BeastWebSocketTest, TlsDialAgainstAnUntrustedCertificateFailsFastAndNonRetryably) {
  // The review's highest-value mutant: a client whose verification is
  // silently off passes every other TLS test (a correct CA also passes
  // with verification disabled) — only dialing an UNTRUSTED cert with
  // default options can catch it.
  BeastServerTransport::Options options = EchoOptions();
  options.tls_certificate_chain_pem = smithy::testing::kTestCertificatePem;
  options.tls_private_key_pem = smithy::testing::kTestPrivateKeyPem;
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  BeastWebSocketClient::Options dial;
  dial.host = "127.0.0.1";
  dial.port = server.port();
  dial.tls = true;  // defaults: verify on, system roots — must NOT trust the test cert
  const auto started = std::chrono::steady_clock::now();
  auto dialed = BeastWebSocketClient::Dial(dial);
  const auto took = std::chrono::steady_clock::now() - started;
  ASSERT_FALSE(dialed.ok());
  EXPECT_FALSE(dialed.error().retryable());  // identity problems do not retry away
  EXPECT_NE(dialed.error().message().find("TLS handshake"), std::string::npos)
      << dialed.error().message();
  EXPECT_LT(took, std::chrono::seconds(10));  // an error, not a hang
  server.Stop();
}

TEST(BeastWebSocketTest, AThrowingGateRefusesWithA500NeverAnAccidentalAccept) {
  // An auth backend outage must fail CLOSED: the guarded gate refuses
  // with a 500 — a catch that admitted instead would open every stream
  // to unauthenticated peers, and only this test notices.
  BeastServerTransport::Options options = EchoOptions();
  options.websocket_gate = [](const HttpRequest&) -> std::optional<HttpResponse> {
    throw std::runtime_error("auth backend down");
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawConnection raw(server.port());
  ASSERT_TRUE(raw.connected());
  ASSERT_TRUE(raw.SendAll(kValidUpgradeRequest));
  const std::string response = raw.ReadUntil("\r\n\r\n");
  EXPECT_EQ(response.rfind("HTTP/1.1 500", 0), 0U) << response;

  EXPECT_FALSE(BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()}).ok());
  server.Stop();
}

TEST(BeastWebSocketTest, AGateRefusalIsAPlainAnswerOnALiveKeepAliveConnection) {
  // The ADR's "written as the plain HTTP answer, keep-alive intact",
  // pinned on the wire: the literal 401, then a plain GET served on the
  // SAME connection afterwards.
  BeastServerTransport::Options options = EchoOptions();
  options.websocket_gate = [](const HttpRequest& request) -> std::optional<HttpResponse> {
    if (request.headers.Get("authorization").value_or("") != "Bearer let-me-in") {
      return PlainResponse(401, "who are you?");
    }
    return std::nullopt;
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawConnection raw(server.port());
  ASSERT_TRUE(raw.connected());
  ASSERT_TRUE(raw.SendAll(kValidUpgradeRequest));
  const std::string refusal = raw.ReadUntil("who are you?");
  EXPECT_EQ(refusal.rfind("HTTP/1.1 401", 0), 0U) << refusal;
  ASSERT_TRUE(raw.SendAll("GET /health HTTP/1.1\r\nHost: test\r\n\r\n"));
  const std::string next = raw.ReadUntil("no such route");
  EXPECT_NE(next.find("HTTP/1.1 404"), std::string::npos) << next;

  // The typed dial's refusal is terminal: clients must not retry-hammer
  // an auth failure.
  auto refused = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_FALSE(refused.ok());
  EXPECT_FALSE(refused.error().retryable());
  server.Stop();
}

TEST(BeastWebSocketTest, AThrowingServeCallbackStillClosesTheSessionCleanly) {
  BeastServerTransport::Options options;
  options.on_websocket = [](const HttpRequest&, WebSocket&) {
    throw std::runtime_error("application bug");
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  auto end = (*dialed)->Receive();
  ASSERT_TRUE(end.ok()) << end.error().message();  // a clean close, not an error
  EXPECT_FALSE(end->has_value());

  // And the server survived the throw: a second session still upgrades.
  auto again = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  EXPECT_TRUE(again.ok());
  server.Stop();
}

TEST(BeastWebSocketTest, AVanishedPeerIsDetectedByTheServerIdleTimeout) {
  // ADR-0015: "a vanished peer is detected without any application ping
  // protocol." The knob makes it unit-testable: idle timeout 1s, a peer
  // that handshakes and never reads (so pings go unanswered).
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.websocket_idle_timeout_seconds = 1;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  const auto started = std::chrono::steady_clock::now();
  RawWsPeer peer(server.port());
  auto future = serve_saw.get_future();
  ASSERT_EQ(future.wait_for(std::chrono::seconds(8)), std::future_status::ready)
      << "the idle timeout never fired: a vanished peer holds its session forever";
  auto result = future.get();
  ASSERT_FALSE(result.ok());
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(5));
  server.Stop();
}

TEST(BeastWebSocketTest, KeepAlivePingsKeepAQuietStreamAlive) {
  // The mirror image: an aggressive 1s idle timeout with a healthy peer
  // must NOT kill the stream — the built-in pings answer for it. Kills
  // the keep_alive_pings=false mutant, which every other test survives.
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  BeastWebSocketClient::Options dial;
  dial.host = "127.0.0.1";
  dial.port = server.port();
  dial.idle_timeout_seconds = 1;
  auto dialed = BeastWebSocketClient::Dial(dial);
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();

  std::this_thread::sleep_for(std::chrono::milliseconds(2500));  // several idle periods
  ASSERT_TRUE((*dialed)->Send(Text("chat", "still here")).ok());
  auto echo = (*dialed)->Receive();
  ASSERT_TRUE(echo.ok()) << echo.error().message();
  ASSERT_TRUE(echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:still here");
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, ConcurrentSendersInterleaveWholeFrames) {
  // websocket.h promises concurrent Sends are serialized. Without the
  // serialization this is two concurrent write ops on one Beast stream —
  // an assertion in debug builds, silent frame corruption in release.
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  constexpr int kPerSender = 25;
  auto sender = [&socket](const std::string& tag) {
    for (int i = 0; i < kPerSender; ++i) {
      if (!socket->Send(Text("chat", tag + "-" + std::to_string(i))).ok()) {
        return;
      }
    }
  };
  std::thread a(sender, "a");
  std::thread b(sender, "b");
  std::multiset<std::string> received;
  for (int i = 0; i < 2 * kPerSender; ++i) {
    auto echo = socket->Receive();
    ASSERT_TRUE(echo.ok()) << echo.error().message();
    ASSERT_TRUE(echo->has_value());
    received.insert((**echo).payload.ToString());
  }
  a.join();
  b.join();
  std::multiset<std::string> expected;
  for (int i = 0; i < kPerSender; ++i) {
    expected.insert("echo:a-" + std::to_string(i));
    expected.insert("echo:b-" + std::to_string(i));
  }
  EXPECT_EQ(received, expected);
  socket->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, AnOversizedInboundMessageFailsTheSession) {
  // The 16 MiB ceiling, inbound: kMaxMessageBytes happens to equal
  // Beast's default read_message_max today, so this pins the bound
  // against either side drifting. The peer cooperates with the failure
  // close (drains); a peer that never reads resolves on the idle
  // deadline instead, like any quiet stream (ADR-0015).
  std::promise<Outcome<std::optional<Message>>> serve_saw;
  BeastServerTransport::Options options;
  options.on_websocket = [&serve_saw](const HttpRequest&, WebSocket& socket) {
    serve_saw.set_value(socket.Receive());
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  RawWsPeer peer(server.port());
  peer.SendBinary(std::string(eventstream::kMaxMessageBytes + 1, 'x'));
  (void)peer.DrainToCloseCode();  // cooperate so the failure close resolves promptly
  auto future = serve_saw.get_future();
  ASSERT_EQ(future.wait_for(std::chrono::seconds(8)), std::future_status::ready);
  ASSERT_FALSE(future.get().ok());
  server.Stop();
}

TEST(BeastWebSocketTest, TheGateAndServeCallbackSeeTheFullUpgradeRequest) {
  // Method, target with its query intact, every header, and the peer
  // stamp — at BOTH decision points. A request copy that lost any of
  // these would break real routing/auth silently.
  std::mutex seen_mutex;
  std::optional<HttpRequest> seen;
  BeastServerTransport::Options options = EchoOptions();
  options.websocket_gate = [&](const HttpRequest& request) -> std::optional<HttpResponse> {
    const std::lock_guard<std::mutex> lock(seen_mutex);
    seen = request;
    return std::nullopt;
  };
  std::promise<std::string> serve_target;
  options.on_websocket = [&serve_target](const HttpRequest& request, WebSocket& socket) {
    serve_target.set_value(request.target);
    (void)socket.Receive();
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  Headers headers;
  headers.Add("authorization", "Bearer tok");
  headers.Add("x-stream-room", "lobby");
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1",
                                            .port = server.port(),
                                            .target = "/rooms/7/stream?replay=3",
                                            .headers = headers});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  {
    const std::lock_guard<std::mutex> lock(seen_mutex);
    ASSERT_TRUE(seen.has_value());
    EXPECT_EQ(seen->method, "GET");
    EXPECT_EQ(seen->target, "/rooms/7/stream?replay=3");
    EXPECT_EQ(seen->headers.Get("authorization").value_or(""), "Bearer tok");
    EXPECT_EQ(seen->headers.Get("x-stream-room").value_or(""), "lobby");
    EXPECT_FALSE(seen->peer_address.empty());
  }
  auto target_future = serve_target.get_future();
  ASSERT_EQ(target_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(target_future.get(), "/rooms/7/stream?replay=3");
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, CloseUnblocksASendWedgedOnANonReadingPeer) {
  // The contract's other half (websocket.h): Close() from another thread
  // unblocks a blocked SEND, not just a blocked Receive. A peer that stops
  // reading wedges a sender for real — the session's receive queue and the
  // kernel buffers fill, then async_write never completes — so Close must
  // escalate (cancel the in-flight write) rather than defer behind it.
  std::promise<void> release_serve;
  std::shared_future<void> released = release_serve.get_future().share();
  BeastServerTransport::Options options;
  options.on_websocket = [released](const HttpRequest&, WebSocket&) {
    released.wait();  // never Receives: the client's sends back up
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  // Far more than the receive queue plus kernel buffers can absorb: the
  // sender is guaranteed to wedge mid-loop, blocked inside Send.
  const std::string bulk(1024 * 1024, 'x');
  std::atomic<int> sent{0};
  std::promise<Outcome<Unit>> send_result;
  std::thread sender([&socket, &bulk, &sent, &send_result] {
    for (int i = 0; i < 64; ++i) {
      auto outcome = socket->Send(Text("bulk", bulk));
      if (!outcome.ok()) {
        send_result.set_value(std::move(outcome));
        return;
      }
      ++sent;
    }
    send_result.set_value(Unit{});  // never wedged: the test setup is wrong
  });

  // Let the sender wedge (progress stalls), then Close from this thread.
  int last = -1;
  while (sent.load() != last) {
    last = sent.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  const auto closed_at = std::chrono::steady_clock::now();
  socket->Close();
  auto result_future = send_result.get_future();
  ASSERT_EQ(result_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "Close() left the Send blocked";
  EXPECT_LT(std::chrono::steady_clock::now() - closed_at, std::chrono::seconds(2));
  sender.join();
  auto result = result_future.get();
  ASSERT_FALSE(result.ok()) << "the wedged Send was expected to fail, not complete";
  EXPECT_NE(result.error().message().find("closed"), std::string::npos) << result.error().message();

  // The receiver side of an aborted-mid-frame wire surfaces an error, not
  // the clean nullopt — the wire was cut with a frame half-written
  // (documented in websocket.h).
  auto receive = socket->Receive();
  EXPECT_FALSE(receive.ok());

  release_serve.set_value();
  server.Stop();
}

TEST(BeastWebSocketTest, SendAfterCloseFailsWithATransportError) {
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  (*dialed)->Close();
  auto end = (*dialed)->Receive();
  ASSERT_TRUE(end.ok());
  EXPECT_FALSE(end->has_value());
  auto sent = (*dialed)->Send(Text("chat", "too late"));
  ASSERT_FALSE(sent.ok());
  EXPECT_NE(sent.error().message().find("closed"), std::string::npos) << sent.error().message();
  server.Stop();
}

TEST(BeastWebSocketTest, ALargeButLegalMessageRoundTrips) {
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::string big(1024 * 1024, 'm');
  ASSERT_TRUE((*dialed)->Send(Text("bulk", big)).ok());
  auto echo = (*dialed)->Receive();
  ASSERT_TRUE(echo.ok()) << echo.error().message();
  ASSERT_TRUE(echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:" + big);
  (*dialed)->Close();
  server.Stop();
}

TEST(BeastWebSocketTest, TheGateRefusesOverTlsToo) {
  // The refusal Respond arm on the TLS template instantiation.
  BeastServerTransport::Options options = EchoOptions();
  options.tls_certificate_chain_pem = smithy::testing::kTestCertificatePem;
  options.tls_private_key_pem = smithy::testing::kTestPrivateKeyPem;
  options.websocket_gate = [](const HttpRequest& request) -> std::optional<HttpResponse> {
    if (request.headers.Get("authorization").value_or("") != "Bearer let-me-in") {
      return PlainResponse(401, "who are you?");
    }
    return std::nullopt;
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  BeastWebSocketClient::Options dial;
  dial.host = "127.0.0.1";
  dial.port = server.port();
  dial.tls = true;
  dial.tls_options.ca_pem = smithy::testing::kTestCertificatePem;
  auto refused = BeastWebSocketClient::Dial(dial);
  EXPECT_FALSE(refused.ok());

  dial.headers.Add("authorization", "Bearer let-me-in");
  auto admitted = BeastWebSocketClient::Dial(dial);
  ASSERT_TRUE(admitted.ok()) << admitted.error().message();
  ASSERT_TRUE((*admitted)->Send(Text("chat", "hi")).ok());
  auto echo = (*admitted)->Receive();
  ASSERT_TRUE(echo.ok() && echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:hi");
  server.Stop();
}

// ---------------------------------------------------------------------------
// ADR-0019: the completion-driven primitives and the shared-session seam,
// over real WebSockets. The pair-based halves of the same contracts live in
// async_event_stream_test.cc.
// ---------------------------------------------------------------------------

TEST(BeastWebSocketTest, AsyncSendAndReceiveRoundTripOverTheWire) {
  // Declared before the session so a failed assertion below cannot leave a
  // parked callback pointing at a dead stack frame — client teardown fires
  // outstanding completions, and these must still be alive to catch them.
  std::promise<Outcome<std::optional<eventstream::Message>>> parked;
  std::promise<Outcome<std::optional<eventstream::Message>>> refused;

  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;
  ASSERT_TRUE(socket->SupportsAsync());

  for (int i = 0; i < 5; ++i) {
    std::promise<Outcome<Unit>> sent;
    socket->SendAsync(Text("chat", "async-" + std::to_string(i)),
                      [&sent](Outcome<Unit> outcome) { sent.set_value(std::move(outcome)); });
    ASSERT_TRUE(sent.get_future().get().ok());

    std::promise<Outcome<std::optional<eventstream::Message>>> received;
    socket->ReceiveAsync([&received](Outcome<std::optional<eventstream::Message>> message) {
      received.set_value(std::move(message));
    });
    auto echo = received.get_future().get();
    ASSERT_TRUE(echo.ok()) << echo.error().message();
    ASSERT_TRUE(echo->has_value());
    EXPECT_EQ((**echo).payload.ToString(), "echo:async-" + std::to_string(i));
  }

  // A second outstanding receive is refused inline while the first parks.
  socket->ReceiveAsync([&parked](Outcome<std::optional<eventstream::Message>> message) {
    parked.set_value(std::move(message));
  });
  socket->ReceiveAsync([&refused](Outcome<std::optional<eventstream::Message>> message) {
    refused.set_value(std::move(message));
  });
  auto refusal = refused.get_future().get();
  ASSERT_FALSE(refusal.ok());
  EXPECT_EQ(refusal.error().kind(), ErrorKind::kValidation);

  // Close completes the parked receive with the terminal outcome, and a
  // send-class op after the close is refused inline with Transport.
  socket->Close();
  auto end = parked.get_future().get();
  ASSERT_TRUE(end.ok()) << end.error().message();
  EXPECT_FALSE(end->has_value());
  std::promise<Outcome<Unit>> late;
  socket->SendAsync(Text("chat", "too-late"),
                    [&late](Outcome<Unit> outcome) { late.set_value(std::move(outcome)); });
  auto dead = late.get_future().get();
  ASSERT_FALSE(dead.ok());
  EXPECT_EQ(dead.error().kind(), ErrorKind::kTransport);
  server.Stop();
}

TEST(BeastWebSocketTest, ReceiveAsyncBehindABlockedReceiverIsRefused) {
  BeastServerTransport server(EchoOptions());
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  // A blocking receive parks (the echo server sends nothing unprompted)...
  Outcome<std::optional<eventstream::Message>> blocked = std::optional<eventstream::Message>();
  std::thread waiter([&] { blocked = socket->Receive(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(250));  // let it park

  // ...making an async receive the second outstanding receive-class op:
  // refused inline (websocket.h's mixed-API one-outstanding contract).
  auto refused = std::make_shared<std::promise<Outcome<std::optional<eventstream::Message>>>>();
  auto refusal_future = refused->get_future();
  socket->ReceiveAsync([refused](Outcome<std::optional<eventstream::Message>> message) {
    refused->set_value(std::move(message));
  });
  const bool refused_inline =
      refusal_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;

  // Unblock and join the waiter before asserting, so a regression fails
  // instead of hanging or terminating.
  socket->Close();
  waiter.join();
  ASSERT_TRUE(refused_inline) << "the second receive-class op parked instead of refusing";
  auto refusal = refusal_future.get();
  ASSERT_FALSE(refusal.ok());
  EXPECT_EQ(refusal.error().kind(), ErrorKind::kValidation);
  EXPECT_TRUE(!blocked.ok() || !blocked->has_value());  // the close's terminal outcome
  server.Stop();
}

TEST(BeastWebSocketTest, AsyncSendParkedOnAWedgedWireIsRefusedThenCompletedByClose) {
  // The Beast-side send matrix the pair cannot stand in for: a parked
  // SendAsync (wire wedged by a non-reading peer), the inline refusal of a
  // second send-class op behind it, and Close completing the parked one.
  std::promise<void> release_serve;
  std::shared_future<void> released = release_serve.get_future().share();
  BeastServerTransport::Options options;
  options.on_websocket = [released](const HttpRequest&, WebSocket&) {
    released.wait();  // never Receives: the client's sends back up
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const std::shared_ptr<WebSocket>& socket = *dialed;

  // Async-send until one fails to complete: that one is parked on the
  // wire. The callbacks own their promises (shared_ptr), so nothing here
  // dangles whichever way the test exits.
  const std::string bulk(1024 * 1024, 'x');
  std::future<Outcome<Unit>> parked_future;
  bool parked = false;
  for (int i = 0; i < 64 && !parked; ++i) {
    auto result = std::make_shared<std::promise<Outcome<Unit>>>();
    parked_future = result->get_future();
    socket->SendAsync(Text("bulk", bulk),
                      [result](Outcome<Unit> outcome) { result->set_value(std::move(outcome)); });
    if (parked_future.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
      parked = true;
    } else {
      ASSERT_TRUE(parked_future.get().ok());
    }
  }
  ASSERT_TRUE(parked) << "the wire never wedged; the setup is wrong";

  // The second send-class op behind the parked one refuses inline.
  auto refused = std::make_shared<std::promise<Outcome<Unit>>>();
  auto refusal_future = refused->get_future();
  socket->SendAsync(Text("chat", "one-too-many"),
                    [refused](Outcome<Unit> outcome) { refused->set_value(std::move(outcome)); });
  auto refusal = refusal_future.get();
  ASSERT_FALSE(refusal.ok());
  EXPECT_EQ(refusal.error().kind(), ErrorKind::kValidation);

  // Close escalates past the in-flight write and completes the parked
  // send with the terminal outcome — never leaves it hanging.
  socket->Close();
  ASSERT_EQ(parked_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "Close() left the parked SendAsync incomplete";
  auto outcome = parked_future.get();
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kTransport);

  release_serve.set_value();
  server.Stop();
}

TEST(BeastWebSocketTest, TheGateGuardsTheSharedSeamToo) {
  BeastServerTransport::Options options;
  options.handler_threads = 1;
  std::atomic<int> launched{0};
  options.websocket_gate = [](const HttpRequest& request) -> std::optional<HttpResponse> {
    if (request.target == "/allowed") return std::nullopt;
    HttpResponse refusal;
    refusal.status = 403;
    return refusal;
  };
  options.on_websocket_session = [&launched](const HttpRequest&,
                                             std::shared_ptr<WebSocket> socket) {
    ++launched;
    socket->Close();
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  // A refused target never reaches the launch callback — the gate answers
  // before any session exists, exactly as on the borrowed seam.
  EXPECT_FALSE(
      BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port(), .target = "/nope"})
          .ok());
  EXPECT_EQ(launched.load(), 0);

  auto accepted = BeastWebSocketClient::Dial(
      {.host = "127.0.0.1", .port = server.port(), .target = "/allowed"});
  ASSERT_TRUE(accepted.ok()) << accepted.error().message();
  for (int i = 0; i < 500 && launched.load() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(launched.load(), 1);
  server.Stop();
}

TEST(BeastWebSocketTest, TheSharedSeamAloneStillNeedsHandlerThreads) {
  BeastServerTransport::Options options;
  options.handler_threads = 0;
  options.on_websocket_session = [](const HttpRequest&, std::shared_ptr<WebSocket>) {};
  BeastServerTransport server(options);
  auto started = server.Start(NotFoundHandler());
  ASSERT_FALSE(started.ok());
  EXPECT_EQ(started.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(started.error().message().find("handler_threads"), std::string::npos);
}

TEST(BeastWebSocketTest, TheSharedSessionSeamServesWithoutParkingAHandlerThread) {
  // One handler thread, many sessions: with the borrowed seam this would
  // deadlock past the first session; the shared seam launches a Detached
  // echo loop per session and returns immediately (ADR-0019).
  BeastServerTransport::Options options;
  options.handler_threads = 1;
  std::atomic<int> launched{0};
  std::atomic<int> ended{0};  // the server-side loops observably unwind
  options.on_websocket_session = [&launched, &ended](const HttpRequest&,
                                                     std::shared_ptr<WebSocket> socket) {
    ++launched;
    [](std::shared_ptr<WebSocket> session, std::atomic<int>* ended) -> eventstream::Detached {
      eventstream::AsyncEventStream<eventstream::Message, eventstream::Message> stream(
          std::move(session), [](const eventstream::Message& message) { return message; },
          [](const eventstream::Message& message) { return message; });
      while (true) {
        auto message = co_await stream.Receive();
        if (!message.ok() || !message->has_value()) break;
        eventstream::Message reply = **message;
        reply.payload = Blob::FromString("echo:" + (*message)->payload.ToString());
        auto sent = co_await stream.Send(reply);
        if (!sent.ok()) break;
      }
      ++*ended;
    }(std::move(socket), &ended);
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFoundHandler()).ok());

  // Three concurrent sessions on the one-thread pool, interleaved.
  std::vector<std::shared_ptr<WebSocket>> clients;
  for (int i = 0; i < 3; ++i) {
    auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
    ASSERT_TRUE(dialed.ok()) << dialed.error().message();
    clients.push_back(*dialed);
  }
  for (int round = 0; round < 3; ++round) {
    for (std::size_t i = 0; i < clients.size(); ++i) {
      const std::string body = "s" + std::to_string(i) + "-r" + std::to_string(round);
      ASSERT_TRUE(clients[i]->Send(Text("chat", body)).ok());
      auto echo = clients[i]->Receive();
      ASSERT_TRUE(echo.ok() && echo->has_value());
      EXPECT_EQ((**echo).payload.ToString(), "echo:" + body);
    }
  }
  EXPECT_EQ(launched.load(), 3);

  // A client close ends its loop — the coroutine frame unwinds, not just
  // the client's view of the wire — and the others keep serving.
  clients[0]->Close();
  for (int i = 0; i < 500 && ended.load() < 1; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(ended.load(), 1);
  ASSERT_TRUE(clients[1]->Send(Text("chat", "still-here")).ok());
  auto still = clients[1]->Receive();
  ASSERT_TRUE(still.ok() && still->has_value());

  // Stop() aborts the shared sessions through the same sweep as borrowed
  // ones: the remaining clients observe their sessions ending AND the
  // server-side loops all unwind (a sweep that missed a parked receive
  // would leak its coroutine frame forever).
  server.Stop();
  auto after_stop = clients[2]->Receive();
  EXPECT_TRUE(!after_stop.ok() || !after_stop->has_value());
  for (int i = 0; i < 500 && ended.load() < 3; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(ended.load(), 3);
}

TEST(BeastWebSocketTest, BothServeSeamsSetIsRefusedAtStart) {
  BeastServerTransport::Options options = EchoOptions();
  options.on_websocket_session = [](const HttpRequest&, std::shared_ptr<WebSocket>) {};
  BeastServerTransport server(options);
  auto started = server.Start(NotFoundHandler());
  ASSERT_FALSE(started.ok());
  EXPECT_EQ(started.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(started.error().message().find("cannot both be set"), std::string::npos);
}

// ---------------------------------------------------------------------------
// The shared WebSocket contract (websocket_contract_test.h), Beast half.
// ---------------------------------------------------------------------------

// The Beast driver: a real loopback session whose peer handshakes and never
// reads, so writes back up in the socket buffers and a send eventually
// parks. The session arrives on a handler thread through the ADR-0019
// shared seam, so Socket() waits for it.
//
// EndSessionFromPeer sends a text frame on the binary wire: the protocol
// violation drives FailAndClose, which takes both parked waiters at once.
// A real close frame would too, but Beast's close op drains the peer's
// queued frames as part of the handshake — which unwedges the parked write
// and races the transition away. The violation has no such race.
struct BeastContractDriver {
  static constexpr int kWedgeAttempts = 64;

  BeastContractDriver() {
    BeastServerTransport::Options options;
    options.handler_threads = 1;
    auto arrived = arrived_;
    options.on_websocket_session = [arrived](const HttpRequest&,
                                             std::shared_ptr<WebSocket> socket) {
      arrived->set_value(std::move(socket));
    };
    server_ = std::make_unique<BeastServerTransport>(options);
    EXPECT_TRUE(server_->Start(NotFoundHandler()).ok());
    peer_ = std::make_unique<RawWsPeer>(server_->port());
  }

  ~BeastContractDriver() {
    peer_.reset();
    server_->Stop();
  }

  BeastContractDriver(const BeastContractDriver&) = delete;
  BeastContractDriver& operator=(const BeastContractDriver&) = delete;

  std::shared_ptr<WebSocket> Socket() {
    auto ready = arrived_->get_future();
    if (ready.wait_for(std::chrono::seconds(10)) != std::future_status::ready) {
      // A bare EXPECT would fall through to get() and block forever — the
      // hang this whole suite exists to turn into a legible failure.
      ADD_FAILURE() << "the session never reached the shared seam";
      std::abort();
    }
    return ready.get();
  }

  // Eight mebibytes a go — one message larger than any loopback's socket
  // buffers, so the wedge is decisive rather than a race against how fast
  // the kernel drains. A megabyte was not: macOS absorbed 1 MiB writes for
  // 30s straight without ever parking, and the 2s-per-attempt detector in
  // WedgeThenPark read those slow-but-progressing writes as parked ones.
  // Sized against the 16 MiB frame limit, with room for the envelope.
  eventstream::Message BulkMessage(int /*n*/) {
    return Text("bulk", std::string(8 * 1024 * 1024, 'x'));
  }

  void EndSessionFromPeer() { peer_->SendText("boom"); }

  std::shared_ptr<std::promise<std::shared_ptr<WebSocket>>> arrived_ =
      std::make_shared<std::promise<std::shared_ptr<WebSocket>>>();
  std::unique_ptr<BeastServerTransport> server_;
  std::unique_ptr<RawWsPeer> peer_;
};

}  // namespace
}  // namespace smithy::http

// The instantiation lives where the suite was registered: gtest builds the
// registration symbols from the bare suite name, so a qualified one does
// not resolve.
namespace smithy::testing {
INSTANTIATE_TYPED_TEST_SUITE_P(Beast, WebSocketContractTest, http::BeastContractDriver);
}  // namespace smithy::testing
