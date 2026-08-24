#include "smithy/http/socket_transport.h"

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include "smithy/http/trace_context.h"

namespace smithy::http {
namespace {

// Sends raw bytes to the loopback server and returns the raw response, so an
// assertion can see the exact wire framing. SocketHttpClient would not do:
// it knows a HEAD response has no body and stops after the headers, so a
// parsed round-trip reports an empty body whether or not the server sent one.
std::string RawRoundTrip(int port, const std::string& request_bytes) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return {};
  timeval timeout{.tv_sec = 10, .tv_usec = 0};
  (void)::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<std::uint16_t>(port));
  if (::inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr) != 1 ||
      ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    ::close(fd);
    return {};
  }
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

TEST(SocketTransportTest, RoundTripsOverRealSockets) {
  SocketHttpServer server;
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
  request.body = "hello over tcp";

  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->headers.Get("x-method"), "POST");
  EXPECT_EQ(response->headers.Get("x-target"), "/cities/a%20b?pageSize=10");
  EXPECT_EQ(response->headers.Get("x-probe"), "42");
  EXPECT_EQ(response->body, "echo:hello over tcp");

  server.Stop();
}

TEST(SocketTransportTest, ThrowingHandlerBecomesA500NotACrash) {
  // The exception escapes the handler on the transport's own thread; before the
  // guard this unwound out of the accept loop and terminated the process. The
  // server must instead answer 500 and stay up for the next request.
  SocketHttpServer server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) -> HttpResponse {
                    if (request.target == "/boom") {
                      throw std::runtime_error("handler blew up");
                    }
                    return HttpResponse{200, {}, "ok"};
                  })
                  .ok());
  SocketHttpClient client("127.0.0.1", server.port());

  HttpRequest boom;
  boom.target = "/boom";
  const auto failed = client.Send(boom);
  ASSERT_TRUE(failed.ok()) << failed.error().message();
  EXPECT_EQ(failed->status, 500);
  EXPECT_FALSE(failed->headers.Get("x-correlation-id").value_or("").empty());

  // The server survived: a subsequent request still succeeds.
  HttpRequest fine;
  fine.target = "/ok";
  const auto ok = client.Send(fine);
  ASSERT_TRUE(ok.ok()) << ok.error().message();
  EXPECT_EQ(ok->status, 200);
  EXPECT_EQ(ok->body, "ok");

  server.Stop();
}

TEST(SocketTransportTest, HandlesSequentialRequestsAndLargeBodies) {
  SocketHttpServer server;
  ASSERT_TRUE(
      server.Start([](const HttpRequest& request) { return HttpResponse{200, {}, request.body}; })
          .ok());
  SocketHttpClient client("127.0.0.1", server.port());

  const std::string large(1 << 20, 'x');  // 1 MiB
  for (int i = 0; i < 3; ++i) {
    HttpRequest request;
    request.method = "POST";
    request.target = "/echo";
    request.body = large;
    const auto response = client.Send(request);
    ASSERT_TRUE(response.ok()) << response.error().message();
    EXPECT_EQ(response->body.size(), large.size());
  }
  server.Stop();
}

TEST(SocketTransportTest, StripsHandlerSetFramingHeaders) {
  // The transport is authoritative for framing (issue #46): it already strips
  // handler-set content-length/connection, but a handler-set
  // transfer-encoding next to the transport's own content-length is the
  // classic request-smuggling pair and must be dropped too.
  SocketHttpServer server;
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
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{"GET", "/", {}, ""});
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->body, "abc");
  EXPECT_FALSE(response->headers.Has("transfer-encoding"));
  EXPECT_EQ(response->headers.Get("content-length"), "3");
  EXPECT_EQ(response->headers.Get("connection"), "close");
  EXPECT_EQ(response->headers.Get("x-app"), "kept");
  server.Stop();
}

TEST(SocketTransportTest, AnInjectedResponseHeaderBecomesA500NotASplitResponse) {
  // The outbound injection defense (issue #109): a handler echoing
  // CR/LF-bearing text into a header must not split the response. The
  // transport replaces the whole response with fixed text.
  SocketHttpServer server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 302;
                    response.headers.Set("location", "https://x/\r\nset-cookie: evil=1");
                    response.body = "redirecting";
                    return response;
                  })
                  .ok());
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{});
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 500);
  EXPECT_FALSE(response->headers.Has("set-cookie"));  // nothing split through
  EXPECT_FALSE(response->headers.Has("location"));
  EXPECT_NE(response->body.find("forbidden bytes"), std::string::npos);
  server.Stop();
}

TEST(SocketTransportTest, LegitimateObsTextAndTabHeadersAreNotFalsePositives) {
  // The guard must not turn valid headers into 500s: HTAB and obs-text
  // (>= 0x80) are legal in field values and must round-trip untouched.
  SocketHttpServer server;
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
  SocketHttpClient client("127.0.0.1", server.port());
  const auto response = client.Send(HttpRequest{});
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->headers.Get("x-tabbed").value_or(""), "a\tb");
  EXPECT_EQ(response->headers.Get("x-obs").value_or(""), "caf\xc3\xa9");
  server.Stop();
}

TEST(SocketTransportTest, AClientRequestWithInjectedHeaderIsRefusedBeforeConnecting) {
  // Port 1 is never dialed: the refusal happens before any socket work, so
  // no listener is needed for this to fail fast with Validation.
  SocketHttpClient client("127.0.0.1", 1);
  HttpRequest request;
  request.headers.Set("x-echo", "a\r\nx-evil: b");
  const auto outcome = client.Send(request);
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(outcome.error().message().find("x-echo"), std::string::npos);
}

TEST(SocketTransportTest, AClientRequestWithInjectedTargetIsRefusedBeforeConnecting) {
  // Request-line injection (issue #109): a raw CR/LF in the target splits
  // "METHOD SP TARGET SP HTTP/1.1" into a smuggled second request. The
  // client must refuse before any socket work — port 1 is never dialed.
  SocketHttpClient client("127.0.0.1", 1);
  HttpRequest request;
  request.target = "/x HTTP/1.1\r\nEvil: injected\r\n\r\nGET /y";
  const auto outcome = client.Send(request);
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(outcome.error().message().find("target"), std::string::npos);
}

TEST(SocketTransportTest, AClientRequestWithInjectedMethodIsRefusedBeforeConnecting) {
  // The method axis: a space or CR/LF there corrupts the request line too.
  SocketHttpClient client("127.0.0.1", 1);
  HttpRequest request;
  request.method = "GET /smuggled HTTP/1.1\r\nEvil: 1\r\n\r\nHEAD";
  const auto outcome = client.Send(request);
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(outcome.error().message().find("method"), std::string::npos);
}

TEST(SocketTransportTest, PeerCloseMidSendIsAnErrorNotSigpipe) {
  SocketHttpServer server;
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{200, {}, "ok"}; }).ok());
  SocketHttpClient client("127.0.0.1", server.port());

  // Over the server's 64 MiB body cap: it rejects on content-length and
  // closes while the client is still writing. That must surface as a
  // transport error (or an HTTP error status), never a SIGPIPE that kills
  // the process.
  HttpRequest request;
  request.method = "POST";
  request.target = "/";
  request.body = std::string((std::size_t{64} << 20) + 1024, 'x');
  const auto response = client.Send(request);
  if (response.ok()) {
    EXPECT_GE(response->status, 400);
  } else {
    EXPECT_EQ(response.error().kind(), ErrorKind::kTransport);
  }
  server.Stop();
}

TEST(SocketTransportTest, ReportsConnectionFailure) {
  SocketHttpServer throwaway;
  ASSERT_TRUE(throwaway.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  const int dead_port = throwaway.port();
  throwaway.Stop();  // port is now closed

  SocketHttpClient client("127.0.0.1", dead_port, /*timeout_ms=*/2000);
  const auto response = client.Send(HttpRequest{});
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().kind(), ErrorKind::kTransport);
}

TEST(SocketTransportTest, StampsThePeerAddress) {
  SocketHttpServer server;
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

TEST(SocketTransportTest, MintsATraceIdentityOverTheWire) {
  // ADR-0011 end to end: a client that sends no traceparent still yields a
  // parseable identity inside the handler chain.
  SocketHttpServer server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest& request) {
                    HttpResponse response;
                    response.headers.Set("x-trace",
                                         request.headers.Get("traceparent").value_or(""));
                    return response;
                  })
                  .ok());
  SocketHttpClient client("127.0.0.1", server.port());
  HttpRequest request;
  request.method = "GET";
  request.target = "/";
  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_TRUE(ParseTraceparent(response->headers.Get("x-trace").value_or("")).has_value())
      << response->headers.Get("x-trace").value_or("");
  server.Stop();
}

TEST(SocketTransportTest, StopIsIdempotent) {
  SocketHttpServer server;
  ASSERT_TRUE(server.Start([](const HttpRequest&) { return HttpResponse{}; }).ok());
  server.Stop();
  server.Stop();  // must not hang or crash
}

TEST(SocketTransportTest, StartLogsTheTestOnlyRelegationNotice) {
  // ADR-0006 relegated this server to tests; a deployment that serves real
  // traffic on it anyway gets an operator-visible trace (issue #46).
  std::ostringstream log;
  std::streambuf* previous = std::clog.rdbuf(log.rdbuf());
  SocketHttpServer server;
  const auto started = server.Start([](const HttpRequest&) { return HttpResponse{}; });
  server.Stop();
  std::clog.rdbuf(previous);
  ASSERT_TRUE(started.ok());
  EXPECT_NE(log.str().find("test-only"), std::string::npos) << log.str();
  EXPECT_NE(log.str().find("BeastServerTransport"), std::string::npos) << log.str();
}

// The same rule as the Beast path (issue #192): a HEAD response is the
// headers the equivalent GET would send, Content-Length included, and no
// body. This transport closes every connection, so a stray body cannot
// desynchronize a later request here — but a client reading Content-Length
// bytes after a HEAD is still reading something that is not its body.
TEST(SocketTransportTest, HeadResponsesCarryTheGetsLengthAndNoBody) {
  SocketHttpServer server;
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 200;
                    response.headers.Set("content-type", "application/json");
                    response.body = R"({"id":"abc"})";
                    return response;
                  })
                  .ok());
  ASSERT_GT(server.port(), 0);

  const std::string raw = RawRoundTrip(server.port(), "HEAD /thing HTTP/1.1\r\nhost: x\r\n\r\n");
  ASSERT_FALSE(raw.empty());

  const auto header_end = raw.find("\r\n\r\n");
  ASSERT_NE(header_end, std::string::npos) << raw;
  EXPECT_EQ(raw.substr(header_end + 4), "") << "HEAD answered with a body: " << raw;
  EXPECT_NE(raw.find("content-length: 12"), std::string::npos) << raw;

  // The GET is untouched, which is what makes that length meaningful.
  const std::string get = RawRoundTrip(server.port(), "GET /thing HTTP/1.1\r\nhost: x\r\n\r\n");
  const auto get_header_end = get.find("\r\n\r\n");
  ASSERT_NE(get_header_end, std::string::npos) << get;
  EXPECT_EQ(get.substr(get_header_end + 4), R"({"id":"abc"})") << get;

  server.Stop();
}

}  // namespace
}  // namespace smithy::http
