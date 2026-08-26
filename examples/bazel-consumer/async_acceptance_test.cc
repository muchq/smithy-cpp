// Out-of-tree acceptance for completion-driven event streams (ADR-0019):
// the async socket primitives, the shared-session serve seam, the
// coroutine adapter, and async-delivery fan-out all compile and run
// against the runtime targets alone, through the module boundary — the
// wiring a thread-free hub uses beneath the generated async surface (ADR-0021).

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "smithy/eventstream/async_event_stream.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/beast_transport.h"
#include "smithy/http/websocket.h"
#include "smithy/server/session_registry.h"

namespace {

using smithy::Outcome;
using smithy::Unit;
using smithy::eventstream::AsyncEventStream;
using smithy::eventstream::Detached;
using smithy::eventstream::Message;
using smithy::http::BeastServerTransport;
using smithy::http::BeastWebSocketClient;
using smithy::http::HttpRequest;
using smithy::http::HttpResponse;
using smithy::http::WebSocket;

Message Event(const std::string& kind, const std::string& body) {
  return Message{.headers = {{":event-type", kind}}, .payload = smithy::Blob::FromString(body)};
}

// The identity codec: the adapter's templates instantiate in a consumer
// toolchain with no generated code in the loop.
Outcome<Message> Identity(const Message& message) { return message; }

HttpResponse NotFound(const HttpRequest&) {
  HttpResponse response;
  response.status = 404;
  return response;
}

TEST(AsyncAcceptanceTest, TheAsyncPrimitivesRoundTripThroughTheModuleBoundary) {
  BeastServerTransport::Options options;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) return;
      if (!socket.Send(Event("echo", "echo:" + (*message)->payload.ToString())).ok()) return;
    }
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFound).ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();
  const auto& socket = *dialed;
  ASSERT_TRUE(socket->SupportsAsync());

  std::promise<Outcome<Unit>> sent;
  socket->SendAsync(Event("chat", "hello"),
                    [&sent](Outcome<Unit> outcome) { sent.set_value(std::move(outcome)); });
  ASSERT_TRUE(sent.get_future().get().ok());

  std::promise<Outcome<std::optional<Message>>> received;
  socket->ReceiveAsync([&received](Outcome<std::optional<Message>> message) {
    received.set_value(std::move(message));
  });
  auto echo = received.get_future().get();
  ASSERT_TRUE(echo.ok()) << echo.error().message();
  ASSERT_TRUE(echo->has_value());
  EXPECT_EQ((**echo).payload.ToString(), "echo:hello");

  socket->Close();
  server.Stop();
}

// One session's whole life on completion contexts: registered for fan-out,
// echoing until the client closes, then gone from the registry.
Detached Serve(smithy::server::SessionRegistry<Message>& registry, std::string id,
               std::shared_ptr<WebSocket> socket) {
  AsyncEventStream<Message, Message> stream(std::move(socket), Identity, Identity);
  if (!registry.Add(id, stream.Share())) {
    stream.Close();
    co_return;
  }
  while (true) {
    auto message = co_await stream.Receive();
    if (!message.ok() || !message->has_value()) break;
    (void)co_await stream.Send(Event("echo", "echo:" + (**message).payload.ToString()));
  }
  registry.Remove(id);
  stream.Close();
}

// The #130 watchdog, out of tree and over a real socket: a hand-written
// Detached loop — the deadline's stated audience — bounds its await
// against a peer that says nothing unsolicited, treats the timeout as a
// tick rather than an end, and the same coroutine still serves the
// message that arrives after one.
TEST(AsyncAcceptanceTest, AnAwaitedReceiveDeadlineTicksWithoutEndingTheSession) {
  BeastServerTransport::Options options;
  options.on_websocket = [](const HttpRequest&, WebSocket& socket) {
    while (true) {
      auto message = socket.Receive();
      if (!message.ok() || !message->has_value()) return;
      if (!socket.Send(Event("echo", "echo:" + (*message)->payload.ToString())).ok()) return;
    }
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFound).ok());
  auto dialed = BeastWebSocketClient::Dial({.host = "127.0.0.1", .port = server.port()});
  ASSERT_TRUE(dialed.ok()) << dialed.error().message();

  std::promise<std::string> first_timeout_code;
  std::promise<std::string> echoed;
  [](std::shared_ptr<WebSocket> socket, std::promise<std::string>* timeout_code,
     std::promise<std::string>* echoed) -> Detached {
    AsyncEventStream<Message, Message> stream(std::move(socket), Identity, Identity);
    // Quiet wire: the bounded await resolves with the timeout, terminally
    // for nothing — the session and the loop both continue.
    auto nothing = co_await stream.Receive(std::chrono::milliseconds(75));
    timeout_code->set_value(nothing.ok() ? "ok" : nothing.error().code());
    (void)co_await stream.Send(Event("chat", "after the deadline"));
    auto echo = co_await stream.Receive(std::chrono::seconds(10));
    echoed->set_value(echo.ok() && echo->has_value() ? (**echo).payload.ToString() : "<no echo>");
    stream.Close();
  }(*dialed, &first_timeout_code, &echoed);

  auto code = first_timeout_code.get_future();
  ASSERT_EQ(code.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(code.get(), "TimeoutError");

  auto reply = echoed.get_future();
  ASSERT_EQ(reply.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(reply.get(), "echo:after the deadline");
  server.Stop();
}

TEST(AsyncAcceptanceTest, ThreeSessionsShareOneHandlerThreadAndAFanOutRegistry) {
  // Declared before the transport on purpose: sessions reference the
  // registry from their coroutines, so it must outlive them.
  smithy::server::SessionRegistry<Message>::Options fanout;
  fanout.async_delivery = true;  // completion chains, no writer threads
  smithy::server::SessionRegistry<Message> registry(fanout);

  BeastServerTransport::Options options;
  options.handler_threads = 1;  // launches only — the borrowed seam would wedge here
  options.on_websocket_session = [&registry](const HttpRequest& request,
                                             std::shared_ptr<WebSocket> socket) {
    Serve(registry, request.target, std::move(socket));  // returns immediately
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(NotFound).ok());

  std::vector<std::shared_ptr<WebSocket>> clients;
  for (int i = 0; i < 3; ++i) {
    auto dialed = BeastWebSocketClient::Dial(
        {.host = "127.0.0.1", .port = server.port(), .target = "/session/" + std::to_string(i)});
    ASSERT_TRUE(dialed.ok()) << dialed.error().message();
    clients.push_back(*dialed);
  }

  // An echo round trip per client: three coroutine sessions live at once
  // on the one handler thread, each registered before its receive loop.
  for (std::size_t i = 0; i < clients.size(); ++i) {
    const std::string body = "consumer-" + std::to_string(i);
    ASSERT_TRUE(clients[i]->Send(Event("chat", body)).ok());
    auto echo = clients[i]->Receive();
    ASSERT_TRUE(echo.ok()) << echo.error().message();
    ASSERT_TRUE(echo->has_value());
    EXPECT_EQ((**echo).payload.ToString(), "echo:" + body);
  }

  // One broadcast reaches every session through its Share() handle, on
  // async completion chains.
  ASSERT_EQ(registry.size(), 3U);
  registry.Broadcast(registry.Ids(), Event("news", "fan-out"));
  for (auto& client : clients) {
    auto news = client->Receive();
    ASSERT_TRUE(news.ok()) << news.error().message();
    ASSERT_TRUE(news->has_value());
    EXPECT_EQ((**news).payload.ToString(), "fan-out");
  }

  // Closing the clients ends the coroutines, which remove themselves.
  for (auto& client : clients) client->Close();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (registry.size() != 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(registry.size(), 0U);
  server.Stop();
}

}  // namespace
