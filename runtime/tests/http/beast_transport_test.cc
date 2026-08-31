#include "smithy/http/beast_transport.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "smithy/http/socket_transport.h"
#include "smithy/server/metrics.h"
#include "smithy/server/middleware.h"
#include "smithy/testing/connection_event_recorder.h"

namespace smithy::http {
namespace {

using smithy::testing::ConnectionEventRecorder;

// Opens a loopback connection to `port` with a bounded receive timeout (and
// SIGPIPE disarmed where SO_NOSIGPIPE exists); returns the fd, or -1.
int ConnectLoopback(int port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  timeval timeout{.tv_sec = 10, .tv_usec = 0};
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
  const int no_sigpipe = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

// Fans out `clients` concurrent SocketHttpClients, each POSTing /echo with a
// distinct body; returns how many did not get their body echoed back.
int FanOutEchoFailures(int port, int clients) {
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  threads.reserve(static_cast<std::size_t>(clients));
  for (int i = 0; i < clients; ++i) {
    threads.emplace_back([&failures, port, i] {
      SocketHttpClient client("127.0.0.1", port);
      HttpRequest request;
      request.method = "POST";
      request.target = "/echo";
      request.body = "client-" + std::to_string(i);
      const auto response = client.Send(request);
      if (!response.ok() || response->body != request.body) {
        ++failures;
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  return failures.load();
}

// Sends raw bytes and returns the raw response so assertions can see the
// exact wire framing (a parsing client would mask duplicate headers).
std::string RawRoundTrip(int port, const std::string& request_bytes) {
  const int fd = ConnectLoopback(port);
  if (fd < 0) return {};
  (void)::send(fd, request_bytes.data(), request_bytes.size(), 0);
  std::string received;
  char scratch[1024];
  for (;;) {
    const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
    if (n <= 0) break;
    received.append(scratch, static_cast<std::size_t>(n));
  }
  ::close(fd);
  return received;
}

std::string AsciiLowerCopy(std::string text) {
  for (char& c : text) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return text;
}

std::size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0;
  for (auto pos = haystack.find(needle); pos != std::string::npos;
       pos = haystack.find(needle, pos + needle.size())) {
    ++count;
  }
  return count;
}

TEST(BeastTransportTest, RoundTripsOverRealSockets) {
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    HttpResponse response;
                    response.status = 200;
                    response.headers.Set("content-type", "text/plain");
                    response.headers.Set("x-method", request.method);
                    response.headers.Set("x-target", request.target);
                    response.headers.Set("x-probe",
                                         request.headers.Get("x-probe").value_or("missing"));
                    response.body = "echo:" + request.body;
                    return response;
                  })
                  .ok());
  ASSERT_GT(server.port(), 0);

  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "POST";
  request.target = "/cities/a%20b?pageSize=10";
  request.headers.Set("x-probe", "42");
  request.body = "hello beast";

  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->headers.Get("x-method"), "POST");
  EXPECT_EQ(response->headers.Get("x-target"), "/cities/a%20b?pageSize=10");
  EXPECT_EQ(response->headers.Get("x-probe"), "42");
  EXPECT_EQ(response->body, "echo:hello beast");

  server.Stop();
}

TEST(BeastTransportTest, StampsThePeerAddress) {
  BeastServerTransport server({.port = 0, .threads = 1});
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    HttpResponse response;
                    response.headers.Set("x-peer", request.peer_address);
                    return response;
                  })
                  .ok());
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "GET";
  request.target = "/";
  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  const std::string peer = response->headers.Get("x-peer").value_or("");
  EXPECT_EQ(peer.rfind("127.0.0.1:", 0), 0u) << peer;
  EXPECT_GT(peer.size(), std::string("127.0.0.1:").size()) << peer;  // a port follows
  server.Stop();
}

TEST(BeastTransportTest, StampsBracketedV6PeerAddresses) {
  BeastServerTransport server({.address = "::1", .port = 0, .threads = 1});
  auto started = server.Start([](const HttpRequest& request) {
    HttpResponse response;
    response.headers.Set("x-peer", request.peer_address);
    return response;
  });
  if (!started.ok()) {
    GTEST_SKIP() << "no IPv6 loopback here: " << started.error().message();
  }

  // Raw v6 dial (the test helpers and SocketHttpClient are v4-loopback only).
  const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  ASSERT_GE(fd, 0);
  sockaddr_in6 addr{};
  addr.sin6_family = AF_INET6;
  addr.sin6_addr = in6addr_loopback;
  addr.sin6_port = htons(static_cast<std::uint16_t>(server.port()));
  ASSERT_EQ(::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)), 0);
  const std::string request = "GET / HTTP/1.1\r\nhost: [::1]\r\nconnection: close\r\n\r\n";
  (void)::send(fd, request.data(), request.size(), 0);
  std::string received;
  char scratch[1024];
  for (;;) {
    const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
    if (n <= 0) break;
    received.append(scratch, static_cast<std::size_t>(n));
  }
  ::close(fd);
  EXPECT_NE(received.find("x-peer: [::1]:"), std::string::npos) << received;
  server.Stop();
}

TEST(BeastTransportTest, ServesConcurrentConnections) {
  // The ADR-0005 transport handled one connection at a time; this is the
  // regression test that Beast genuinely serves in parallel.
  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};
  BeastServerTransport server(BeastServerTransport::Options{.threads = 4});
  ASSERT_TRUE(server
                  .Start([&](const HttpRequest& request) {
                    const int now = ++in_flight;
                    int expected = max_in_flight.load();
                    while (now > expected && !max_in_flight.compare_exchange_weak(expected, now)) {
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    --in_flight;
                    return HttpResponse{200, {}, request.body};
                  })
                  .ok());

  EXPECT_EQ(FanOutEchoFailures(server.port(), 8), 0);
  server.Stop();
  EXPECT_GT(max_in_flight.load(), 1) << "requests were serialized";
}

TEST(BeastTransportTest, StripsHandlerSetFramingHeaders) {
  // The transport is authoritative for framing (issue #46): handler-set
  // framing headers must never reach the wire. A duplicate or conflicting
  // content-length / transfer-encoding pair is the classic request-smuggling
  // vector, and the connection token must state what the server actually
  // does. The socket server already strips these; this pins Beast to it.
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 200;
                    response.headers.Set("Content-Length", "999");
                    response.headers.Set("Transfer-Encoding", "chunked");
                    response.headers.Set("Connection", "keep-alive");
                    response.headers.Set("x-app", "kept");
                    response.body = "abc";
                    return response;
                  })
                  .ok());
  const std::string raw =
      RawRoundTrip(server.port(), "GET / HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  ASSERT_FALSE(raw.empty());
  const std::string wire = AsciiLowerCopy(raw);
  EXPECT_EQ(CountOccurrences(wire, "content-length:"), 1u) << raw;
  EXPECT_NE(wire.find("content-length: 3\r\n"), std::string::npos) << raw;
  EXPECT_EQ(wire.find("transfer-encoding"), std::string::npos) << raw;
  EXPECT_EQ(CountOccurrences(wire, "connection:"), 1u) << raw;
  EXPECT_EQ(wire.find("keep-alive"), std::string::npos) << raw;
  EXPECT_NE(wire.find("connection: close\r\n"), std::string::npos) << raw;
  EXPECT_NE(wire.find("x-app: kept\r\n"), std::string::npos) << raw;
  EXPECT_NE(wire.find("\r\n\r\nabc"), std::string::npos) << raw;
  server.Stop();
}

TEST(BeastTransportTest, AnInjectedResponseHeaderBecomesA500NotASplitResponse) {
  // The outbound injection defense (issue #109), at the same authority
  // point as the framing strip above. The handler models the real attack
  // shape: request-derived text that reached it DECODED (percent-decoding,
  // a JSON body, a database field) and is echoed into a header value. The
  // raw wire must carry one plain 500 — never a second field line, never
  // the smuggled set-cookie.
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 302;
                    response.headers.Set("location", "https://x/\r\nset-cookie: evil=1");
                    response.body = "redirecting";
                    return response;
                  })
                  .ok());
  const std::string raw =
      RawRoundTrip(server.port(), "GET / HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  ASSERT_FALSE(raw.empty());
  const std::string wire = AsciiLowerCopy(raw);
  EXPECT_NE(wire.find("http/1.1 500"), std::string::npos) << raw;
  EXPECT_EQ(wire.find("set-cookie"), std::string::npos) << raw;
  EXPECT_EQ(wire.find("location"), std::string::npos) << raw;
  EXPECT_EQ(wire.find("redirecting"), std::string::npos) << raw;  // body replaced too
  EXPECT_NE(wire.find("forbidden bytes"), std::string::npos) << raw;
  server.Stop();
}

TEST(BeastTransportTest, AnInjectedResponseHeaderNameAlsoBecomesA500) {
  // The value case above; this pins the name axis end to end — a CR/LF in a
  // header NAME must not split the response either.
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 200;
                    response.headers.Set("x-app\r\nset-cookie", "evil=1");
                    response.body = "ok";
                    return response;
                  })
                  .ok());
  const std::string raw =
      RawRoundTrip(server.port(), "GET / HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  ASSERT_FALSE(raw.empty());
  const std::string wire = AsciiLowerCopy(raw);
  EXPECT_NE(wire.find("http/1.1 500"), std::string::npos) << raw;
  EXPECT_EQ(wire.find("set-cookie"), std::string::npos) << raw;
  server.Stop();
}

TEST(BeastTransportTest, TheInjectionRejectingResponseFramesCorrectlyOnKeepAlive) {
  // The 500-replacement must recompute content-length for its own body and
  // keep the connection in sync — a second request on the same keep-alive
  // connection must still parse. A stale content-length (the original
  // body's) would desync the stream and hang or corrupt the next response.
  BeastServerTransport server({.threads = 2});
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    HttpResponse response;
                    response.status = 200;
                    if (request.target == "/inject") {
                      response.headers.Set("location", "https://x/\r\nset-cookie: evil=1");
                    }
                    response.body = request.target == "/inject" ? "redirecting" : "second-ok";
                    return response;
                  })
                  .ok());

  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);
  // First request trips the injection guard: a keep-alive 500.
  const std::string first = "GET /inject HTTP/1.1\r\nhost: x\r\n\r\n";
  ASSERT_EQ(::send(fd, first.data(), first.size(), 0), static_cast<ssize_t>(first.size()));
  std::string received;
  char scratch[512];
  // Read exactly the first response: headers, then content-length bytes.
  auto read_more = [&] {
    const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
    ASSERT_GT(n, 0) << "connection closed before the first response completed";
    received.append(scratch, static_cast<std::size_t>(n));
  };
  std::size_t header_end = std::string::npos;
  while ((header_end = received.find("\r\n\r\n")) == std::string::npos) read_more();
  const std::string lower = AsciiLowerCopy(received.substr(0, header_end));
  EXPECT_NE(lower.find("http/1.1 500"), std::string::npos) << received;
  EXPECT_EQ(lower.find("set-cookie"), std::string::npos) << received;
  // The content-length must match the replacement body — parse it and read
  // exactly that many body bytes, proving the framing is self-consistent.
  const std::size_t cl_pos = lower.find("content-length: ");
  ASSERT_NE(cl_pos, std::string::npos) << received;
  const std::size_t content_length = static_cast<std::size_t>(std::stoi(lower.substr(cl_pos + 16)));
  const std::size_t body_start = header_end + 4;
  while (received.size() < body_start + content_length) read_more();
  const std::string body = received.substr(body_start, content_length);
  EXPECT_NE(body.find("forbidden bytes"), std::string::npos) << body;

  // The connection is still framed correctly: a second request is served in
  // sync. If content-length had been wrong, this read would hang or mis-parse.
  const std::string second = "GET /next HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n";
  ASSERT_EQ(::send(fd, second.data(), second.size(), 0), static_cast<ssize_t>(second.size()));
  std::string tail = received.substr(body_start + content_length);
  while (tail.find("second-ok") == std::string::npos) {
    const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
    ASSERT_GT(n, 0) << "second request never served in sync: " << tail;
    tail.append(scratch, static_cast<std::size_t>(n));
  }
  EXPECT_NE(AsciiLowerCopy(tail).find("http/1.1 200"), std::string::npos) << tail;
  ::close(fd);
  server.Stop();
}

TEST(BeastTransportTest, LegitimateObsTextAndTabHeadersAreNotFalsePositives) {
  // The guard must not turn valid headers into 500s: HTAB and obs-text
  // (>= 0x80) are legal in field values and must pass through untouched.
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 200;
                    response.headers.Set("x-tabbed", "a\tb");
                    response.headers.Set("x-obs", "caf\xc3\xa9");  // UTF-8 é, obs-text
                    response.body = "ok";
                    return response;
                  })
                  .ok());
  const std::string raw =
      RawRoundTrip(server.port(), "GET / HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  ASSERT_FALSE(raw.empty());
  const std::string wire = AsciiLowerCopy(raw);
  EXPECT_NE(wire.find("http/1.1 200"), std::string::npos) << raw;
  EXPECT_NE(wire.find("x-tabbed: a\tb\r\n"), std::string::npos) << raw;
  EXPECT_NE(raw.find("caf\xc3\xa9"), std::string::npos) << raw;
  server.Stop();
}

TEST(BeastTransportTest, MaxConnectionsBoundsConcurrencyWithoutRejecting) {
  // Issue #46: at the cap the server pauses accepting — new connections wait
  // in the kernel's listen backlog until a session closes — rather than
  // rejecting or allocating unbounded per-connection state. Every client
  // still gets an answer; with the cap of one, handlers serialize even
  // though four io threads are available (without the cap this measures >1,
  // as ServesConcurrentConnections proves).
  std::atomic<int> in_flight{0};
  std::atomic<int> max_in_flight{0};
  BeastServerTransport server(BeastServerTransport::Options{.threads = 4, .max_connections = 1});
  ASSERT_TRUE(server
                  .Start([&](const HttpRequest& request) {
                    const int now = ++in_flight;
                    int expected = max_in_flight.load();
                    while (now > expected && !max_in_flight.compare_exchange_weak(expected, now)) {
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    --in_flight;
                    return HttpResponse{200, {}, request.body};
                  })
                  .ok());

  EXPECT_EQ(FanOutEchoFailures(server.port(), 6), 0);
  server.Stop();
  EXPECT_EQ(max_in_flight.load(), 1) << "a cap of one connection must serialize handlers";
}

TEST(BeastTransportTest, IdleKeepAliveSessionCannotPinTheCap) {
  // The cap's one starvation hazard: an idle keep-alive session holds a slot
  // without doing work. The idle read must expire on request_timeout_seconds
  // and free the slot, or cap + one lazy client = permanent starvation (the
  // production guide promises this).
  BeastServerTransport server(BeastServerTransport::Options{
      .threads = 2, .max_connections = 1, .request_timeout_seconds = 1});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{200, {}, "ok"}; }).ok());

  // Session 1: keep-alive request, then hold the connection open, idle.
  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);
  const std::string head = "GET / HTTP/1.1\r\nhost: x\r\n\r\n";  // HTTP/1.1: keep-alive
  ASSERT_EQ(::send(fd, head.data(), head.size(), 0), static_cast<ssize_t>(head.size()));
  std::string received;
  char scratch[512];
  while (received.find("\r\n\r\nok") == std::string::npos) {
    const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
    ASSERT_GT(n, 0) << "session 1 never got its response";
    received.append(scratch, static_cast<std::size_t>(n));
  }

  // Session 2 waits in the backlog until session 1's idle read times out
  // (~1s), then must be served. If the timeout didn't free the slot, this
  // Send would hang until the client gives up.
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{"GET", "/", {}, ""});
  ::close(fd);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  server.Stop();
}

TEST(BeastTransportTest, ZeroMaxConnectionsMeansUnlimited) {
  // 0 disables the cap; a flipped comparison would turn it into "never
  // accept", which this round trip would catch as a hang/timeout.
  BeastServerTransport server(BeastServerTransport::Options{.max_connections = 0});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{204, {}, ""}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{"GET", "/", {}, ""});
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 204);
  server.Stop();
}

TEST(BeastTransportTest, HandlesLargeBodies) {
  BeastServerTransport server;
  ASSERT_TRUE(
      server.Start([](const HttpRequest& request) { return HttpResponse{200, {}, request.body}; })
          .ok());
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "POST";
  request.target = "/echo";
  request.body = std::string(4 << 20, 'x');  // 4 MiB
  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body.size(), request.body.size());
  server.Stop();
}

TEST(BeastTransportTest, OversizedDeclaredBodyReadsA413) {
  // The transport writes this rejection itself, before a handler chain
  // exists, so Options::on_rejected is the only observation point (issue
  // #46) — it must fire with what the parser got to (headers were parsed:
  // method/target present) and the peer.
  std::mutex mutex;
  std::vector<BeastServerTransport::RejectedRequest> rejected;
  ConnectionEventRecorder events;  // must stay empty: on_rejected already
                                   // observed this connection (ADR-0013)
  BeastServerTransport server(
      BeastServerTransport::Options{.max_body_bytes = 1024,
                                    .on_rejected =
                                        [&](const BeastServerTransport::RejectedRequest& r) {
                                          const std::lock_guard<std::mutex> lock(mutex);
                                          rejected.push_back(r);
                                        },
                                    .on_connection_event = events.Hook()});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "POST";
  request.target = "/";
  request.body = std::string(64 * 1024, 'x');
  const auto response = client.Send(request);
  // Issue #94: a declared Content-Length over the limit is the deterministic,
  // cheap-to-reject case — the parser fails at end-of-headers and the client
  // must reliably read a 413 (not a connection abort). The bounded lingering
  // close is what keeps the response readable while the client finishes
  // writing.
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 413);
  EXPECT_EQ(response->headers.Get("connection"), "close");
  {
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(rejected.size(), 1u);
    EXPECT_EQ(rejected[0].status, 413);
    EXPECT_EQ(rejected[0].method, "POST");
    EXPECT_EQ(rejected[0].target, "/");
    EXPECT_EQ(rejected[0].peer_address.rfind("127.0.0.1:", 0), 0u) << rejected[0].peer_address;
  }
  {
    // The double-count rule: the over-limit path fires on_rejected only.
    const std::lock_guard<std::mutex> lock(events.mutex);
    EXPECT_TRUE(events.events.empty());
  }
  server.Stop();
}

TEST(BeastTransportTest, FramingGarbageIsObservedAsAConnectionEvent) {
  // Bytes that never parse into a request are invisible to Observe — the
  // connection-event hook is the only observation point (ADR-0013).
  ConnectionEventRecorder recorder;
  BeastServerTransport server(
      BeastServerTransport::Options{.on_connection_event = recorder.Hook()});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());

  EXPECT_EQ(RawRoundTrip(server.port(), "\x01\x02\x03\r\n\r\n"), "");  // closed, no response
  ASSERT_TRUE(recorder.WaitFor(1));
  {
    const std::lock_guard<std::mutex> lock(recorder.mutex);
    ASSERT_EQ(recorder.events.size(), 1u);
    EXPECT_EQ(recorder.events[0].kind, BeastServerTransport::ConnectionEvent::Kind::kFramingError);
    EXPECT_EQ(recorder.events[0].peer_address.rfind("127.0.0.1:", 0), 0u)
        << recorder.events[0].peer_address;
    EXPECT_FALSE(recorder.events[0].detail.empty());
  }
  server.Stop();
}

TEST(BeastTransportTest, AMidRequestDisconnectIsObservedAsDropped) {
  ConnectionEventRecorder recorder;
  BeastServerTransport server(
      BeastServerTransport::Options{.on_connection_event = recorder.Hook()});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());

  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);
  const std::string head = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc";
  ASSERT_EQ(::send(fd, head.data(), head.size(), 0), static_cast<ssize_t>(head.size()));
  ::close(fd);  // vanish mid-body

  ASSERT_TRUE(recorder.WaitFor(1));
  {
    const std::lock_guard<std::mutex> lock(recorder.mutex);
    ASSERT_EQ(recorder.events.size(), 1u);
    EXPECT_EQ(recorder.events[0].kind, BeastServerTransport::ConnectionEvent::Kind::kDropped);
    EXPECT_EQ(recorder.events[0].peer_address.rfind("127.0.0.1:", 0), 0u);
  }
  server.Stop();
}

TEST(BeastTransportTest, AStalledRequestIsObservedAsAReadTimeoutAndIdleIsSilent) {
  // Two connections against a 1-second request timeout: one sent part of a
  // request and stalled (the slowloris shape — observed, with the stall's
  // elapsed), one connected and sent nothing (indistinguishable from
  // healthy idle keep-alive reaping — deliberately silent, ADR-0013).
  ConnectionEventRecorder recorder;
  BeastServerTransport server(BeastServerTransport::Options{
      .request_timeout_seconds = 1, .on_connection_event = recorder.Hook()});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());

  const int idle = ConnectLoopback(server.port());
  ASSERT_GE(idle, 0);
  const int stalled = ConnectLoopback(server.port());
  ASSERT_GE(stalled, 0);
  const std::string partial = "POST / HTTP/1.1\r\nHost: x\r\n";
  ASSERT_EQ(::send(stalled, partial.data(), partial.size(), 0),
            static_cast<ssize_t>(partial.size()));

  ASSERT_TRUE(recorder.WaitFor(1));
  // Give the idle connection's reap a moment too, then assert it stayed
  // silent: exactly one event, the stall — and exactly the STALLED
  // connection's peer, pinned via its full local endpoint so an inverted
  // got_some gate (which would report the idle peer instead) cannot pass
  // on the shared prefix alone.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  sockaddr_in local{};
  socklen_t local_len = sizeof(local);
  ASSERT_EQ(::getsockname(stalled, reinterpret_cast<sockaddr*>(&local), &local_len), 0);
  const std::string stalled_peer = "127.0.0.1:" + std::to_string(ntohs(local.sin_port));
  {
    const std::lock_guard<std::mutex> lock(recorder.mutex);
    ASSERT_EQ(recorder.events.size(), 1u);
    EXPECT_EQ(recorder.events[0].kind, BeastServerTransport::ConnectionEvent::Kind::kReadTimeout);
    EXPECT_GE(recorder.events[0].elapsed, std::chrono::milliseconds(900));
    EXPECT_EQ(recorder.events[0].peer_address, stalled_peer);
  }
  ::close(idle);
  ::close(stalled);
  server.Stop();
}

// RST-closes `fd` (SO_LINGER{on,0}): the peer sees ECONNRESET on its next
// wire operation instead of a clean FIN.
void RstClose(int fd) {
  const linger hard{.l_onoff = 1, .l_linger = 0};
  (void)::setsockopt(fd, SOL_SOCKET, SO_LINGER, &hard, sizeof(hard));
  ::close(fd);
}

TEST(BeastTransportTest, APeerResetMidRequestIsObservedAsDropped) {
  // The transport-category arm of the classification (ADR-0013): an RST
  // mid-request surfaces as connection_reset — not partial_message — and
  // must classify kDropped, not kFramingError.
  ConnectionEventRecorder recorder;
  BeastServerTransport server(
      BeastServerTransport::Options{.on_connection_event = recorder.Hook()});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());

  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);
  const std::string head = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 10\r\n\r\nabc";
  ASSERT_EQ(::send(fd, head.data(), head.size(), 0), static_cast<ssize_t>(head.size()));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // let the read consume the bytes
  RstClose(fd);

  ASSERT_TRUE(recorder.WaitFor(1));
  const std::lock_guard<std::mutex> lock(recorder.mutex);
  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].kind, BeastServerTransport::ConnectionEvent::Kind::kDropped);
  EXPECT_FALSE(recorder.events[0].detail.empty());
  EXPECT_EQ(recorder.events[0].peer_address.rfind("127.0.0.1:", 0), 0u)
      << recorder.events[0].peer_address;
  server.Stop();
}

TEST(BeastTransportTest, APeerVanishingBeforeTheResponseWriteIsObservedAsDropped) {
  // The write-path event (ADR-0013): the handler is held until the client
  // has RST-closed, so the response write itself fails. The entered latch
  // makes the sequencing deterministic — the RST cannot land before the
  // read completed, so the read path cannot produce this event's kind for
  // the wrong reason (the pre-release emptiness check pins that).
  ConnectionEventRecorder recorder;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto released = std::make_shared<std::shared_future<void>>(release.get_future().share());
  BeastServerTransport server(
      BeastServerTransport::Options{.on_connection_event = recorder.Hook()});
  ASSERT_TRUE(server
                  .Start([&entered, released](const HttpRequest&) {
                    entered.set_value();  // the request was fully read
                    released->wait();
                    HttpResponse response;
                    response.body = std::string(64 * 1024, 'r');  // beat kernel buffering
                    return response;
                  })
                  .ok());

  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);
  const std::string request = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  ASSERT_EQ(::send(fd, request.data(), request.size(), 0), static_cast<ssize_t>(request.size()));
  entered_future.wait();  // deterministic: the read phase is over
  RstClose(fd);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));  // RST lands
  {
    const std::lock_guard<std::mutex> lock(recorder.mutex);
    EXPECT_TRUE(recorder.events.empty());  // nothing until the write runs
  }
  release.set_value();

  ASSERT_TRUE(recorder.WaitFor(1));
  const std::lock_guard<std::mutex> lock(recorder.mutex);
  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].kind, BeastServerTransport::ConnectionEvent::Kind::kDropped);
  EXPECT_FALSE(recorder.events[0].detail.empty());
  EXPECT_EQ(recorder.events[0].peer_address.rfind("127.0.0.1:", 0), 0u)
      << recorder.events[0].peer_address;
  server.Stop();
}

TEST(BeastTransportTest, FailuresDuringStopStaySilent) {
  // The stopping gate (ADR-0013): the same vanishing peer produces no
  // event when the write fails inside Stop()'s drain window — shutdown
  // cancellations are lifecycle, not incident. Sequencing is latched, not
  // slept: the handler-entered promise proves the read phase finished
  // before the RST, and a refused fresh connect proves Stop() closed the
  // acceptor (its first act after setting stopping) before the release.
  ConnectionEventRecorder recorder;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto released = std::make_shared<std::shared_future<void>>(release.get_future().share());
  BeastServerTransport server(BeastServerTransport::Options{
      .drain_timeout_seconds = 5, .on_connection_event = recorder.Hook()});
  ASSERT_TRUE(server
                  .Start([&entered, released](const HttpRequest&) {
                    entered.set_value();
                    released->wait();
                    HttpResponse response;
                    response.body = std::string(64 * 1024, 'r');
                    return response;
                  })
                  .ok());
  const int port = server.port();

  const int fd = ConnectLoopback(port);
  ASSERT_GE(fd, 0);
  const std::string request = "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n";
  ASSERT_EQ(::send(fd, request.data(), request.size(), 0), static_cast<ssize_t>(request.size()));
  entered_future.wait();
  RstClose(fd);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  std::thread stopper([&server] { server.Stop(); });
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    const int probe = ConnectLoopback(port);
    if (probe < 0) break;  // the acceptor is closed: Stop() is underway
    ::close(probe);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  release.set_value();  // handler finishes; the write fails while stopping
  stopper.join();

  const std::lock_guard<std::mutex> lock(recorder.mutex);
  EXPECT_TRUE(recorder.events.empty());
}

TEST(BeastTransportTest, ASlowHandlerStillWritesItsResponse) {
  // Each wire phase gets its own request_timeout_seconds budget (ADR-0013):
  // Beast expiries are absolute, so without Respond's re-arm a handler
  // outrunning the read deadline's residue had its write cancelled — and a
  // healthy, waiting peer misreported as kDropped.
  ConnectionEventRecorder recorder;
  BeastServerTransport server(BeastServerTransport::Options{
      .request_timeout_seconds = 1, .on_connection_event = recorder.Hook()});
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                    HttpResponse response;
                    response.body = "worth the wait";
                    return response;
                  })
                  .ok());

  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "POST";
  request.target = "/";
  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "worth the wait");
  {
    const std::lock_guard<std::mutex> lock(recorder.mutex);
    EXPECT_TRUE(recorder.events.empty());
  }
  server.Stop();
}

TEST(BeastTransportTest, HealthyLifecycleEmitsNoConnectionEvents) {
  // Silence means healthy (ADR-0013) — the signal must not scale with
  // traffic. Both healthy close shapes are exercised: a connection:close
  // request (the server closes first), and — the load-bearing one — a raw
  // KEEP-ALIVE connection whose client sends FIN at the message boundary,
  // so the server's re-armed read actually completes with end_of_stream.
  // SocketHttpClient alone cannot pin that arm: it always sends
  // connection: close, and the server then never reads the FIN.
  ConnectionEventRecorder recorder;
  BeastServerTransport server(
      BeastServerTransport::Options{.on_connection_event = recorder.Hook()});
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    HttpResponse response;
                    response.body = request.body;
                    return response;
                  })
                  .ok());
  {
    SocketHttpClient client("127.0.0.1", server.port());
    HttpRequest request;
    request.method = "POST";
    request.target = "/echo";
    request.body = "healthy";
    const auto response = client.Send(request);
    ASSERT_TRUE(response.ok()) << response.error().message();
    EXPECT_EQ(response->body, "healthy");
  }  // client destructor closes cleanly

  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);
  const std::string keep_alive = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";  // no connection: close
  ASSERT_EQ(::send(fd, keep_alive.data(), keep_alive.size(), 0),
            static_cast<ssize_t>(keep_alive.size()));
  char scratch[1024];
  ASSERT_GT(::recv(fd, scratch, sizeof(scratch), 0), 0);  // the response arrived
  ::close(fd);  // clean FIN at the message boundary -> end_of_stream server-side

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  server.Stop();
  const std::lock_guard<std::mutex> lock(recorder.mutex);
  EXPECT_TRUE(recorder.events.empty());
}

TEST(BeastTransportTest, AThrowingConnectionObserverIsContained) {
  // The observer must never take down the connection path it is watching:
  // after it throws on a garbage connection, the next request still serves.
  BeastServerTransport server(BeastServerTransport::Options{
      .on_connection_event = [](const BeastServerTransport::ConnectionEvent&) {
        throw std::runtime_error("observer bug");
      }});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());

  EXPECT_EQ(RawRoundTrip(server.port(), "\x01\x02\x03\r\n\r\n"), "");
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "POST";
  request.target = "/";
  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  server.Stop();
}

TEST(BeastTransportTest, AThrowingRejectionObserverIsContained) {
  // The observer must never take down the rejection path it is watching:
  // the client still reads its 413 when the hook throws.
  BeastServerTransport server(BeastServerTransport::Options{
      .max_body_bytes = 1024, .on_rejected = [](const BeastServerTransport::RejectedRequest&) {
        throw std::runtime_error("observer bug");
      }});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "POST";
  request.target = "/";
  request.body = std::string(64 * 1024, 'x');
  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 413);
  server.Stop();
}

TEST(BeastTransportTest, OversizedHeadersReadA431) {
  std::mutex mutex;
  std::vector<BeastServerTransport::RejectedRequest> rejected;
  BeastServerTransport server(BeastServerTransport::Options{
      .max_header_bytes = 1024, .on_rejected = [&](const BeastServerTransport::RejectedRequest& r) {
        const std::lock_guard<std::mutex> lock(mutex);
        rejected.push_back(r);
      }});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{200, {}, ""}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "GET";
  request.target = "/";
  request.headers.Set("x-huge", std::string(8 * 1024, 'h'));
  const auto response = client.Send(request);
  // Issue #94: header-limit violations answer 431 before the close.
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 431);
  EXPECT_EQ(response->headers.Get("connection"), "close");
  {
    // A 431 can fire mid-headers; the contract promises the status and peer,
    // while method/target carry whatever parsed.
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(rejected.size(), 1u);
    EXPECT_EQ(rejected[0].status, 431);
    EXPECT_EQ(rejected[0].peer_address.rfind("127.0.0.1:", 0), 0u) << rejected[0].peer_address;
  }
  server.Stop();
}

TEST(BeastTransportTest, MidStreamOverflowAnswersOrClosesButNeverHangs) {
  BeastServerTransport server(BeastServerTransport::Options{.max_body_bytes = 1024});
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());

  // Raw chunked upload with no declared length: the parser only discovers the
  // overflow mid-body. The client must end up with a 413 or a closed
  // connection within the drain budget — never a hang (issue #94).
  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);

#ifdef MSG_NOSIGNAL
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;
#endif
  const std::string head = "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n";
  (void)::send(fd, head.data(), head.size(), kSendFlags);
  const std::string chunk = "400\r\n" + std::string(1024, 'x') + "\r\n";  // 1 KiB per chunk
  for (int i = 0; i < 16; ++i) {  // 16 KiB total, well over the 1 KiB limit
    if (::send(fd, chunk.data(), chunk.size(), kSendFlags) < 0) {
      break;  // server already closed: acceptable, as long as we don't hang
    }
  }

  std::string received;
  char scratch[512];
  for (;;) {
    const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
    if (n <= 0) break;  // EOF, reset, or SO_RCVTIMEO — all bounded
    received.append(scratch, static_cast<std::size_t>(n));
  }
  ::close(fd);
  if (!received.empty()) {
    EXPECT_EQ(received.rfind("HTTP/1.1 413", 0), 0u) << received.substr(0, 64);
  }
  server.Stop();
}

TEST(BeastTransportTest, BlockedHandlersDoNotStarveTheIoPool) {
  // Issue #46: handlers run on their own executor (Options::handler_threads),
  // so even a single io thread keeps accepting connections and reading
  // requests while several handlers block concurrently. With handlers inline
  // on the io pool, threads = 1 would serialize them and this barrier could
  // never fill — each handler would report "starved" after its bounded wait.
  constexpr int kConcurrent = 3;
  std::atomic<int> waiting{0};
  BeastServerTransport server(BeastServerTransport::Options{.threads = 1});
  ASSERT_TRUE(
      server
          .Start([&](const HttpRequest&) {
            ++waiting;
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (waiting.load() < kConcurrent && std::chrono::steady_clock::now() < deadline) {
              std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            const bool all_blocked_together = waiting.load() >= kConcurrent;
            return HttpResponse{200, {}, all_blocked_together ? "ok" : "starved"};
          })
          .ok());

  std::vector<std::thread> clients;
  std::atomic<int> ok{0};
  clients.reserve(kConcurrent);
  for (int i = 0; i < kConcurrent; ++i) {
    clients.emplace_back([&] {
      SocketHttpClient client("127.0.0.1", server.port());
      const auto response = client.Send(HttpRequest{"GET", "/", {}, ""});
      if (response.ok() && response->body == "ok") {
        ++ok;
      }
    });
  }
  for (std::thread& thread : clients) {
    thread.join();
  }
  server.Stop();
  EXPECT_EQ(ok.load(), kConcurrent) << "handlers serialized on the io pool";
}

TEST(BeastTransportTest, ThrowingHandlerBecomesA500NotACrash) {
  // The #41 guard must hold on the executor too: an exception escaping into
  // an asio::thread_pool worker would std::terminate the process. Exercise
  // both dispatch modes — the pool (default) and inline (handler_threads=0).
  for (const int handler_threads : {16, 0}) {
    BeastServerTransport server(BeastServerTransport::Options{.handler_threads = handler_threads});
    ASSERT_TRUE(server
                    .Start([](const HttpRequest& request) -> HttpResponse {
                      if (request.target == "/boom") {
                        throw std::runtime_error("handler bug");
                      }
                      return HttpResponse{200, {}, "fine"};
                    })
                    .ok());
    SocketHttpClient client("127.0.0.1", server.port());
    const auto boom = client.Send(HttpRequest{"GET", "/boom", {}, ""});
    ASSERT_TRUE(boom.ok()) << boom.error().message();
    EXPECT_EQ(boom->status, 500) << "handler_threads=" << handler_threads;
    EXPECT_FALSE(boom->headers.Get("x-correlation-id").value_or("").empty());
    // The process and the server both survived: the next request serves.
    SocketHttpClient again("127.0.0.1", server.port());
    const auto ok = again.Send(HttpRequest{"GET", "/", {}, ""});
    ASSERT_TRUE(ok.ok()) << ok.error().message();
    EXPECT_EQ(ok->status, 200);
    server.Stop();
  }
}

TEST(BeastTransportTest, KeepAliveConnectionServesSequentialRequestsViaTheExecutor) {
  // Pins the Respond → ReadNext re-arm across the executor hop: two requests
  // on one keep-alive connection, both answered, in order.
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    return HttpResponse{200, {}, "echo:" + request.body};
                  })
                  .ok());
  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);
  std::string received;
  char scratch[512];
  for (const std::string body : {"one", "two"}) {
    const std::string request =
        "POST / HTTP/1.1\r\nhost: x\r\ncontent-length: " + std::to_string(body.size()) +
        "\r\n\r\n" + body;
    ASSERT_EQ(::send(fd, request.data(), request.size(), 0), static_cast<ssize_t>(request.size()));
    const std::string expected = "echo:" + body;
    while (received.find(expected) == std::string::npos) {
      const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
      ASSERT_GT(n, 0) << "no response for request body '" << body << "'";
      received.append(scratch, static_cast<std::size_t>(n));
    }
  }
  ::close(fd);
  EXPECT_LT(received.find("echo:one"), received.find("echo:two"));
  server.Stop();
}

TEST(BeastTransportTest, BurstBeyondHandlerPoolQueuesAndCompletes) {
  // More concurrent requests than handler threads: the excess queues on the
  // executor and every client is still answered — no deadlock, no rejection.
  BeastServerTransport server(BeastServerTransport::Options{.threads = 2, .handler_threads = 2});
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(30));
                    return HttpResponse{200, {}, request.body};
                  })
                  .ok());
  EXPECT_EQ(FanOutEchoFailures(server.port(), 6), 0);
  server.Stop();
}

TEST(BeastTransportTest, InlineHandlersStillServeWhenPoolDisabled) {
  // handler_threads = 0 opts back into inline dispatch on the io pool (no
  // executor hop) — the round trip must still work end to end.
  BeastServerTransport server(BeastServerTransport::Options{.handler_threads = 0});
  ASSERT_TRUE(
      server.Start([](const HttpRequest& request) { return HttpResponse{200, {}, request.body}; })
          .ok());
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{"POST", "/", {}, "inline"});
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "inline");
  server.Stop();
}

TEST(BeastTransportTest, StopDrainsInFlightRequests) {
  std::atomic<bool> handler_entered{false};
  BeastServerTransport server(BeastServerTransport::Options{.drain_timeout_seconds = 5});
  ASSERT_TRUE(server
                  .Start([&](const HttpRequest&) {
                    handler_entered = true;
                    std::this_thread::sleep_for(std::chrono::milliseconds(300));
                    return HttpResponse{200, {}, "drained"};
                  })
                  .ok());

  const int port = server.port();
  Outcome<HttpResponse> response = HttpResponse{};
  std::thread caller([&] {
    SocketHttpClient client("127.0.0.1", port);
    response = client.Send(HttpRequest{"GET", "/", {}, ""});
  });
  while (!handler_entered) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  // Stop while the handler is mid-request: the response must still be
  // written in full before the pool is torn down.
  server.Stop();
  caller.join();
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->body, "drained");
}

TEST(BeastTransportTest, StopWithWedgedHandlerReturnsWithinTheGrace) {
  // Issue #46's last drain gap: a handler that never returns must not wedge
  // Stop() forever. Past drain_timeout plus a short teardown grace the
  // transport abandons the stuck worker (deliberately leaking it — a thread
  // cannot be killed safely) and Stop() returns. Exercised in both dispatch
  // modes: a wedged pool worker and a wedged io thread behave the same.
  for (const int handler_threads : {16, 0}) {
    // Heap-allocated and captured by value: the abandoned handler thread may
    // outlive this loop iteration, so it must own its flags rather than
    // reference reused stack slots.
    auto release = std::make_shared<std::atomic<bool>>(false);
    auto entered = std::make_shared<std::atomic<bool>>(false);
    BeastServerTransport server(BeastServerTransport::Options{.handler_threads = handler_threads,
                                                              .drain_timeout_seconds = 0});
    ASSERT_TRUE(server
                    .Start([release, entered](const HttpRequest&) {
                      entered->store(true);
                      while (!release->load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                      }
                      return HttpResponse{200, {}, ""};
                    })
                    .ok());
    std::thread caller([port = server.port()] {
      SocketHttpClient client("127.0.0.1", port, /*timeout_ms=*/2000);
      (void)client.Send(HttpRequest{"GET", "/", {}, ""});  // times out; expected
    });
    while (!entered->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto begin = std::chrono::steady_clock::now();
    server.Stop();
    const auto elapsed = std::chrono::steady_clock::now() - begin;
    EXPECT_LT(elapsed, std::chrono::seconds(10))
        << "Stop() wedged on a stuck handler (handler_threads=" << handler_threads << ")";

    release->store(true);  // unwedge: the abandoned reaper finishes the cleanup
    caller.join();
    // Give the (unobservable, by design) detached reaper a beat to complete
    // its cleanup so leak checkers don't sample the deliberate leak mid-heal.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
}

TEST(BeastTransportTest, DestructorWithWedgedHandlerIsBoundedToo) {
  // The destructor rides the same Shutdown path as Stop(); the pre-#46 bug
  // statement was "blocks Stop()/destructor forever", so pin the destructor
  // half explicitly.
  auto release = std::make_shared<std::atomic<bool>>(false);
  auto entered = std::make_shared<std::atomic<bool>>(false);
  std::thread caller;
  const auto begin = std::chrono::steady_clock::now();
  {
    BeastServerTransport server(BeastServerTransport::Options{.drain_timeout_seconds = 0});
    ASSERT_TRUE(server
                    .Start([release, entered](const HttpRequest&) {
                      entered->store(true);
                      while (!release->load()) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                      }
                      return HttpResponse{200, {}, ""};
                    })
                    .ok());
    caller = std::thread([port = server.port()] {
      SocketHttpClient client("127.0.0.1", port, /*timeout_ms=*/2000);
      (void)client.Send(HttpRequest{"GET", "/", {}, ""});  // times out; expected
    });
    while (!entered->load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  }  // ~BeastServerTransport with the handler still wedged
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(10))
      << "destructor wedged on a stuck handler";
  release->store(true);
  caller.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let the reaper heal
}

TEST(BeastTransportTest, RestartAfterAbandonedTeardownServesFresh) {
  // After a wedged teardown leaks its State, the same transport object must
  // Start() cleanly with fresh state and serve — the leak is confined to
  // the abandoned generation.
  auto release = std::make_shared<std::atomic<bool>>(false);
  auto entered = std::make_shared<std::atomic<bool>>(false);
  BeastServerTransport server(BeastServerTransport::Options{.drain_timeout_seconds = 0});
  ASSERT_TRUE(server
                  .Start([release, entered](const HttpRequest&) {
                    entered->store(true);
                    while (!release->load()) {
                      std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    return HttpResponse{200, {}, ""};
                  })
                  .ok());
  std::thread caller([port = server.port()] {
    SocketHttpClient client("127.0.0.1", port, /*timeout_ms=*/2000);
    (void)client.Send(HttpRequest{"GET", "/", {}, ""});  // times out; expected
  });
  while (!entered->load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  server.Stop();  // abandons the wedged generation

  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{201, {}, "fresh"}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{"GET", "/", {}, ""});
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 201);
  EXPECT_EQ(response->body, "fresh");
  server.Stop();

  release->store(true);
  caller.join();
  std::this_thread::sleep_for(std::chrono::milliseconds(200));  // let the reaper heal
}

TEST(BeastTransportTest, StartupErrorsAreReported) {
  BeastServerTransport bad(BeastServerTransport::Options{.address = "not-an-address"});
  EXPECT_FALSE(bad.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());

  BeastServerTransport first;
  ASSERT_TRUE(first.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  BeastServerTransport conflict(BeastServerTransport::Options{.port = first.port()});
  EXPECT_FALSE(conflict.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  first.Stop();
}

TEST(BeastTransportTest, StopIsIdempotentAndRestartable) {
  BeastServerTransport server;
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{204, {}, ""}; }).ok());
  server.Stop();
  server.Stop();
  // A stopped transport can be started again (fresh state).
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{205, {}, ""}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{"GET", "/", {}, ""});
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 205);
  server.Stop();
}

// RFC 9110 §9.3.2: a HEAD response carries the headers the equivalent GET
// would carry and no body. Framing is the transport's call — a handler has no
// say in it — so this is where the method has to be read (issue #192).
//
// Raw bytes, not SocketHttpClient: that client stops after the headers of a
// HEAD response, so a parsed round-trip reports an empty body whether or not
// the server sent one. Only the wire can say.
TEST(BeastTransportTest, HeadResponsesCarryTheGetsLengthAndNoBody) {
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 200;
                    response.headers.Set("content-type", "application/json");
                    // The body a GET would produce. The handler cannot tell
                    // the difference, which is the point — a generated
                    // serializer has no method to branch on.
                    response.body = R"({"id":"abc"})";
                    return response;
                  })
                  .ok());

  const std::string raw =
      RawRoundTrip(server.port(), "HEAD /thing HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  ASSERT_FALSE(raw.empty());

  const auto header_end = raw.find("\r\n\r\n");
  ASSERT_NE(header_end, std::string::npos) << raw;
  EXPECT_EQ(raw.substr(header_end + 4), "") << "HEAD answered with a body: " << raw;

  // Not merely absent: the length the GET would report, so a client can size
  // the resource without fetching it. Content-Length: 0 would be a different
  // and false claim about it.
  const std::string headers = AsciiLowerCopy(raw.substr(0, header_end));
  EXPECT_NE(headers.find("content-length: 12"), std::string::npos) << raw;
  EXPECT_NE(headers.find("content-type: application/json"), std::string::npos) << raw;

  // The GET is untouched, which is what makes the length above meaningful.
  const std::string get =
      RawRoundTrip(server.port(), "GET /thing HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  const auto get_header_end = get.find("\r\n\r\n");
  ASSERT_NE(get_header_end, std::string::npos) << get;
  EXPECT_EQ(get.substr(get_header_end + 4), R"({"id":"abc"})") << get;

  server.Stop();
}

TEST(BeastTransportTest, TheMetricsEndpointScrapesOverTheRealTransport) {
  // The registry and the exposition are unit-tested; what only a real socket
  // proves is that a scrape survives the transport — the exposition's own
  // content type reaches the client, and the traffic counted is the traffic
  // the transport actually served.
  auto metrics = std::make_shared<smithy::server::MetricsRegistry>(
      smithy::server::MetricsOptions{.enabled = true, .service_name = "todo-service"});
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start(smithy::server::Chain({smithy::server::MetricsEndpoint(metrics),
                                                smithy::server::RecordMetrics(metrics)},
                                               [](const HttpRequest&) {
                                                 HttpResponse response;
                                                 response.status = 200;
                                                 response.operation = "GetThing";
                                                 response.body = "ok";
                                                 return response;
                                               }))
                  .ok());

  ASSERT_FALSE(
      RawRoundTrip(server.port(), "GET /thing HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n")
          .empty());

  const std::string scrape =
      RawRoundTrip(server.port(), "GET /metrics HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  const auto header_end = scrape.find("\r\n\r\n");
  ASSERT_NE(header_end, std::string::npos) << scrape;
  EXPECT_NE(AsciiLowerCopy(scrape.substr(0, header_end))
                .find("content-type: text/plain; version=0.0.4; charset=utf-8"),
            std::string::npos)
      << scrape;
  const std::string body = scrape.substr(header_end + 4);
  EXPECT_NE(
      body.find(
          R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="GetThing"} 1)"),
      std::string::npos)
      << body;
  // The scrape itself went through MetricsEndpoint, which sits outside
  // RecordMetrics — so it answered without counting itself.
  EXPECT_EQ(body.find(R"(operation="",status="200")"), std::string::npos) << body;

  server.Stop();
}

TEST(BeastTransportTest, AnOverLimitRejectionReachesTheMetricsScrape) {
  // The gap RecordMetrics cannot close on its own: the transport writes this
  // 413 before any handler chain exists, so middleware never sees it and an
  // over-limit flood would be invisible in the request counters. Wiring
  // on_rejected is what makes it visible, and only a real transport proves
  // the wiring — the rejection has no in-process caller to fake.
  auto metrics = std::make_shared<smithy::server::MetricsRegistry>(
      smithy::server::MetricsOptions{.enabled = true, .service_name = "todo-service"});
  BeastServerTransport server(BeastServerTransport::Options{
      .max_body_bytes = 1024, .on_rejected = smithy::server::RecordRejections(metrics)});
  ASSERT_TRUE(server
                  .Start(smithy::server::Chain({smithy::server::MetricsEndpoint(metrics),
                                                smithy::server::RecordMetrics(metrics)},
                                               [](const HttpRequest&) {
                                                 HttpResponse response;
                                                 response.status = 200;
                                                 response.operation = "GetThing";
                                                 return response;
                                               }))
                  .ok());

  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest oversized;
  oversized.method = "POST";
  oversized.target = "/upload";
  oversized.body = std::string(64 * 1024, 'x');
  const auto rejected = client.Send(oversized);
  ASSERT_TRUE(rejected.ok()) << rejected.error().message();
  ASSERT_EQ(rejected->status, 413);

  const std::string scrape =
      RawRoundTrip(server.port(), "GET /metrics HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  const auto header_end = scrape.find("\r\n\r\n");
  ASSERT_NE(header_end, std::string::npos) << scrape;
  const std::string body = scrape.substr(header_end + 4);
  EXPECT_NE(
      body.find(
          R"(http_server_requests_total{service_name="todo-service",http_method="POST",route="unmatched"} 1)"),
      std::string::npos)
      << body;
  // Counted, but not filed as a latency observation: a request refused at
  // parse time has no service latency, and zeros here would flatter the
  // panel during exactly the flood it should expose.
  EXPECT_EQ(
      body.find(
          R"(http_server_request_duration_microseconds_count{service_name="todo-service",http_method="POST",route="unmatched"})"),
      std::string::npos)
      << body;

  server.Stop();
}

TEST(BeastTransportTest, TheMetricsEndpointsHeadReportsTheGetsLength) {
  // Same framing hazard as the health endpoint below: MetricsEndpoint answers
  // HEAD itself, so it is on the handler to hand the transport a full body
  // and let the transport withhold the octets while keeping the length.
  auto metrics = std::make_shared<smithy::server::MetricsRegistry>(
      smithy::server::MetricsOptions{.enabled = true, .service_name = "todo-service"});
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start(smithy::server::Chain({smithy::server::MetricsEndpoint(metrics)},
                                               [](const HttpRequest&) {
                                                 HttpResponse response;
                                                 response.status = 404;
                                                 response.body = "no route";
                                                 return response;
                                               }))
                  .ok());

  const std::string head =
      RawRoundTrip(server.port(), "HEAD /metrics HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  const std::string get =
      RawRoundTrip(server.port(), "GET /metrics HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  const auto head_end = head.find("\r\n\r\n");
  const auto get_end = get.find("\r\n\r\n");
  ASSERT_NE(head_end, std::string::npos) << head;
  ASSERT_NE(get_end, std::string::npos) << get;

  EXPECT_EQ(head.substr(head_end + 4), "") << "HEAD answered with a body: " << head;
  const std::string expected_length = "content-length: " + std::to_string(get.size() - get_end - 4);
  EXPECT_NE(AsciiLowerCopy(head.substr(0, head_end)).find(expected_length), std::string::npos)
      << "HEAD did not report the GET's length: " << head;

  server.Stop();
}

TEST(BeastTransportTest, TheHealthEndpointsHeadReportsTheGetsLength) {
  // HealthEndpoint answers HEAD itself rather than routing it, so it is the
  // one shipped handler that can get the HEAD shape wrong on its own. Framing
  // belongs to the transport: a handler that empties the body to "omit" it
  // has not omitted anything, it has changed the length to 0 — a false answer
  // to the only question a HEAD asks.
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start(smithy::server::Chain({smithy::server::HealthEndpoint("/livez")},
                                               [](const HttpRequest&) {
                                                 HttpResponse response;
                                                 response.status = 404;
                                                 response.body = "no route";
                                                 return response;
                                               }))
                  .ok());

  const std::string head =
      RawRoundTrip(server.port(), "HEAD /livez HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  const std::string get =
      RawRoundTrip(server.port(), "GET /livez HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n");
  ASSERT_FALSE(head.empty());
  ASSERT_FALSE(get.empty());

  const auto head_end = head.find("\r\n\r\n");
  const auto get_end = get.find("\r\n\r\n");
  ASSERT_NE(head_end, std::string::npos) << head;
  ASSERT_NE(get_end, std::string::npos) << get;

  EXPECT_EQ(get.substr(get_end + 4), R"({"status":"healthy"})") << get;
  EXPECT_EQ(head.substr(head_end + 4), "") << "HEAD answered with a body: " << head;

  const std::string head_headers = AsciiLowerCopy(head.substr(0, head_end));
  EXPECT_NE(head_headers.find("content-length: 20"), std::string::npos)
      << "HEAD did not report the GET's length: " << head;
  EXPECT_NE(head_headers.find("content-type: application/json"), std::string::npos) << head;

  server.Stop();
}

// The failure a stray HEAD body actually causes. Sequential on one
// connection, the way StripsHandlerSetFramingHeaders checks the same
// property: read the HEAD response to its end, then ask for something else
// on the same socket. Any body the HEAD emitted is still queued, so it
// arrives where the second response should begin.
TEST(BeastTransportTest, AHeadLeavesTheConnectionInSync) {
  BeastServerTransport server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 200;
                    response.body = "0123456789";
                    return response;
                  })
                  .ok());

  const int fd = ConnectLoopback(server.port());
  ASSERT_GE(fd, 0);
  const std::string head = "HEAD /a HTTP/1.1\r\nhost: x\r\n\r\n";
  ASSERT_EQ(::send(fd, head.data(), head.size(), 0), static_cast<ssize_t>(head.size()));

  std::string received;
  char scratch[512];
  auto read_more = [&] {
    const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
    ASSERT_GT(n, 0) << "connection closed before the HEAD response completed: " << received;
    received.append(scratch, static_cast<std::size_t>(n));
  };
  std::size_t header_end = std::string::npos;
  while ((header_end = received.find("\r\n\r\n")) == std::string::npos) read_more();

  // The headers say ten bytes follow. None do, and the next thing on the
  // socket is the answer to the next request.
  EXPECT_NE(AsciiLowerCopy(received.substr(0, header_end)).find("content-length: 10"),
            std::string::npos)
      << received;
  EXPECT_EQ(received.substr(header_end + 4), "") << "HEAD answered with a body: " << received;

  const std::string get = "GET /b HTTP/1.1\r\nhost: x\r\nconnection: close\r\n\r\n";
  ASSERT_EQ(::send(fd, get.data(), get.size(), 0), static_cast<ssize_t>(get.size()));
  std::string tail;
  while (tail.find("0123456789") == std::string::npos) {
    const auto n = ::recv(fd, scratch, sizeof(scratch), 0);
    ASSERT_GT(n, 0) << "the GET was never served in sync: " << tail;
    tail.append(scratch, static_cast<std::size_t>(n));
  }
  // One response, one body: the GET's. Two would mean the HEAD sent one too.
  EXPECT_EQ(CountOccurrences(tail, "0123456789"), 1u) << tail;
  EXPECT_EQ(CountOccurrences(tail, "HTTP/1.1 200"), 1u) << tail;

  ::close(fd);
  server.Stop();
}

// Both structs are described to callers as having optional fields — a
// rejection whose request never parsed has no method or target, and a
// connection event whose socket is gone has no peer address. Saying that by
// omission has to compile.
//
// This is a compile-time assertion wearing a test's clothes: under
// --config=werror, dropping the `= {}` from either struct makes these
// designated initializers a -Wmissing-designated-field-initializers error and
// the target fails to build (issue #193). The runtime EXPECTs are here so the
// test also states what the omitted fields are worth.
TEST(BeastTransportTest, OptionalFieldsCanBeOmittedFromDesignatedInitializers) {
  const BeastServerTransport::RejectedRequest rejected{.status = 431};
  EXPECT_EQ(rejected.status, 431);
  EXPECT_TRUE(rejected.peer_address.empty());
  EXPECT_TRUE(rejected.method.empty());
  EXPECT_TRUE(rejected.target.empty());

  const BeastServerTransport::ConnectionEvent event{
      .kind = BeastServerTransport::ConnectionEvent::Kind::kFramingError};
  EXPECT_EQ(event.kind, BeastServerTransport::ConnectionEvent::Kind::kFramingError);
  EXPECT_TRUE(event.peer_address.empty());
  EXPECT_TRUE(event.detail.empty());
  EXPECT_EQ(event.elapsed, std::chrono::microseconds{0});
}

}  // namespace
}  // namespace smithy::http
