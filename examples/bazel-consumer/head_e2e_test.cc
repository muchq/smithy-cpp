// Out-of-tree acceptance for HEAD (issue #192): a modeled HEAD operation must
// answer the Content-Length its GET twin would and not one octet of the body,
// driven through the generated server and read off a raw socket that never
// touches generated types.
//
// The raw socket is not a stylistic choice. Both shipped clients now know a
// HEAD response has no body and stop after the headers, so a round trip
// through either one reports an empty body whether or not the server sent
// one — the assertion would pass against a server that had not been fixed at
// all. Only the bytes on the wire can tell those apart.
//
// Probe and Fetch serve the same resource by design: the length HEAD reports
// is only meaningful against the length the GET actually delivers, so the
// GET is asserted here too.

#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdint>
#include <memory>
#include <string>

#include "acme/redirect/client.h"
#include "acme/redirect/server.h"
#include "smithy/client/config.h"
#include "smithy/core/error.h"
#include "smithy/http/socket_transport.h"

namespace {

using acme::redirect::FetchInput;
using acme::redirect::FetchOutput;
using acme::redirect::NoSuchSlug;
using acme::redirect::ProbeInput;
using acme::redirect::ProbeOutput;
using acme::redirect::RedirectorHandler;
using acme::redirect::RedirectorServer;
using acme::redirect::ResolveDynamicInput;
using acme::redirect::ResolveDynamicOutput;
using acme::redirect::ResolveInput;
using acme::redirect::ResolveOutput;

constexpr char kEtag[] = "\"v1\"";
constexpr char kContent[] = "hello from the origin";
constexpr int kContentLength = 21;  // sizeof(kContent) - 1, spelled out so the
                                    // wire assertion below is a literal

// Answers Probe exactly as it answers Fetch: same resource, same payload. A
// handler cannot see the method, and this one does not try — clearing the
// payload for HEAD would answer Content-Length: 0, which is a different and
// false claim about the resource.
class ProbeHandler final : public RedirectorHandler {
 public:
  smithy::Outcome<ProbeOutput> Probe(const ProbeInput& input,
                                     const smithy::server::RequestContext&) override {
    if (input.slug != "abc") return NotFound(input.slug);
    return ProbeOutput{.etag = kEtag, .content = smithy::Blob::FromString(kContent)};
  }

  smithy::Outcome<FetchOutput> Fetch(const FetchInput& input,
                                     const smithy::server::RequestContext&) override {
    if (input.slug != "abc") return NotFound(input.slug);
    return FetchOutput{.status = 200, .etag = kEtag, .content = smithy::Blob::FromString(kContent)};
  }

  smithy::Outcome<ResolveOutput> Resolve(const ResolveInput& input,
                                         const smithy::server::RequestContext&) override {
    return NotFound(input.slug);
  }

  smithy::Outcome<ResolveDynamicOutput> ResolveDynamic(
      const ResolveDynamicInput& input, const smithy::server::RequestContext&) override {
    return NotFound(input.slug);
  }

 private:
  static smithy::Error NotFound(const std::string& slug) {
    smithy::Error error = smithy::Error::Modeled("NoSuchSlug", "no slug: " + slug);
    error.set_detail(NoSuchSlug{.message = "no slug: " + slug});
    return error;
  }
};

// Raw bytes in, raw bytes out. See the file comment for why a generated or
// hand-written client cannot stand in here.
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

std::string BodyOf(const std::string& raw) {
  const auto end = raw.find("\r\n\r\n");
  return end == std::string::npos ? std::string() : raw.substr(end + 4);
}

std::string HeadersOf(const std::string& raw) {
  const auto end = raw.find("\r\n\r\n");
  std::string headers = end == std::string::npos ? raw : raw.substr(0, end);
  for (char& c : headers) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return headers;
}

TEST(HeadE2ETest, AModeledHeadCarriesTheGetsLengthAndNoBody) {
  RedirectorServer server(std::make_shared<ProbeHandler>());
  smithy::http::SocketHttpServer transport;
  ASSERT_TRUE(transport.Start(server.Handler()).ok());

  const std::string head =
      RawRoundTrip(transport.port(), "HEAD /c/abc HTTP/1.1\r\nhost: x\r\n\r\n");
  const std::string get = RawRoundTrip(transport.port(), "GET /c/abc HTTP/1.1\r\nhost: x\r\n\r\n");
  ASSERT_FALSE(head.empty());
  ASSERT_FALSE(get.empty());

  // The GET first: the length asserted below means nothing unless this is the
  // resource that actually comes back.
  EXPECT_EQ(BodyOf(get), kContent) << get;
  EXPECT_NE(HeadersOf(get).find("content-length: 21"), std::string::npos) << get;

  EXPECT_EQ(BodyOf(head), "") << "HEAD answered with a body: " << head;
  EXPECT_NE(HeadersOf(head).find("content-length: 21"), std::string::npos)
      << "HEAD did not report the GET's length: " << head;
  // The rest of the header set survives — a HEAD answers "what would I get",
  // so the validator has to come with it.
  EXPECT_NE(HeadersOf(head).find("etag: \"v1\""), std::string::npos) << head;

  transport.Stop();
}

TEST(HeadE2ETest, TheGeneratedClientReadsTheHeadWithoutBlocking) {
  RedirectorServer server(std::make_shared<ProbeHandler>());
  smithy::http::SocketHttpServer transport;
  ASSERT_TRUE(transport.Start(server.Handler()).ok());

  smithy::ClientConfig config;
  config.endpoint = "http://127.0.0.1:" + std::to_string(transport.port());
  config.http_client =
      std::make_shared<smithy::http::SocketHttpClient>("127.0.0.1", transport.port());
  auto created = acme::redirect::RedirectorClient::Create(config);
  ASSERT_TRUE(created.ok()) << created.error().message();
  const auto& client = *created;

  // Returns rather than waiting out the timeout on a Content-Length no
  // compliant server will fill, and the modeled header still arrives.
  const auto probed = client.Probe(ProbeInput{.slug = "abc"});
  ASSERT_TRUE(probed.ok()) << probed.error().message();
  EXPECT_EQ(probed->etag.value_or("<missing>"), kEtag);
  EXPECT_FALSE(probed->content.has_value());

  // The GET twin through the same client, so the empty payload above is the
  // method's doing and not a handler that serves nothing.
  const auto fetched = client.Fetch(FetchInput{.slug = "abc"});
  ASSERT_TRUE(fetched.ok()) << fetched.error().message();
  ASSERT_TRUE(fetched->content.has_value());
  EXPECT_EQ(fetched->content->ToString(), kContent);

  transport.Stop();
}

}  // namespace
