// BeastHttpClient against BeastServerTransport: plaintext round trips with
// keep-alive reuse, TLS with certificate + hostname verification against the
// server's TLS termination, the verification failure modes (untrusted chain,
// and a trusted chain carrying the wrong name), and both ends' TLS posture —
// the server's version floor and ALPN, and the client's own floor against a
// deliberately downgraded peer. The server-observability tests that need a
// TLS fixture (rejection and connection events under TLS, ADR-0013) live
// here too.

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <array>
#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "smithy/client/config.h"
#include "smithy/http/beast_transport.h"
#include "smithy/http/message.h"
#include "smithy/http/socket_transport.h"
#include "smithy/http/trace_context.h"
#include "smithy/http/transport.h"
#include "smithy/testing/connection_event_recorder.h"
#include "smithy/testing/tls_test_identity.h"

namespace smithy::http {
namespace {

using smithy::testing::kMismatchedNameCertificatePem;
using smithy::testing::kMismatchedNamePrivateKeyPem;
using smithy::testing::kTestCertificatePem;
using smithy::testing::kTestPrivateKeyPem;

RequestHandler EchoHandler() {
  return [](const HttpRequest& request) {
    HttpResponse response;
    response.status = 200;
    response.headers.Set("x-echo-method", request.method);
    response.headers.Set("x-echo-target", request.target);
    response.headers.Set("x-echo-traceparent", request.headers.Get("traceparent").value_or(""));
    response.body = request.body;
    return response;
  };
}

HttpRequest PostRequest(const std::string& body) {
  HttpRequest request;
  request.method = "POST";
  request.target = "/echo";
  request.headers.Set("content-type", "text/plain");
  request.headers.Set("content-length", std::to_string(body.size()));
  request.body = body;
  return request;
}

// The TLS-terminating server every TLS test here dials.
BeastServerTransport::Options TlsServerOptions(int threads = 1) {
  return {.port = 0,
          .threads = threads,
          .tls_certificate_chain_pem = kTestCertificatePem,
          .tls_private_key_pem = kTestPrivateKeyPem};
}

TEST(BeastClientTest, PlaintextRoundTripsAndReusesConnections) {
  BeastServerTransport server({.port = 0, .threads = 2});
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  BeastHttpClient client({.host = "127.0.0.1", .port = server.port()});
  // Several sequential requests: the second and later ones ride the pooled
  // keep-alive connection.
  for (int i = 0; i < 3; ++i) {
    const std::string body = "hello " + std::to_string(i);
    auto response = client.Send(PostRequest(body));
    ASSERT_TRUE(response.ok()) << response.error().message();
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, body);
    EXPECT_EQ(response->headers.Get("x-echo-method").value_or(""), "POST");
    EXPECT_EQ(response->headers.Get("x-echo-target").value_or(""), "/echo");
  }
  server.Stop();
}

TEST(BeastClientTest, SurvivesServerRestartBetweenRequests) {
  // The pooled connection dies with the server; the client must notice the
  // stale connection and redial instead of failing the request.
  auto server = std::make_unique<BeastServerTransport>(
      BeastServerTransport::Options{.port = 0, .threads = 1});
  ASSERT_TRUE(server->Start(EchoHandler()).ok());
  const int port = server->port();

  BeastHttpClient client({.host = "127.0.0.1", .port = port});
  ASSERT_TRUE(client.Send(PostRequest("one")).ok());

  server->Stop();
  server = std::make_unique<BeastServerTransport>(
      BeastServerTransport::Options{.port = port, .threads = 1});
  ASSERT_TRUE(server->Start(EchoHandler()).ok());

  auto response = client.Send(PostRequest("two"));
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "two");
  server->Stop();
}

TEST(BeastClientTest, MintsDistinctTraceIdsAcrossKeepAliveRequests) {
  // ADR-0011 on the production transport: minting is per request, not per
  // connection — two requests riding one pooled keep-alive connection get
  // two identities.
  BeastServerTransport server({.port = 0, .threads = 1});
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  BeastHttpClient client({.host = "127.0.0.1", .port = server.port()});
  const auto first = client.Send(PostRequest("one"));
  const auto second = client.Send(PostRequest("two"));  // rides the pooled connection
  ASSERT_TRUE(first.ok() && second.ok());
  const auto first_trace = ParseTraceparent(first->headers.Get("x-echo-traceparent").value_or(""));
  const auto second_trace =
      ParseTraceparent(second->headers.Get("x-echo-traceparent").value_or(""));
  ASSERT_TRUE(first_trace.has_value() && second_trace.has_value());
  EXPECT_NE(first_trace->trace_id, second_trace->trace_id);
  server.Stop();
}

TEST(BeastClientTest, HeadRoundTripsWithoutWaitingForTheBodyItsLengthAdvertises) {
  // The client half of issue #192. The server now answers a HEAD with the
  // GET's Content-Length and no octets, so a client that reads that header as
  // a promise of bytes waits for a body no compliant peer will send — the
  // request ends at request_timeout_ms, and the connection is left mid-message.
  BeastServerTransport server({.port = 0, .threads = 1});
  ASSERT_TRUE(server
                  .Start([](const HttpRequest&) {
                    HttpResponse response;
                    response.status = 200;
                    response.headers.Set("content-type", "application/json");
                    response.body = R"({"id":"abc"})";
                    return response;
                  })
                  .ok());

  // Short, so a client that waits reports it here instead of stalling the
  // suite for the 30s default.
  BeastHttpClient client({.host = "127.0.0.1", .port = server.port(), .request_timeout_ms = 2000});

  HttpRequest head;
  head.method = "HEAD";
  head.target = "/thing";
  const auto response = client.Send(head);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->body, "");
  EXPECT_EQ(response->headers.Get("content-length").value_or(""), "12");

  // The connection goes back in the pool on keep-alive. A HEAD the client
  // mis-framed would leave it out of sync, and this GET would read the tail
  // of the previous message.
  HttpRequest get;
  get.method = "GET";
  get.target = "/thing";
  const auto pooled = client.Send(get);
  ASSERT_TRUE(pooled.ok()) << pooled.error().message();
  EXPECT_EQ(pooled->status, 200);
  EXPECT_EQ(pooled->body, R"({"id":"abc"})");
  server.Stop();
}

TEST(BeastClientTest, TlsRoundTripsWithCustomCa) {
  BeastServerTransport server(TlsServerOptions(/*threads=*/2));
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  BeastHttpClient client({.host = "127.0.0.1",
                          .port = server.port(),
                          .tls = true,
                          .tls_options = {.ca_pem = kTestCertificatePem}});
  for (int i = 0; i < 2; ++i) {
    auto response = client.Send(PostRequest("secret"));
    ASSERT_TRUE(response.ok()) << response.error().message();
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(response->body, "secret");
  }
  server.Stop();
}

TEST(BeastClientTest, OverLimitRejectionIsObservedUnderTls) {
  // Options::on_rejected on the TLS stream flavor: the 413 written inside
  // the encrypted session is observed like its plaintext twin.
  std::mutex mutex;
  std::vector<BeastServerTransport::RejectedRequest> rejected;
  auto options = TlsServerOptions();
  options.max_body_bytes = 1024;
  options.on_rejected = [&](const BeastServerTransport::RejectedRequest& r) {
    const std::lock_guard<std::mutex> lock(mutex);
    rejected.push_back(r);
  };
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  BeastHttpClient client({.host = "127.0.0.1",
                          .port = server.port(),
                          .tls = true,
                          .tls_options = {.ca_pem = kTestCertificatePem}});
  const auto response = client.Send(PostRequest(std::string(64 * 1024, 'x')));
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 413);
  {
    // Status and peer only: field extraction is pinned by the plaintext twin
    // (beast_transport_test.cc); peer exercises get_lowest_layer on the
    // ssl-stream instantiation.
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(rejected.size(), 1u);
    EXPECT_EQ(rejected[0].status, 413);
    EXPECT_EQ(rejected[0].peer_address.rfind("127.0.0.1:", 0), 0u) << rejected[0].peer_address;
  }
  server.Stop();
}

TEST(BeastClientTest, PlaintextToTheTlsPortIsObservedAsAHandshakeFailure) {
  // The "LB is misrouting" alarm (ADR-0013): a client speaking plain HTTP
  // to the TLS port fails the handshake, and the event carries the peer —
  // known before TLS ever ran.
  smithy::testing::ConnectionEventRecorder recorder;
  auto options = TlsServerOptions();
  options.on_connection_event = recorder.Hook();
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  SocketHttpClient plaintext("127.0.0.1", server.port());
  const auto response = plaintext.Send(PostRequest("hello?"));
  EXPECT_FALSE(response.ok());

  ASSERT_TRUE(recorder.WaitFor(1));
  const std::lock_guard<std::mutex> lock(recorder.mutex);
  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].kind,
            BeastServerTransport::ConnectionEvent::Kind::kTlsHandshakeFailure);
  EXPECT_EQ(recorder.events[0].peer_address.rfind("127.0.0.1:", 0), 0u)
      << recorder.events[0].peer_address;
  EXPECT_FALSE(recorder.events[0].detail.empty());
  server.Stop();
}

TEST(BeastClientTest, TlsLifecycleAndProbesStaySilent) {
  // The other half of ADR-0013's handshake taxonomy, pinned against the
  // noise regression its correctness review caught: (a) this runtime's own
  // client tears down pooled TLS connections without close_notify
  // (stream_truncated at the server's next read — a healthy close, not a
  // drop), and (b) TCP health probes connect and leave, or idle into the
  // deadline, without ever really starting a handshake. All silent.
  smithy::testing::ConnectionEventRecorder recorder;
  auto options = TlsServerOptions();
  options.request_timeout_seconds = 1;
  options.on_connection_event = recorder.Hook();
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  {
    // A real TLS request, then the client destructor's abrupt teardown.
    BeastHttpClient client({.host = "127.0.0.1",
                            .port = server.port(),
                            .tls = true,
                            .tls_options = {.ca_pem = kTestCertificatePem}});
    const auto response = client.Send(PostRequest("over tls"));
    ASSERT_TRUE(response.ok()) << response.error().message();
    EXPECT_EQ(response->status, 200);
  }

  {
    // Probe shapes on the TLS port: connect-and-leave, and connect-and-idle
    // past the handshake deadline.
    boost::asio::io_context probe_io;
    boost::asio::ip::tcp::socket toucher(probe_io);
    toucher.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"),
                                                   static_cast<unsigned short>(server.port())));
    toucher.close();
    boost::asio::ip::tcp::socket idler(probe_io);
    idler.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"),
                                                 static_cast<unsigned short>(server.port())));
    // A probe that dies mid-TLS-record (three bytes of a five-byte record
    // header) is still the connect-and-leave shape, not a wrong handshake.
    boost::asio::ip::tcp::socket half_record(probe_io);
    half_record.connect(boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"),
                                                       static_cast<unsigned short>(server.port())));
    (void)half_record.send(boost::asio::buffer(std::string_view("\x16\x03\x01")));
    half_record.close();
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));  // idler's deadline passes
    idler.close();
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  {
    const std::lock_guard<std::mutex> lock(recorder.mutex);
    EXPECT_TRUE(recorder.events.empty()) << "kind=" << static_cast<int>(recorder.events[0].kind)
                                         << " detail=" << recorder.events[0].detail;
  }
  server.Stop();
}

TEST(BeastClientTest, TlsVerificationRejectsUntrustedServers) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  // Default trust roots do not contain the self-signed test certificate.
  BeastHttpClient client({.host = "127.0.0.1", .port = server.port(), .tls = true});
  auto response = client.Send(PostRequest("secret"));
  ASSERT_FALSE(response.ok());
  EXPECT_EQ(response.error().kind(), ErrorKind::kTransport);
  server.Stop();
}

TEST(BeastClientTest, TlsVerificationRejectsATrustedCertificateForTheWrongHost) {
  // The other half of verification, and the half a trusted-CA test cannot
  // reach: this server's certificate chains to a root the client explicitly
  // trusts (it IS the root — self-signed, handed over as ca_pem), so chain
  // validation succeeds and the ONLY thing left to reject the handshake is
  // the hostname check. Its SAN covers other.example.com; the client dials
  // 127.0.0.1. A client that stopped verifying names — or one whose SAN
  // matching silently changed underneath it, the standing risk every
  // BoringSSL bump carries — would round-trip here instead of failing.
  BeastServerTransport server({.port = 0,
                               .threads = 1,
                               .tls_certificate_chain_pem = kMismatchedNameCertificatePem,
                               .tls_private_key_pem = kMismatchedNamePrivateKeyPem});
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  BeastHttpClient client({.host = "127.0.0.1",
                          .port = server.port(),
                          .tls = true,
                          .tls_options = {.ca_pem = kMismatchedNameCertificatePem}});
  auto response = client.Send(PostRequest("secret"));
  ASSERT_FALSE(response.ok()) << "handshake succeeded against a certificate for another host";
  EXPECT_EQ(response.error().kind(), ErrorKind::kTransport);

  // The same server and the same trust anchor, with verification off, does
  // round-trip — proving the refusal above is the name check and not an
  // unrelated failure to reach or trust this server.
  BeastHttpClient unverified({.host = "127.0.0.1",
                              .port = server.port(),
                              .tls = true,
                              .tls_options = {.verify_peer = false}});
  auto reached = unverified.Send(PostRequest("secret"));
  ASSERT_TRUE(reached.ok()) << reached.error().message();
  EXPECT_EQ(reached->body, "secret");
  server.Stop();
}

// A bare TLS listener that answers exactly one handshake under a protocol
// ceiling (and optionally a floor) of its caller's choosing.
// BeastServerTransport deliberately exposes no knob to weaken its own posture
// (the floor is fixed, not configuration), so pinning what the *client*
// refuses — or the versions it can reach — needs a server built by hand.
class CappedTlsServer {
 public:
  explicit CappedTlsServer(int max_proto_version, int min_proto_version = 0)
      : ctx_(boost::asio::ssl::context::tls_server),
        acceptor_(io_,
                  boost::asio::ip::tcp::endpoint(boost::asio::ip::make_address("127.0.0.1"), 0)) {
    boost::system::error_code ec;
    (void)ctx_.use_certificate_chain(boost::asio::buffer(std::string_view(kTestCertificatePem)),
                                     ec);
    EXPECT_FALSE(ec) << ec.message();
    (void)ctx_.use_private_key(boost::asio::buffer(std::string_view(kTestPrivateKeyPem)),
                               boost::asio::ssl::context::pem, ec);
    EXPECT_FALSE(ec) << ec.message();
    EXPECT_EQ(SSL_CTX_set_max_proto_version(ctx_.native_handle(), max_proto_version), 1);
    if (min_proto_version != 0) {
      EXPECT_EQ(SSL_CTX_set_min_proto_version(ctx_.native_handle(), min_proto_version), 1);
    }
    port_ = acceptor_.local_endpoint().port();
    thread_ = std::thread([this] {
      boost::system::error_code accept_ec;
      boost::asio::ip::tcp::socket socket(io_);
      (void)acceptor_.accept(socket, accept_ec);
      if (accept_ec) {
        return;  // closed before anyone dialed
      }
      boost::asio::ssl::stream<boost::asio::ip::tcp::socket> stream(std::move(socket), ctx_);
      boost::system::error_code ec;
      (void)stream.handshake(boost::asio::ssl::stream_base::server, ec);
      if (ec) {
        return;  // a refused downgrade ends here; the test asserts client-side
      }
      // Answer one request so a handshake the client DOES accept round-trips
      // like any other server, which is what makes the TLS 1.2 control
      // meaningful. Read the request first so the client is never writing
      // into a socket this side has already finished with.
      std::array<char, 1024> scratch{};
      (void)stream.read_some(boost::asio::buffer(scratch), ec);
      constexpr std::string_view kResponse =
          "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecret";
      (void)boost::asio::write(stream, boost::asio::buffer(kResponse), ec);
      (void)stream.shutdown(ec);
    });
  }

  ~CappedTlsServer() {
    boost::system::error_code ec;
    (void)acceptor_.close(ec);  // unblocks accept() if nobody ever dialed
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  CappedTlsServer(const CappedTlsServer&) = delete;
  CappedTlsServer& operator=(const CappedTlsServer&) = delete;

  int port() const { return port_; }

 private:
  boost::asio::io_context io_;
  boost::asio::ssl::context ctx_;
  boost::asio::ip::tcp::acceptor acceptor_;
  int port_ = 0;
  std::thread thread_;
};

TEST(BeastClientTest, TlsClientRefusesAPreTls12Server) {
  // The client's own version floor (SetupClientTlsContext's ApplyTls12Floor).
  // The suite already pins the server refusing downgraded clients; this is
  // the mirror, and until now the client's floor was only ever asserted to be
  // *set*, never observed to bite. The certificate here is the matching one
  // the client fully trusts, so the sole reason this handshake cannot
  // complete is that the peer tops out below TLS 1.2.
  CappedTlsServer server(TLS1_1_VERSION);

  BeastHttpClient client({.host = "127.0.0.1",
                          .port = server.port(),
                          .tls = true,
                          .tls_options = {.ca_pem = kTestCertificatePem}});
  auto response = client.Send(PostRequest("secret"));
  ASSERT_FALSE(response.ok()) << "completed a handshake below the TLS 1.2 floor";
  EXPECT_EQ(response.error().kind(), ErrorKind::kTransport);
}

TEST(BeastClientTest, TlsClientAcceptsATls12Server) {
  // The control for the floor test above: the identical hand-built listener
  // capped at TLS 1.2 — the lowest version the floor admits — does complete.
  // Without this, a client that refused every CappedTlsServer for some
  // unrelated reason would still pass the test above.
  CappedTlsServer server(TLS1_2_VERSION);

  BeastHttpClient client({.host = "127.0.0.1",
                          .port = server.port(),
                          .tls = true,
                          .tls_options = {.ca_pem = kTestCertificatePem}});
  auto response = client.Send(PostRequest("secret"));
  ASSERT_TRUE(response.ok()) << response.error().message();
}

TEST(BeastClientTest, TlsVerificationCanBeDisabledExplicitly) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  BeastHttpClient client({.host = "127.0.0.1",
                          .port = server.port(),
                          .tls = true,
                          .tls_options = {.verify_peer = false}});
  auto response = client.Send(PostRequest("secret"));
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "secret");
  server.Stop();
}

TEST(BeastClientTest, FromConfigParsesTheEndpointAndRejectsBadSchemes) {
  ClientConfig config;
  config.endpoint = "https://api.example.com";
  ASSERT_TRUE(BeastHttpClient::FromConfig(config).ok());
  config.endpoint = "http://api.example.com:8080/prefix";
  ASSERT_TRUE(BeastHttpClient::FromConfig(config).ok());
  config.endpoint = "ftp://api.example.com";
  EXPECT_FALSE(BeastHttpClient::FromConfig(config).ok());
  config.endpoint = "";
  EXPECT_FALSE(BeastHttpClient::FromConfig(config).ok());
}

TEST(BeastClientTest, FromConfigHonorsTheConfigsTlsKnobs) {
  BeastServerTransport server(TlsServerOptions(/*threads=*/2));
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  // The one-stop production path (issue #49): endpoint, TLS trust, timeout,
  // and pool size all come from the one ClientConfig the generated client
  // will also use — nothing is configured twice.
  ClientConfig config;
  config.endpoint = "https://127.0.0.1:" + std::to_string(server.port());
  config.tls.ca_pem = kTestCertificatePem;
  auto client = BeastHttpClient::FromConfig(config);
  ASSERT_TRUE(client.ok()) << client.error().message();
  auto response = (*client)->Send(PostRequest("secret"));
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "secret");
  server.Stop();
}

TEST(BeastClientTest, ConfigAndOptionsDefaultsAgree) {
  // The two knobs FromConfig copies as scalars are defaulted in both structs
  // (the TLS knobs share one struct and can't drift); this guards the pair.
  const ClientConfig config;
  const BeastHttpClient::Options options;
  EXPECT_EQ(config.request_timeout_ms, options.request_timeout_ms);
  EXPECT_EQ(config.max_idle_connections, options.max_idle_connections);
}

TEST(BeastClientTest, RejectsTlsServerMisconfiguration) {
  BeastServerTransport server(
      {.port = 0, .tls_certificate_chain_pem = kTestCertificatePem});  // key missing
  EXPECT_FALSE(server.Start(EchoHandler()).ok());
}

// Raw TLS dialer for the posture tests: BeastHttpClient can't be talked into
// an old protocol version or a custom ALPN list, so these handshake with
// asio::ssl directly. No verification — the posture, not trust, is under
// test.
struct RawTlsProbe {
  boost::asio::io_context io;
  boost::asio::ssl::context ctx{boost::asio::ssl::context::tls_client};
  std::optional<boost::asio::ssl::stream<boost::asio::ip::tcp::socket>> stream;

  // Returns the handshake's error code; success is a falsy code.
  boost::system::error_code Handshake(int port) {
    stream.emplace(io, ctx);
    boost::asio::ip::tcp::resolver resolver(io);
    boost::system::error_code ec;
    const auto endpoints = resolver.resolve("127.0.0.1", std::to_string(port), ec);
    if (!ec) {
      (void)boost::asio::connect(stream->next_layer(), endpoints, ec);
    }
    if (!ec) {
      (void)stream->handshake(boost::asio::ssl::stream_base::client, ec);
    }
    return ec;
  }
};

TEST(BeastClientTest, ATlsPeerVanishingMidRequestIsObservedAsDropped) {
  // The mid-message side of the stream_truncated split (ADR-0013): a TLS
  // client that dies after starting a request — TCP close, no close_notify
  // — is a drop, not a clean close.
  smithy::testing::ConnectionEventRecorder recorder;
  auto options = TlsServerOptions();
  options.on_connection_event = recorder.Hook();
  BeastServerTransport server(options);
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  RawTlsProbe probe;
  ASSERT_FALSE(probe.Handshake(server.port()));
  boost::system::error_code ec;
  (void)probe.stream->write_some(boost::asio::buffer(std::string_view("POST /echo HTTP/1.1\r\n")),
                                 ec);
  ASSERT_FALSE(ec) << ec.message();
  (void)probe.stream->next_layer().close(ec);  // TCP close, no SSL shutdown

  ASSERT_TRUE(recorder.WaitFor(1));
  const std::lock_guard<std::mutex> lock(recorder.mutex);
  ASSERT_EQ(recorder.events.size(), 1u);
  EXPECT_EQ(recorder.events[0].kind, BeastServerTransport::ConnectionEvent::Kind::kDropped);
  EXPECT_FALSE(recorder.events[0].detail.empty());
  // No peer assertion: the probe never reads the server's session tickets,
  // so its close() is an RST that can land before the read phase even arms
  // — the documented may-be-empty case. Peer presence on drops is pinned
  // by the plaintext twins (beast_transport_test.cc).
  server.Stop();
}

TEST(BeastClientTest, TlsServerRefusesPreTls12Clients) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  RawTlsProbe probe;
  ASSERT_EQ(SSL_CTX_set_max_proto_version(probe.ctx.native_handle(), TLS1_1_VERSION), 1);
  EXPECT_TRUE(probe.Handshake(server.port()));  // refused below the TLS 1.2 floor
  server.Stop();
}

TEST(BeastClientTest, Tls12CipherPolicyIsEcdheAeadOnly) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  // A TLS 1.2 client with default ciphers lands on an ECDHE+AEAD suite (1.3
  // is capped away so cipher_list, not the fixed 1.3 suites, decides).
  RawTlsProbe aead;
  ASSERT_EQ(SSL_CTX_set_max_proto_version(aead.ctx.native_handle(), TLS1_2_VERSION), 1);
  ASSERT_FALSE(aead.Handshake(server.port()));
  const std::string cipher =
      SSL_CIPHER_get_name(SSL_get_current_cipher(aead.stream->native_handle()));
  EXPECT_TRUE(cipher.find("GCM") != std::string::npos ||
              cipher.find("CHACHA20") != std::string::npos)
      << cipher;

  // A client that can only do CBC-mode 1.2 suites is refused. (The AES-SHA1
  // variants are the one CBC family BoringSSL still ships; the SHA384 CBC
  // suites OpenSSL keeps would make this assert fail to even set up there.)
  RawTlsProbe cbc;
  ASSERT_EQ(SSL_CTX_set_max_proto_version(cbc.ctx.native_handle(), TLS1_2_VERSION), 1);
  ASSERT_EQ(SSL_CTX_set_cipher_list(cbc.ctx.native_handle(),
                                    "ECDHE-ECDSA-AES128-SHA:ECDHE-RSA-AES128-SHA"),
            1);
  EXPECT_TRUE(cbc.Handshake(server.port()));
  server.Stop();
}

TEST(BeastClientTest, TlsServerNegotiatesHttp11Alpn) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  RawTlsProbe probe;
  // Wire format: length-prefixed names. h2 first — selection must be by
  // support, not offer order. (set_alpn_protos returns 0 on success.)
  const unsigned char offer[] = {2, 'h', '2', 8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  ASSERT_EQ(SSL_CTX_set_alpn_protos(probe.ctx.native_handle(), offer, sizeof(offer)), 0);
  ASSERT_FALSE(probe.Handshake(server.port()));

  const unsigned char* selected = nullptr;
  unsigned int selected_len = 0;
  SSL_get0_alpn_selected(probe.stream->native_handle(), &selected, &selected_len);
  EXPECT_EQ(std::string_view(reinterpret_cast<const char*>(selected), selected_len), "http/1.1");
  server.Stop();
}

TEST(BeastClientTest, ARequestWithAnInjectedHeaderIsRefusedBeforeAnyWire) {
  // The outbound injection defense (issue #109): CR/LF in a request header
  // would split the request on the wire. The client refuses with
  // Validation before it dials — a live server proves nothing is sent.
  BeastServerTransport server({.port = 0, .threads = 1});
  ASSERT_TRUE(server.Start(EchoHandler()).ok());
  BeastHttpClient client({.host = "127.0.0.1", .port = server.port()});
  HttpRequest request = PostRequest("body");
  request.headers.Set("x-echo", "value\r\nx-evil: injected");
  const auto outcome = client.Send(request);
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(outcome.error().message().find("x-echo"), std::string::npos);
  server.Stop();
}

TEST(BeastClientTest, ARequestWithAnInjectedTargetIsRefusedBeforeAnyWire) {
  // Request-line injection (issue #109): a raw CR/LF in the target would
  // split the request line. Refused with Validation before the dial — the
  // live server would echo it as a 200 if the request were actually sent.
  BeastServerTransport server({.port = 0, .threads = 1});
  ASSERT_TRUE(server.Start(EchoHandler()).ok());
  BeastHttpClient client({.host = "127.0.0.1", .port = server.port()});
  HttpRequest request = PostRequest("body");
  request.target = "/x HTTP/1.1\r\nEvil: injected\r\n\r\nGET /y";
  const auto outcome = client.Send(request);
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(outcome.error().message().find("target"), std::string::npos);
  server.Stop();
}

TEST(BeastClientTest, ARequestWithAnInjectedMethodIsRefusedBeforeAnyWire) {
  // The method axis: a space or CR/LF in the method corrupts the request
  // line just as a bad target does. Refused with Validation before the dial.
  BeastServerTransport server({.port = 0, .threads = 1});
  ASSERT_TRUE(server.Start(EchoHandler()).ok());
  BeastHttpClient client({.host = "127.0.0.1", .port = server.port()});
  HttpRequest request = PostRequest("body");
  request.method = "POST /smuggled HTTP/1.1\r\nEvil: 1\r\n\r\nHEAD";
  const auto outcome = client.Send(request);
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(outcome.error().message().find("method"), std::string::npos);
  server.Stop();
}

TEST(BeastClientTest, ARequestWithAnEmptyMethodIsRefusedBeforeAnyWire) {
  // An empty method would write a request line starting with a bare space;
  // the client refuses it (the header contract's non-emptiness rule).
  BeastServerTransport server({.port = 0, .threads = 1});
  ASSERT_TRUE(server.Start(EchoHandler()).ok());
  BeastHttpClient client({.host = "127.0.0.1", .port = server.port()});
  HttpRequest request = PostRequest("body");
  request.method = "";
  const auto outcome = client.Send(request);
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().kind(), ErrorKind::kValidation);
  EXPECT_NE(outcome.error().message().find("method"), std::string::npos);
  server.Stop();
}

TEST(BeastClientTest, ALegitimateTargetWithColonsAndEncodingIsNotAFalsePositive) {
  // The guard must not reject valid targets: colon, semicolon, and percent
  // escapes are all legal origin-form characters and must round-trip.
  BeastServerTransport server({.port = 0, .threads = 1});
  ASSERT_TRUE(server.Start(EchoHandler()).ok());
  BeastHttpClient client({.host = "127.0.0.1", .port = server.port()});
  HttpRequest request = PostRequest("body");
  request.target = "/a:b/c;p=q?city=a%20b&n=1";
  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 200);
  EXPECT_EQ(response->headers.Get("x-echo-target").value_or(""), "/a:b/c;p=q?city=a%20b&n=1");
  server.Stop();
}

TEST(BeastClientTest, TlsServerRefusesAlpnWithoutHttp11) {
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  RawTlsProbe probe;
  const unsigned char offer[] = {2, 'h', '2'};
  ASSERT_EQ(SSL_CTX_set_alpn_protos(probe.ctx.native_handle(), offer, sizeof(offer)), 0);
  // An ALPN offer with no overlap fails the handshake (no_application_protocol)
  // instead of silently proceeding in a protocol the client didn't agree to.
  EXPECT_TRUE(probe.Handshake(server.port()));
  server.Stop();
}

TEST(BeastClientTest, TlsDefaultHandshakeNegotiatesTls13) {
  // The ceiling actually reached, not just the floor enforced: neither end
  // caps its maximum version, so what an uncapped handshake lands on is
  // whatever the linked TLS library offers by default — today TLS 1.3. A
  // boringssl bump that quietly moved that default would change every
  // consumer's negotiated protocol without failing any floor test; this pin
  // turns that drift into a red leg instead of a changelog argument.
  BeastServerTransport server(TlsServerOptions());
  ASSERT_TRUE(server.Start(EchoHandler()).ok());

  RawTlsProbe probe;
  ASSERT_FALSE(probe.Handshake(server.port()));
  EXPECT_STREQ(SSL_get_version(probe.stream->native_handle()), "TLSv1.3");
  server.Stop();
}

TEST(BeastClientTest, TlsClientReachesATls13OnlyServer) {
  // The client-side twin: a listener that requires TLS 1.3 (floor == ceiling
  // == 1.3). The floor tests above only prove the client refuses below 1.2;
  // this proves its own uncapped ceiling reaches 1.3, so a regression in the
  // client context (or the linked TLS library) that stranded it at 1.2
  // cannot hide behind servers that still allow 1.2.
  CappedTlsServer server(TLS1_3_VERSION, TLS1_3_VERSION);

  BeastHttpClient client({.host = "127.0.0.1",
                          .port = server.port(),
                          .tls = true,
                          .tls_options = {.ca_pem = kTestCertificatePem}});
  auto response = client.Send(PostRequest("secret"));
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->body, "secret");
}

}  // namespace
}  // namespace smithy::http
