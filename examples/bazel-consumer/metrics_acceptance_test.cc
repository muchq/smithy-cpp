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

// A handler that emits its own domain metrics alongside the built-in HTTP
// families — the reason MetricsRegistry hands out typed families rather than
// only serving what Observe feeds it. The handles are minted once and held;
// they are cheap to copy and address the same family.
class MetricsHandler final : public TodoHandler {
 public:
  explicit MetricsHandler(const std::shared_ptr<smithy::server::MetricsRegistry>& metrics)
      : tasks_added_(metrics->NewCounter("todo_tasks_added_total", "Tasks added, by priority.")),
        title_length_(metrics->NewHistogram("todo_title_length_chars", "Task title length.",
                                            {8.0, 32.0, 128.0})),
        tasks_stored_(metrics->NewGauge("todo_tasks_stored", "Tasks currently stored.")) {
    // The priority label has a bounded, known-at-startup set, so both series
    // are declared: without this the first task of each kind lands as a
    // counter's first sample and increase() never sees it.
    tasks_added_.Declare({{"priority", "set"}});
    tasks_added_.Declare({{"priority", "unset"}});
    tasks_stored_.Declare();
  }

  smithy::Outcome<AddTaskOutput> AddTask(const AddTaskInput& input,
                                         const smithy::server::RequestContext&) override {
    tasks_added_.Increment({{"priority", input.priority.has_value() ? "set" : "unset"}});
    title_length_.Observe(static_cast<double>(input.title.size()));
    tasks_stored_.Increment();
    return AddTaskOutput{.taskId = "task-1", .title = input.title};
  }

  smithy::Outcome<GetTaskOutput> GetTask(const GetTaskInput& input,
                                         const smithy::server::RequestContext&) override {
    smithy::Error error = smithy::Error::Modeled("NoSuchTask", "no task: " + input.taskId);
    error.set_detail(NoSuchTask{.message = "no task: " + input.taskId});
    return error;
  }

 private:
  smithy::server::Counter tasks_added_;
  smithy::server::Histogram title_length_;
  smithy::server::Gauge tasks_stored_;
};

class MetricsAcceptanceTest : public ::testing::Test {
 protected:
  void SetUp() override {
    metrics_ = std::make_shared<smithy::server::MetricsRegistry>();
    server_ = std::make_unique<TodoServer>(std::make_shared<MetricsHandler>(metrics_));
    transport_ = std::make_unique<smithy::http::BeastServerTransport>(
        smithy::http::BeastServerTransport::Options{.threads = 1, .handler_threads = 4});
    // The composition the production guide documents, assembled here in
    // consumer code against the published targets alone.
    // The liveness probe sits INSIDE the recorder, so probe traffic is
    // counted — the arrangement that makes its operation label matter.
    ASSERT_TRUE(transport_
                    ->Start(smithy::server::Chain({smithy::server::MetricsEndpoint(metrics_),
                                                   smithy::server::RecordMetrics(metrics_),
                                                   smithy::server::HealthEndpoint("/livez")},
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

TEST_F(MetricsAcceptanceTest, AHealthProbeIsItsOwnSeriesNotAnAnonymous404) {
  // Over a real socket, out of tree: the orchestrator's probe and a request
  // for a route the model does not define must not share a series. They both
  // miss the generated router, and before HealthEndpoint stamped its path
  // both reported `operation=""` — so a service polled every few seconds had
  // its 404 rate buried under probe volume and no query could separate them.
  smithy::http::BeastHttpClient raw({.host = "127.0.0.1", .port = transport_->port()});
  smithy::http::HttpRequest probe;
  probe.method = "GET";
  probe.target = "/livez";
  const auto probed = raw.Send(probe);
  ASSERT_TRUE(probed.ok()) << probed.error().message();
  EXPECT_EQ(probed->status, 200);

  smithy::http::HttpRequest unrouted;
  unrouted.method = "GET";
  unrouted.target = "/livez-typo";
  ASSERT_TRUE(raw.Send(unrouted).ok());

  const auto scrape = Scrape();
  ASSERT_TRUE(scrape.ok()) << scrape.error().message();
  const std::string& body = scrape->body;
  EXPECT_NE(
      body.find(R"(smithy_http_requests_total{method="GET",operation="/livez",status="200"} 1)"),
      std::string::npos)
      << body;
  EXPECT_NE(body.find(R"(smithy_http_requests_total{method="GET",operation="",status="404"} 1)"),
            std::string::npos)
      << body;
  // And the probe's latency is its own, so a service p99 can exclude it.
  EXPECT_NE(
      body.find(R"(smithy_http_request_duration_seconds_count{method="GET",operation="/livez"} 1)"),
      std::string::npos)
      << body;
}

TEST_F(MetricsAcceptanceTest, ApplicationMetricsShareTheEndpointWithTheBuiltIns) {
  // What a consumer actually wants from a metrics endpoint: its own domain
  // numbers on the same scrape as the HTTP families, so one Prometheus target
  // covers the service. The handler minted these from the same registry the
  // middleware serves.
  ASSERT_TRUE(client_->AddTask(AddTaskInput{.title = "short"}).ok());
  ASSERT_TRUE(client_
                  ->AddTask(AddTaskInput{.title = "a considerably longer task title",
                                         .priority = acme::todo::Priority::kHigh})
                  .ok());

  const auto scrape = Scrape();
  ASSERT_TRUE(scrape.ok()) << scrape.error().message();
  const std::string& body = scrape->body;

  EXPECT_NE(body.find("# TYPE todo_tasks_added_total counter"), std::string::npos) << body;
  EXPECT_NE(body.find(R"(todo_tasks_added_total{priority="unset"} 1)"), std::string::npos) << body;
  EXPECT_NE(body.find(R"(todo_tasks_added_total{priority="set"} 1)"), std::string::npos) << body;

  EXPECT_NE(body.find("# TYPE todo_title_length_chars histogram"), std::string::npos) << body;
  EXPECT_NE(body.find(R"(todo_title_length_chars_bucket{le="8"} 1)"), std::string::npos) << body;
  EXPECT_NE(body.find("todo_title_length_chars_count 2"), std::string::npos) << body;

  EXPECT_NE(body.find("# TYPE todo_tasks_stored gauge"), std::string::npos) << body;
  EXPECT_NE(body.find("todo_tasks_stored 2"), std::string::npos) << body;

  // Still one scrape: the built-in families are unaffected by the additions.
  EXPECT_NE(body.find(R"(operation="AddTask")"), std::string::npos) << body;
}

TEST_F(MetricsAcceptanceTest, DeclaredSeriesAreOnTheScrapeBeforeAnyTraffic) {
  // The zero baseline, end to end: a dashboard built against this service
  // reads 0 from startup rather than finding no series at all, so the first
  // task added is a visible step instead of an invisible one.
  const auto scrape = Scrape();
  ASSERT_TRUE(scrape.ok()) << scrape.error().message();
  const std::string& body = scrape->body;
  EXPECT_NE(body.find(R"(todo_tasks_added_total{priority="set"} 0)"), std::string::npos) << body;
  EXPECT_NE(body.find(R"(todo_tasks_added_total{priority="unset"} 0)"), std::string::npos) << body;
  EXPECT_NE(body.find("todo_tasks_stored 0"), std::string::npos) << body;
}

}  // namespace
