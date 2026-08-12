// Out-of-tree acceptance for HTTP redirects (issue #184): a 3xx with a
// Location header and no useful body, which is the whole shape of a URL
// shortener, driven through the generated client, the generated server, and a
// raw socket that never touches generated types.
//
// Both spellings of the redirect are exercised because they take different
// branches through the generator:
//   - Resolve         — status on the @http trait (needs @suppress in the model)
//   - ResolveDynamic  — status via @httpResponseCode, chosen per request
// The dynamic branch used to reject every 3xx client-side (the client tested
// `status < 200 || status > 299`), so ResolveDynamicRedirects* is the
// regression test for that fix, and the 304 case below is the regression test
// for the server emitting a body on a status that must not carry one.

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "acme/redirect/client.h"
#include "acme/redirect/server.h"
#include "smithy/client/config.h"
#include "smithy/core/error.h"
#include "smithy/http/loopback.h"
#include "smithy/http/socket_transport.h"

namespace {

using acme::redirect::FetchInput;
using acme::redirect::FetchOutput;
using acme::redirect::NoSuchSlug;
using acme::redirect::RedirectorClient;
using acme::redirect::RedirectorHandler;
using acme::redirect::RedirectorServer;
using acme::redirect::ResolveDynamicInput;
using acme::redirect::ResolveDynamicOutput;
using acme::redirect::ResolveInput;
using acme::redirect::ResolveOutput;

constexpr int kOk = 200;
constexpr int kMovedPermanently = 301;
constexpr int kFound = 302;
constexpr int kNotModified = 304;

constexpr char kEtag[] = "\"v1\"";
constexpr char kContent[] = "hello from the origin";

// A slug table standing in for the shortener's database. "retired" slugs
// answer 301, "cached" answers 304 (a status RFC 9110 forbids a body on),
// everything else 302.
class SlugHandler final : public RedirectorHandler {
 public:
  SlugHandler() {
    targets_["abc"] = "https://example.com/live";
    targets_["retired"] = "https://example.com/moved";
    targets_["cached"] = "https://example.com/cached";
  }

  smithy::Outcome<ResolveOutput> Resolve(const ResolveInput& input,
                                         const smithy::server::RequestContext&) override {
    auto target = Lookup(input.slug);
    if (!target) return std::move(target).error();
    return ResolveOutput{.location = *target};
  }

  smithy::Outcome<ResolveDynamicOutput> ResolveDynamic(
      const ResolveDynamicInput& input, const smithy::server::RequestContext&) override {
    auto target = Lookup(input.slug);
    if (!target) return std::move(target).error();
    int status = kFound;
    if (input.slug == "retired") status = kMovedPermanently;
    if (input.slug == "cached") status = kNotModified;
    return ResolveDynamicOutput{.status = status, .location = *target};
  }

  // A conditional GET: the caller's If-None-Match decides 200 or 304.
  //
  // The 304 branch deliberately still populates `content` — the fetch already
  // happened, and a handler that looks up the body before deciding the status
  // is the ordinary way to write this. Whether that content reaches the wire
  // is the framework's job, not the handler's: RFC 9110 compliance must not
  // depend on every handler author remembering to clear the payload.
  smithy::Outcome<FetchOutput> Fetch(const FetchInput& input,
                                     const smithy::server::RequestContext&) override {
    auto target = Lookup(input.slug);
    if (!target) return std::move(target).error();
    const bool fresh = input.ifNoneMatch.has_value() && *input.ifNoneMatch == kEtag;
    return FetchOutput{.status = fresh ? kNotModified : kOk,
                       .etag = kEtag,
                       .content = smithy::Blob::FromString(kContent)};
  }

 private:
  smithy::Outcome<std::string> Lookup(const std::string& slug) {
    const std::lock_guard<std::mutex> lock(mu_);
    const auto it = targets_.find(slug);
    if (it == targets_.end()) {
      smithy::Error error = smithy::Error::Modeled("NoSuchSlug", "no slug: " + slug);
      error.set_detail(NoSuchSlug{.message = "no slug: " + slug});
      return error;
    }
    return it->second;
  }

  std::mutex mu_;  // handlers must be thread-safe: transports dispatch on a thread pool
  std::map<std::string, std::string> targets_;
};

enum class Transport { kLoopback, kSocket };

class RedirectE2ETest : public ::testing::TestWithParam<Transport> {
 protected:
  void SetUp() override {
    server_ = std::make_unique<RedirectorServer>(std::make_shared<SlugHandler>());
    smithy::ClientConfig config;
    if (GetParam() == Transport::kLoopback) {
      auto loopback = std::make_shared<smithy::http::Loopback>();
      ASSERT_TRUE(loopback->Start(server_->Handler()).ok());
      config.http_client = loopback;
    } else {
      socket_server_ = std::make_unique<smithy::http::SocketHttpServer>();
      ASSERT_TRUE(socket_server_->Start(server_->Handler()).ok());
      config.endpoint = "http://127.0.0.1:" + std::to_string(socket_server_->port());
    }
    auto client = RedirectorClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<RedirectorClient>(std::move(*client));
  }

  void TearDown() override {
    if (socket_server_ != nullptr) socket_server_->Stop();
  }

  std::unique_ptr<RedirectorServer> server_;
  std::unique_ptr<smithy::http::SocketHttpServer> socket_server_;
  std::unique_ptr<RedirectorClient> client_;
};

// A modeled 302 is a success, not an error: the typed outcome carries Location.
TEST_P(RedirectE2ETest, StaticRedirectIsASuccessfulOutcome) {
  const auto resolved = client_->Resolve(ResolveInput{.slug = "abc"});
  ASSERT_TRUE(resolved.ok()) << resolved.error().message();
  EXPECT_EQ(resolved->location, "https://example.com/live");
}

// The @httpResponseCode branch: the handler picks the status per request and
// the client reports it. Before issue #184 this failed for every slug — the
// client's success window was 2xx-only, so a 301/302 came back as an error
// with Location discarded.
TEST_P(RedirectE2ETest, DynamicRedirectCarriesTheHandlerChosenStatus) {
  const auto temporary = client_->ResolveDynamic(ResolveDynamicInput{.slug = "abc"});
  ASSERT_TRUE(temporary.ok()) << temporary.error().message();
  EXPECT_EQ(temporary->status, kFound);
  EXPECT_EQ(temporary->location, "https://example.com/live");

  const auto permanent = client_->ResolveDynamic(ResolveDynamicInput{.slug = "retired"});
  ASSERT_TRUE(permanent.ok()) << permanent.error().message();
  EXPECT_EQ(permanent->status, kMovedPermanently);
  EXPECT_EQ(permanent->location, "https://example.com/moved");
}

// The control for the two above: widening success to 3xx must not have
// widened it into 4xx. An unknown slug is still a typed modeled error.
TEST_P(RedirectE2ETest, UnknownSlugIsStillAModeledError) {
  const auto missing = client_->Resolve(ResolveInput{.slug = "nope"});
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code(), "NoSuchSlug");
  ASSERT_NE(missing.error().detail<NoSuchSlug>(), nullptr);
  EXPECT_EQ(missing.error().detail<NoSuchSlug>()->message, "no slug: nope");

  const auto missing_dynamic = client_->ResolveDynamic(ResolveDynamicInput{.slug = "nope"});
  ASSERT_FALSE(missing_dynamic.ok());
  EXPECT_EQ(missing_dynamic.error().code(), "NoSuchSlug");
}

// The typed client agrees with the wire: a 304 is a success carrying no
// payload, not an error and not a stale body.
TEST_P(RedirectE2ETest, ConditionalFetchRoundTripsThroughTheGeneratedClient) {
  const auto cold = client_->Fetch(FetchInput{.slug = "abc"});
  ASSERT_TRUE(cold.ok()) << cold.error().message();
  EXPECT_EQ(cold->status, kOk);
  ASSERT_TRUE(cold->content.has_value());
  EXPECT_EQ(cold->content->ToString(), kContent);

  // The handler populated the payload on this one; the server dropped it, so
  // the client sees a 304 with nothing attached.
  const auto conditional = client_->Fetch(FetchInput{.slug = "abc", .ifNoneMatch = kEtag});
  ASSERT_TRUE(conditional.ok()) << conditional.error().message();
  EXPECT_EQ(conditional->status, kNotModified);
  EXPECT_FALSE(conditional->content.has_value());
}

INSTANTIATE_TEST_SUITE_P(Transports, RedirectE2ETest,
                         ::testing::Values(Transport::kLoopback, Transport::kSocket),
                         [](const auto& info) {
                           return info.param == Transport::kLoopback ? "Loopback" : "Socket";
                         });

// The consumer's real boundary: a browser follows Location off the wire, so
// assert the wire with a raw client and no generated types on the reading
// side. A round trip through the generated client would agree with the
// generated server about a renamed header and prove nothing.
TEST(RedirectWireTest, TheBytesABrowserSeesCarryStatusAndLocation) {
  RedirectorServer server(std::make_shared<SlugHandler>());
  smithy::http::SocketHttpServer transport;
  ASSERT_TRUE(transport.Start(server.Handler()).ok());
  smithy::http::SocketHttpClient client("127.0.0.1", transport.port());

  smithy::http::HttpRequest request;
  request.method = "GET";
  request.target = "/r/abc";
  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();

  EXPECT_EQ(response->status, kFound);
  EXPECT_EQ(response->headers.Get("Location").value_or("<missing>"), "https://example.com/live");

  // A 302 *may* carry a body, and today's generator sends "{}" — alloy's own
  // CustomCodeOutput conformance case pins a "{}" body at status 399, so
  // suppressing it for 3xx would fail conformance. Pinned here as the
  // behavior we chose to keep, not as the behavior we'd design: if a future
  // change drops it, this test says so instead of the change being silent.
  EXPECT_EQ(response->body, "{}");

  transport.Stop();
}

// 304 is not a matter of taste: RFC 9110 forbids a body on it, and a 304
// carrying "{}" misleads every cache between the server and the client. The
// generator used to special-case only 204, so this is the wire proof of the
// fix — and it has to be raw, because the generated client never looks.
TEST(RedirectWireTest, NotModifiedCarriesNoBodyAndNoContentType) {
  RedirectorServer server(std::make_shared<SlugHandler>());
  smithy::http::SocketHttpServer transport;
  ASSERT_TRUE(transport.Start(server.Handler()).ok());
  smithy::http::SocketHttpClient client("127.0.0.1", transport.port());

  smithy::http::HttpRequest request;
  request.method = "GET";
  request.target = "/d/cached";
  const auto response = client.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();

  EXPECT_EQ(response->status, kNotModified);
  EXPECT_EQ(response->headers.Get("Location").value_or("<missing>"), "https://example.com/cached");
  EXPECT_EQ(response->body, "");
  EXPECT_FALSE(response->headers.Get("content-type").has_value());

  // The positive twin, sharing the fixture: the same operation on a status
  // that *does* allow a body still sends one. Without this, a server that had
  // simply stopped emitting bodies would pass the assertions above.
  smithy::http::HttpRequest live;
  live.method = "GET";
  live.target = "/d/abc";
  const auto live_response = client.Send(live);
  ASSERT_TRUE(live_response.ok()) << live_response.error().message();
  EXPECT_EQ(live_response->status, kFound);
  EXPECT_EQ(live_response->body, "{}");
  EXPECT_TRUE(live_response->headers.Get("content-type").has_value());

  transport.Stop();
}

// The same rule on the other branch of the generator. An @httpPayload is
// written by different code than a document body, and it used to return before
// the no-content guard was ever reached — so a 304 from a payload-bearing
// operation still shipped the payload. A conditional GET is how a real service
// meets that case.
//
// The handler hands back a populated payload on the 304 on purpose (see
// SlugHandler::Fetch): a test where the handler clears it would pass with or
// without the guard, because there would be nothing to suppress.
TEST(RedirectWireTest, ANotModifiedFromAPayloadOperationSendsNoPayload) {
  RedirectorServer server(std::make_shared<SlugHandler>());
  smithy::http::SocketHttpServer transport;
  ASSERT_TRUE(transport.Start(server.Handler()).ok());
  smithy::http::SocketHttpClient client("127.0.0.1", transport.port());

  // Unconditional: 200 with the payload, which is the control — it proves the
  // fixture serves content at all, so the empty 304 below means something.
  smithy::http::HttpRequest cold;
  cold.method = "GET";
  cold.target = "/c/abc";
  const auto cold_response = client.Send(cold);
  ASSERT_TRUE(cold_response.ok()) << cold_response.error().message();
  EXPECT_EQ(cold_response->status, kOk);
  EXPECT_EQ(cold_response->body, kContent);
  EXPECT_EQ(cold_response->headers.Get("ETag").value_or("<missing>"), kEtag);

  // Conditional on the ETag the server just sent: 304, and not one byte of it.
  smithy::http::HttpRequest conditional;
  conditional.method = "GET";
  conditional.target = "/c/abc";
  conditional.headers.Set("If-None-Match", kEtag);
  const auto conditional_response = client.Send(conditional);
  ASSERT_TRUE(conditional_response.ok()) << conditional_response.error().message();
  EXPECT_EQ(conditional_response->status, kNotModified);
  EXPECT_EQ(conditional_response->body, "");
  EXPECT_FALSE(conditional_response->headers.Get("content-type").has_value());
  // Headers still land: a 304 is required to carry the validator.
  EXPECT_EQ(conditional_response->headers.Get("ETag").value_or("<missing>"), kEtag);

  transport.Stop();
}

}  // namespace
