// The Prometheus endpoint (issue #91) from the consumer's side of the module
// boundary: the generated Todo service on BeastServerTransport, wrapped in the
// exact middleware chain docs/production-guide.md teaches, scraped over a real
// socket.
//
// What only this level proves is the `operation` label. In-tree the endpoint is
// driven by hand-written handlers that stamp `HttpResponse::operation`
// themselves, so those tests would keep passing if the generated router stopped
// stamping it — and the label would silently go empty for every request in
// every real deployment, collapsing per-operation dashboards into one anonymous
// bucket. Here the router is the generated one, so the label is the model's.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "acme/todo/client.h"
#include "acme/todo/server.h"
#include "smithy/client/config.h"
#include "smithy/http/beast_transport.h"
#include "smithy/server/metrics.h"
#include "smithy/server/middleware.h"

namespace {

using acme::todo::AddTaskInput;
using acme::todo::AddTaskOutput;
using acme::todo::GetTaskInput;
using acme::todo::GetTaskOutput;
using acme::todo::NoSuchTask;
using acme::todo::TodoClient;
using acme::todo::TodoHandler;
using acme::todo::TodoServer;

class MetricsHandler final : public TodoHandler {
 public:
  smithy::Outcome<AddTaskOutput> AddTask(const AddTaskInput& input,
                                         const smithy::server::RequestContext&) override {
    return AddTaskOutput{.taskId = "task-1", .title = input.title};
  }

  smithy::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input,
                                         const smithy::server::RequestContext&) override {
    smithy::Error error = smithy::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
    error.set_detail(NoSuchTask{.message = "no task: " + input.taskId});
    return error;
  }
};

class MetricsAcceptanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    server_ = std::make_unique<TodoServer>(std::make_shared<MetricsHandler>());
    metrics_ = std::make_shared<smithy::server::MetricsRegistry>();
    transport_ = std::make_unique<smithy::http::BeastServerTransport>(
        smithy::http::BeastServerTransport::Options{.threads = 1, .handler_threads = 4});
    // The composition the production guide documents, assembled here in
    // consumer code against the published targets alone.
    ASSERT_TRUE(transport_
                    ->Start(smithy::server::Chain({smithy::server::MetricsEndpoint(metrics_),
                                                   smithy::server::RecordMetrics(metrics_)},
                                                  server_->Handler()))
                    .ok());

    smithy::ClientConfig config;
    config.endpoint = "http://127.0.0.1:" + std::to_string(transport_->port());
    auto http_client = smithy::http::BeastHttpClient::FromConfig(config);
    ASSERT_TRUE(http_client.ok()) << http_client.error().message();
    config.http_client = *http_client;
    auto client = TodoClient::Create(std::move(config));
    ASSERT_TRUE(client.ok()) << client.error().message();
    client_ = std::make_unique<TodoClient>(std::move(*client));
  }

  void TearDown() override { transport_->Stop(); }

  // Scrapes /metrics the way Prometheus does: a plain GET, no generated code
  // in the loop.
  smithy::Outcome<smithy::http::HttpResponse> Scrape() {
    smithy::http::BeastHttpClient raw({.host = "127.0.0.1", .port = transport_->port()});
    smithy::http::HttpRequest request;
    request.method = "GET";
    request.target = "/metrics";
    return raw.Send(request);
  }

  std::unique_ptr<TodoServer> server_;
  std::shared_ptr<smithy::server::MetricsRegistry> metrics_;
  std::unique_ptr<smithy::http::BeastServerTransport> transport_;
  std::unique_ptr<TodoClient> client_;
};

TEST_F(MetricsAcceptanceTest, TheGeneratedRoutersOperationIsTheMetricLabel) {
  ASSERT_TRUE(client_->AddTask(AddTaskInput{.title = "ship it"}).ok());
  ASSERT_TRUE(client_->AddTask(AddTaskInput{.title = "again"}).ok());
  const auto missing = client_->GetTask(GetTaskInput{.taskId = "nope"});
  ASSERT_FALSE(missing.ok());

  const auto scrape = Scrape();
  ASSERT_TRUE(scrape.ok()) << scrape.error().message();
  EXPECT_EQ(scrape->status, 200);
  EXPECT_EQ(scrape->headers.Get("content-type").value_or("<missing>"),
            "text/plain; version=0.0.4; charset=utf-8");

  const std::string& body = scrape->body;
  // The operation names come from the model, through the generated router —
  // not from anything this test stamped.
  EXPECT_NE(body.find(R"(operation="AddTask")"), std::string::npos) << body;
  EXPECT_NE(body.find(R"(operation="GetTask")"), std::string::npos) << body;
  EXPECT_NE(
      body.find(R"(smithy_http_requests_total{method="POST",operation="AddTask",status="200"} 2)"),
      std::string::npos)
      << body;
  // The modeled error is served traffic too, under its own status.
  EXPECT_NE(body.find(R"(operation="GetTask",status="404")"), std::string::npos) << body;

  // Latency was filed under the same operation label, with a real total.
  EXPECT_NE(
      body.find(
          R"(smithy_http_request_duration_seconds_count{method="POST",operation="AddTask"} 2)"),
      std::string::npos)
      << body;
}

TEST_F(MetricsAcceptanceTest, ScrapesDoNotCountThemselvesAndLeaveNothingInFlight) {
  ASSERT_TRUE(client_->AddTask(AddTaskInput{.title = "one"}).ok());
  ASSERT_TRUE(Scrape().ok());
  ASSERT_TRUE(Scrape().ok());

  const auto scrape = Scrape();
  ASSERT_TRUE(scrape.ok()) << scrape.error().message();
  const std::string& body = scrape->body;
  // MetricsEndpoint sits outside RecordMetrics, so three scrapes added no
  // GET series of their own.
  EXPECT_EQ(body.find(R"(method="GET")"), std::string::npos) << body;
  // Every request that started also finished.
  EXPECT_NE(body.find("smithy_http_requests_in_flight 0"), std::string::npos) << body;
}

TEST_F(MetricsAcceptanceTest, AnUnroutedRequestCountsWithAnEmptyOperation) {
  // A 404 never reaches an operation, and its target — which is what varies
  // without bound — must not become a label.
  smithy::http::BeastHttpClient raw({.host = "127.0.0.1", .port = transport_->port()});
  smithy::http::HttpRequest request;
  request.method = "GET";
  request.target = "/no/such/route/8f3a2b";
  const auto missed = raw.Send(request);
  ASSERT_TRUE(missed.ok()) << missed.error().message();
  EXPECT_EQ(missed->status, 404);

  const auto scrape = Scrape();
  ASSERT_TRUE(scrape.ok()) << scrape.error().message();
  const std::string& body = scrape->body;
  EXPECT_NE(body.find(R"(smithy_http_requests_total{method="GET",operation="",status="404"} 1)"),
            std::string::npos)
      << body;
  EXPECT_EQ(body.find("8f3a2b"), std::string::npos)
      << "the request target leaked into a label: " << body;
}

}  // namespace
