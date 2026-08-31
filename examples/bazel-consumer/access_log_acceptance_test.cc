// The access log a consumer actually writes, from outside the module (#202).
//
// What only this level proves: that `RequestObservation` carries enough to BE
// an access log, on the composition the production guide teaches, over a real
// socket. In-tree the fields are pinned on a hand-driven chain — no generated
// router, no transport-stamped peer, no rate limiter beside it — so every one
// of them could be deleted without a consumer file noticing.
//
// The claim under test is the one #202 was opened for: the client an
// observation reports is the same one `PerClientRateLimit` keyed on, so "whose
// bucket did that 429 come from" is answerable from the log. That needs the
// same `TrustedProxies` handed to both, which is the step the guide's snippet
// used to omit.
//
// `Observe` is composed OUTSIDE the limiter here, deliberately. The limiter
// short-circuits, so a 429 never reaches middleware below it — and the 429 is
// exactly the request whose client you want logged. Put the limiter outermost
// instead and the rejections are invisible to the log.

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "acme/todo/client.h"
#include "acme/todo/server.h"
#include "smithy/client/config.h"
#include "smithy/http/beast_transport.h"
#include "smithy/http/forwarded.h"
#include "smithy/server/middleware.h"

namespace {

using acme::todo::AddTaskInput;
using acme::todo::AddTaskOutput;
using acme::todo::GetTaskInput;
using acme::todo::GetTaskOutput;
using acme::todo::TodoClient;
using acme::todo::TodoHandler;
using acme::todo::TodoServer;

// One access-log record, which is all four of the #202 fields plus what was
// already there. A real sink would render this as JSON (#203); recording it
// verbatim is what lets the test assert on it.
struct LogLine {
  std::string method;
  std::string route;
  int status = 0;
  std::size_t request_bytes = 0;
  std::size_t response_bytes = 0;
  bool handler_threw = false;
  std::string client;
  smithy::http::DerivedClient::Source client_source = smithy::http::DerivedClient::Source::kUnknown;
};

class AccessLog {
 public:
  void Write(const smithy::server::RequestObservation& o) {
    const std::lock_guard<std::mutex> lock(mutex_);
    lines_.push_back(LogLine{.method = o.method,
                             .route = o.operation,
                             .status = o.status,
                             .request_bytes = o.request_bytes,
                             .response_bytes = o.response_bytes,
                             .handler_threw = o.handler_threw,
                             .client = o.client.address,
                             .client_source = o.client.source});
  }
  std::vector<LogLine> lines() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return lines_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<LogLine> lines_;
};

// Records every key the limiter was asked to admit, so the test can compare
// the log's client against the bucket rather than against a literal.
class RecordingLimiter {
 public:
  explicit RecordingLimiter(int budget) : budget_(budget) {}
  bool Allow(const std::string& client) {
    const std::lock_guard<std::mutex> lock(mutex_);
    keys_.push_back(client);
    return --budget_ >= 0;
  }
  std::vector<std::string> keys() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return keys_;
  }

 private:
  mutable std::mutex mutex_;
  int budget_;
  std::vector<std::string> keys_;
};

class ThrowingOnDemandHandler final : public TodoHandler {
 public:
  smithy::Outcome<AddTaskOutput> AddTask(const AddTaskInput& input,
                                         const smithy::server::RequestContext&) override {
    if (input.title == "boom") {
      throw std::runtime_error("handler exploded");
    }
    return AddTaskOutput{.taskId = "task-1", .title = input.title};
  }

  smithy::Outcome<GetTaskOutput> GetTask(const GetTaskInput&,
                                         const smithy::server::RequestContext&) override {
    return GetTaskOutput{.taskId = "task-1", .title = "stored"};
  }
};

class AccessLogAcceptanceTest : public ::testing::Test {
 protected:
  void SetUp() override { Start(/*observe_trusts_the_proxy=*/true); }

  // The limiter always gets the real trust boundary. `Observe` gets it only
  // when asked — the negative control below starts a server without it, which
  // is the wiring the production guide's snippet used to teach.
  void Start(bool observe_trusts_the_proxy) {
    // The loopback peer is 127.0.0.1, so trusting it makes this connection
    // look like one arriving through a proxy — the topology the derivation
    // exists for.
    auto trusted = smithy::http::TrustedProxies::Parse({"127.0.0.0/8"});
    ASSERT_TRUE(trusted.ok()) << trusted.error().message();
    const smithy::http::TrustedProxies observe_trust =
        observe_trusts_the_proxy ? *trusted : smithy::http::TrustedProxies::None();

    log_ = std::make_shared<AccessLog>();
    limiter_ = std::make_shared<RecordingLimiter>(kBudget);
    server_ = std::make_unique<TodoServer>(std::make_shared<ThrowingOnDemandHandler>());
    transport_ = std::make_unique<smithy::http::BeastServerTransport>(
        smithy::http::BeastServerTransport::Options{.threads = 1, .handler_threads = 2});

    auto log = log_;
    auto limiter = limiter_;
    ASSERT_TRUE(
        transport_
            ->Start(smithy::server::Chain(
                {// Outermost, so a rejected request is still logged with the
                 // client it was rejected for.
                 smithy::server::Observe(
                     [log](const smithy::server::RequestObservation& o) { log->Write(o); }, nullptr,
                     nullptr, observe_trust),
                 smithy::server::PerClientRateLimit(
                     [limiter](const std::string& client) { return limiter->Allow(client); },
                     *trusted, std::chrono::seconds(1))},
                server_->Handler()))
            .ok());
  }

  void TearDown() override { transport_->Stop(); }

  // A request carrying an x-forwarded-for, the way one arrives through a
  // proxy. The client is the header's entry; the peer is the proxy.
  smithy::Outcome<smithy::http::HttpResponse> SendForwarded(const std::string& body) {
    smithy::http::BeastHttpClient raw({.host = "127.0.0.1", .port = transport_->port()});
    smithy::http::HttpRequest request;
    request.method = "POST";
    request.target = "/tasks";
    request.headers.Set("content-type", "application/json");
    request.headers.Set("x-forwarded-for", kForwardedClient);
    request.body = body;
    return raw.Send(request);
  }

  static constexpr int kBudget = 2;
  static constexpr const char* kForwardedClient = "203.0.113.7";

  std::shared_ptr<AccessLog> log_;
  std::shared_ptr<RecordingLimiter> limiter_;
  std::unique_ptr<TodoServer> server_;
  std::unique_ptr<smithy::http::BeastServerTransport> transport_;
};

TEST_F(AccessLogAcceptanceTest, TheLoggedClientIsTheBucketTheLimiterKeyedOn) {
  // The #202 claim, end to end. Both are handed the same TrustedProxies, so
  // the log can answer a question about the limiter's decision. Given
  // different boundaries — or none, which is what the guide's snippet used to
  // copy — these two disagree and the log names the proxy.
  const auto served = SendForwarded(R"({"title":"ship it"})");
  ASSERT_TRUE(served.ok()) << served.error().message();
  ASSERT_EQ(served->status, 200);

  const auto lines = log_->lines();
  ASSERT_EQ(lines.size(), 1u);
  const auto keys = limiter_->keys();
  ASSERT_EQ(keys.size(), 1u);

  EXPECT_EQ(lines[0].client, keys[0])
      << "the log names a different client than the limiter keyed on";
  EXPECT_EQ(lines[0].client, kForwardedClient);
  // Not the peer, which is what the raw header or an unconfigured Observe
  // would have reported.
  EXPECT_NE(lines[0].client, "127.0.0.1");
  EXPECT_EQ(lines[0].client_source, smithy::http::DerivedClient::Source::kForwarded);
}

TEST_F(AccessLogAcceptanceTest, ARejectionIsLoggedWithTheClientItWasRejectedFor) {
  // "Whose bucket did that 429 come from" — unanswerable before #202, because
  // the observation carried only the raw header, which is not what the
  // limiter keys on.
  for (int i = 0; i < kBudget; ++i) {
    ASSERT_TRUE(SendForwarded(R"({"title":"ok"})").ok());
  }
  const auto rejected = SendForwarded(R"({"title":"over"})");
  ASSERT_TRUE(rejected.ok()) << rejected.error().message();
  EXPECT_EQ(rejected->status, 429);

  const auto lines = log_->lines();
  ASSERT_EQ(lines.size(), static_cast<std::size_t>(kBudget) + 1);
  const LogLine& last = lines.back();
  EXPECT_EQ(last.status, 429);
  EXPECT_EQ(last.client, kForwardedClient);
  EXPECT_EQ(last.client, limiter_->keys().back());
  // The limiter short-circuits, so the request never reached the router.
  EXPECT_EQ(last.route, "");
}

TEST_F(AccessLogAcceptanceTest, ByteCountsComeFromTheRealWireBodies) {
  const std::string body = R"({"title":"ship it"})";
  const auto served = SendForwarded(body);
  ASSERT_TRUE(served.ok()) << served.error().message();

  const auto lines = log_->lines();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0].request_bytes, body.size());
  EXPECT_EQ(lines[0].response_bytes, served->body.size());
  EXPECT_GT(lines[0].response_bytes, 0u);
  EXPECT_EQ(lines[0].route, "AddTask") << "the route came from the generated router";
}

TEST_F(AccessLogAcceptanceTest, AThrownHandlerIsLoggedAsThrownThroughBeastContainment) {
  // The transport contains the throw and answers 500. A deliberate 500 looks
  // identical on the wire, so handler_threw is the only thing that separates
  // "we crashed" from "we said no" — and it has to survive the transport's
  // containment, not just an in-process rethrow.
  const auto boom = SendForwarded(R"({"title":"boom"})");
  ASSERT_TRUE(boom.ok()) << boom.error().message();
  EXPECT_EQ(boom->status, 500);

  const auto lines = log_->lines();
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(lines[0].handler_threw);
  EXPECT_EQ(lines[0].status, 500);
  // No response was built, so there are no body bytes to report — which is
  // what makes handler_threw the thing that reads it as an absence.
  EXPECT_EQ(lines[0].response_bytes, 0u);
  // And the client is still there: a 500 is exactly when you want to know
  // who was calling.
  EXPECT_EQ(lines[0].client, kForwardedClient);
}

// The negative control for TheLoggedClientIsTheBucketTheLimiterKeyedOn.
//
// A passing test only means something if it can fail, and the way this one
// fails is a wiring mistake rather than a code change — so the mistake is
// wired up here and its symptom asserted, instead of being a claim in a
// commit message that has to be re-verified by hand.
//
// This is exactly what the production guide's chain used to teach: the trust
// boundary handed to `PerClientRateLimit` and not to `Observe`.
class MisconfiguredAccessLogTest : public AccessLogAcceptanceTest {
 protected:
  void SetUp() override { Start(/*observe_trusts_the_proxy=*/false); }
};

TEST_F(MisconfiguredAccessLogTest, ObserveWithoutTheTrustBoundaryLogsTheProxy) {
  const auto served = SendForwarded(R"({"title":"ship it"})");
  ASSERT_TRUE(served.ok()) << served.error().message();
  ASSERT_EQ(served->status, 200);

  const auto lines = log_->lines();
  ASSERT_EQ(lines.size(), 1u);
  const auto keys = limiter_->keys();
  ASSERT_EQ(keys.size(), 1u);

  // The symptom: the log names the proxy, the limiter keyed on the client,
  // and nothing anywhere reports an error. Both numbers look plausible on
  // their own — which is why the positive test compares them to each other
  // rather than to literals.
  EXPECT_EQ(lines[0].client, "127.0.0.1");
  EXPECT_EQ(keys[0], kForwardedClient);
  EXPECT_NE(lines[0].client, keys[0])
      << "the misconfiguration stopped being observable, so the positive test "
         "can no longer fail and has stopped proving anything";
  // TrustedProxies::None() is the deliberate direct-connect statement, so the
  // peer IS the client and the header is ignored wholly — correct behavior
  // for that configuration, and the wrong configuration for this deployment.
  EXPECT_EQ(lines[0].client_source, smithy::http::DerivedClient::Source::kUntrustedHeaderIgnored);
}

}  // namespace
