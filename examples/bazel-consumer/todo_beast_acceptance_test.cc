// Production-transport acceptance from the consumer's side of the module
// boundary: the generated Todo service on BeastServerTransport, driven by the
// generated client over BeastHttpClient::FromConfig — the exact wiring
// docs/production-guide.md teaches. What this proves is the boundary itself:
// the hardening knobs (handler_threads among them) are visible and usable
// from a consumer build, generated round trips and modeled errors survive the
// production transport, and a throwing handler is contained as a 500 without
// killing the service. The executor's concurrency semantics are pinned where
// they live, in the runtime suite (beast_transport_test.cc).

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "acme/todo/client.h"
#include "acme/todo/server.h"
#include "smithy/client/config.h"
#include "smithy/http/beast_transport.h"
#include "smithy/testing/connection_event_recorder.h"
#include "smithy/testing/tls_test_identity.h"

namespace {

using acme::todo::AddTaskInput;
using acme::todo::AddTaskOutput;
using acme::todo::GetTaskInput;
using acme::todo::GetTaskOutput;
using acme::todo::NoSuchTask;
using acme::todo::TodoClient;
using acme::todo::TodoHandler;
using acme::todo::TodoServer;
using smithy::testing::kTestCertificatePem;
using smithy::testing::kTestPrivateKeyPem;

// The blessed endpoint→FromConfig→Create wiring from the production guide,
// shared by every test here: plain http by default, https when a trust
// anchor is supplied.
smithy::Outcome<TodoClient> MakeTodoClient(int port, const std::string& ca_pem = "") {
  smithy::ClientConfig config;
  config.endpoint =
      std::string(ca_pem.empty() ? "http" : "https") + "://127.0.0.1:" + std::to_string(port);
  config.tls.ca_pem = ca_pem;
  auto http_client = smithy::http::BeastHttpClient::FromConfig(config);
  if (!http_client) return std::move(http_client).error();
  config.http_client = *http_client;
  return TodoClient::Create(std::move(config));
}

// A handler with the negative path every real service eventually ships: a
// "boom" title throws (the bug the framework must contain).
class AcceptanceHandler final : public TodoHandler {
 public:
  smithy::Outcome<AddTaskOutput> AddTask(const AddTaskInput& input,
                                         const smithy::server::RequestContext&) override {
    if (input.title == "boom") {
      throw std::runtime_error("consumer handler bug");
    }
    return AddTaskOutput{.taskId = "task-1", .title = input.title};
  }

  smithy::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input,
                                         const smithy::server::RequestContext&) override {
    smithy::Error error = smithy::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
    error.set_detail(NoSuchTask{.message = "no task: " + input.taskId});
    return error;
  }
};

// Starts the service on the production transport and returns a generated
// client wired through BeastHttpClient::FromConfig (the production-guide
// flow). Setting handler_threads here is the consumer-facing proof of the
// executor knob: the field must exist and compose across the module boundary.
class TodoBeastAcceptanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    server_ = std::make_unique<TodoServer>(std::make_shared<AcceptanceHandler>());
    transport_ = std::make_unique<smithy::http::BeastServerTransport>(
        smithy::http::BeastServerTransport::Options{
            .threads = 1, .handler_threads = 8, .drain_timeout_seconds = 5});
    ASSERT_TRUE(transport_->Start(server_->Handler()).ok());

    auto client = MakeTodoClient(transport_->port());
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<TodoClient>(std::move(*client));
  }

  void TearDown() override { transport_->Stop(); }

  std::unique_ptr<TodoServer> server_;
  std::unique_ptr<smithy::http::BeastServerTransport> transport_;
  std::unique_ptr<TodoClient> client_;
};

TEST_F(TodoBeastAcceptanceTest, RoundTripsAndModeledErrorsWork) {
  const auto added = client_->AddTask(AddTaskInput{.title = "ship on beast"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->title, "ship on beast");

  const auto missing = client_->GetTask(GetTaskInput{.taskId = "nope"});
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code(), "NoSuchTask");
  ASSERT_NE(missing.error().detail<NoSuchTask>(), nullptr);
}

// The #109 numeric bounds, out of tree and over the real socket: hostile
// wire values for the model's intEnum and float members are rejected by
// the generated server before the handler runs, and the generated client
// serializes valid ones — the same generator output consumers ship.
TEST_F(TodoBeastAcceptanceTest, NumericBoundsAreEnforcedOverTheRealSocket) {
  smithy::http::BeastHttpClient raw({.host = "127.0.0.1", .port = transport_->port()});
  const auto post = [&raw](const std::string& body) {
    smithy::http::HttpRequest request;
    request.method = "POST";
    request.target = "/tasks";
    request.headers.Set("content-type", "application/json");
    request.body = body;
    return raw.Send(request);
  };

  // 2^32+2 would alias onto a valid Priority under a truncating cast; the
  // parse rejects it instead.
  auto aliased = post(R"({"title":"t","priority":4294967298})");
  ASSERT_TRUE(aliased.ok()) << aliased.error().message();
  EXPECT_EQ(aliased->status, 400);
  EXPECT_EQ(aliased->headers.Get("x-error-type").value_or("<missing>"), "SerializationException");

  // In range but outside the modeled set: membership validation, with the
  // suite-exact message shape.
  auto unknown = post(R"({"title":"t","priority":3})");
  ASSERT_TRUE(unknown.ok()) << unknown.error().message();
  EXPECT_EQ(unknown->status, 400);
  EXPECT_EQ(unknown->headers.Get("x-error-type").value_or("<missing>"), "ValidationException");
  EXPECT_NE(unknown->body.find("Member must satisfy enum value set: [1, 2]"), std::string::npos)
      << unknown->body;

  // A finite double beyond float range was UB to cast; it fails the parse.
  auto overflow = post(R"({"title":"t","effortHours":1e300})");
  ASSERT_TRUE(overflow.ok()) << overflow.error().message();
  EXPECT_EQ(overflow->status, 400);
  EXPECT_EQ(overflow->headers.Get("x-error-type").value_or("<missing>"), "SerializationException");

  // Valid values ride the generated client end to end.
  auto added = client_->AddTask(AddTaskInput{
      .title = "well bounded", .priority = acme::todo::Priority::kHigh, .effortHours = 1.5F});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->title, "well bounded");
}

TEST_F(TodoBeastAcceptanceTest, LifecycleStopsAndRestartsAcrossTransportGenerations) {
  // The rolling-restart pattern from the production guide, composed entirely
  // from the consumer-visible API: serve, Stop() (bounded — the drain knob
  // above is the consumer's contract), then a fresh transport generation on
  // the same generated server object serves again.
  const auto first = client_->AddTask(AddTaskInput{.title = "before restart"});
  ASSERT_TRUE(first.ok()) << first.error().message();

  const auto begin = std::chrono::steady_clock::now();
  transport_->Stop();
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(10));

  smithy::http::BeastServerTransport second_generation(
      smithy::http::BeastServerTransport::Options{.threads = 1, .handler_threads = 8});
  ASSERT_TRUE(second_generation.Start(server_->Handler()).ok());
  auto client = MakeTodoClient(second_generation.port());
  ASSERT_TRUE(client.ok()) << client.error().message();
  const auto after = client->AddTask(AddTaskInput{.title = "after restart"});
  ASSERT_TRUE(after.ok()) << after.error().message();
  EXPECT_EQ(after->title, "after restart");
  second_generation.Stop();
}

TEST_F(TodoBeastAcceptanceTest, HandlerExceptionIsContainedAndServiceSurvives) {
  // The framework answers a correlated 500 instead of dying (issue #41), and
  // the very next call on the same server succeeds.
  const auto boom = client_->AddTask(AddTaskInput{.title = "boom"});
  ASSERT_FALSE(boom.ok());

  const auto after = client_->AddTask(AddTaskInput{.title = "still serving"});
  ASSERT_TRUE(after.ok()) << after.error().message();
  EXPECT_EQ(after->title, "still serving");
}

TEST(TodoBeastMetadataTest, ContextArrivesOverTheProductionTransport) {
  // ADR-0010 on the production transport: the Beast-stamped peer address
  // reaches a generated server's handler from a consumer build (the socket
  // and loopback flavors are pinned in todo_integration_test.cc).
  class PeerProbe final : public TodoHandler {
   public:
    smithy::Outcome<AddTaskOutput> AddTask(const AddTaskInput&,
                                           const smithy::server::RequestContext& context) override {
      return AddTaskOutput{.taskId = "task-1", .title = context.request->peer_address};
    }
    smithy::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input,
                                           const smithy::server::RequestContext&) override {
      return smithy::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
    }
  };

  TodoServer server(std::make_shared<PeerProbe>());
  smithy::http::BeastServerTransport transport(
      smithy::http::BeastServerTransport::Options{.threads = 1});
  ASSERT_TRUE(transport.Start(server.Handler()).ok());
  auto client = MakeTodoClient(transport.port());
  ASSERT_TRUE(client.ok()) << client.error().message();
  const auto added = client->AddTask(AddTaskInput{.title = "who am i"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->title.rfind("127.0.0.1:", 0), 0u) << added->title;
  transport.Stop();
}

TEST(TodoBeastMetadataTest, TransportRejectionsAreObservableFromAConsumerBuild) {
  // The on_rejected knob at the module boundary (the handler_threads
  // convention): an over-limit rejection that Observe middleware can never
  // see reaches the consumer's own hook.
  std::mutex mutex;
  std::vector<smithy::http::BeastServerTransport::RejectedRequest> rejected;
  TodoServer server(std::make_shared<AcceptanceHandler>());
  smithy::http::BeastServerTransport transport(smithy::http::BeastServerTransport::Options{
      .threads = 1,
      .max_body_bytes = 1024,
      .on_rejected = [&](const smithy::http::BeastServerTransport::RejectedRequest& r) {
        const std::lock_guard<std::mutex> lock(mutex);
        rejected.push_back(r);
      }});
  ASSERT_TRUE(transport.Start(server.Handler()).ok());

  smithy::http::BeastHttpClient raw({.host = "127.0.0.1", .port = transport.port()});
  smithy::http::HttpRequest request;
  request.method = "POST";
  request.target = "/tasks";
  request.headers.Set("content-type", "application/json");
  request.body = std::string(64 * 1024, 'x');
  const auto response = raw.Send(request);
  ASSERT_TRUE(response.ok()) << response.error().message();
  EXPECT_EQ(response->status, 413);
  {
    // Scoped so the mutex is released before Stop() joins the io threads.
    const std::lock_guard<std::mutex> lock(mutex);
    ASSERT_EQ(rejected.size(), 1u);
    EXPECT_EQ(rejected[0].status, 413);
  }
  transport.Stop();
}

TEST(TodoBeastMetadataTest, ConnectionEventsAreObservableFromAConsumerBuild) {
  // The on_connection_event knob at the module boundary (ADR-0013): a TLS
  // client speaking to a plain-http server never produces a request, so
  // only the transport can see it — as framing garbage (the ClientHello).
  smithy::testing::ConnectionEventRecorder recorder;
  TodoServer server(std::make_shared<AcceptanceHandler>());
  smithy::http::BeastServerTransport transport(smithy::http::BeastServerTransport::Options{
      .threads = 1, .on_connection_event = recorder.Hook()});
  ASSERT_TRUE(transport.Start(server.Handler()).ok());

  smithy::http::BeastHttpClient wrong_scheme({.host = "127.0.0.1",
                                              .port = transport.port(),
                                              .tls = true,
                                              .tls_options = {.ca_pem = kTestCertificatePem}});
  smithy::http::HttpRequest request;
  request.method = "POST";
  request.target = "/tasks";
  const auto response = wrong_scheme.Send(request);
  EXPECT_FALSE(response.ok());

  ASSERT_TRUE(recorder.WaitFor(1));
  {
    const std::lock_guard<std::mutex> lock(recorder.mutex);
    ASSERT_EQ(recorder.events.size(), 1u);
    EXPECT_EQ(recorder.events[0].kind,
              smithy::http::BeastServerTransport::ConnectionEvent::Kind::kFramingError);
    EXPECT_EQ(recorder.events[0].peer_address.rfind("127.0.0.1:", 0), 0u)
        << recorder.events[0].peer_address;
  }
  transport.Stop();
}

TEST(TodoBeastTlsAcceptanceTest, TlsTerminationServesTheGeneratedClient) {
  // The https flavor of the production wiring: TLS termination on the server
  // options, trust via config.tls.ca_pem, everything else identical. Proves
  // the server's fixed TLS posture (1.2 floor, AEAD ciphers, http/1.1 ALPN —
  // pinned in the runtime suite) composes with the blessed FromConfig client
  // from a consumer build.
  TodoServer server(std::make_shared<AcceptanceHandler>());
  smithy::http::BeastServerTransport transport(
      smithy::http::BeastServerTransport::Options{.threads = 1,
                                                  .tls_certificate_chain_pem = kTestCertificatePem,
                                                  .tls_private_key_pem = kTestPrivateKeyPem});
  ASSERT_TRUE(transport.Start(server.Handler()).ok());

  auto client = MakeTodoClient(transport.port(), kTestCertificatePem);
  ASSERT_TRUE(client.ok()) << client.error().message();

  const auto added = client->AddTask(AddTaskInput{.title = "ship on https"});
  ASSERT_TRUE(added.ok()) << added.error().message();
  EXPECT_EQ(added->title, "ship on https");
  transport.Stop();
}

}  // namespace
