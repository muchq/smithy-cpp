// Pins the dependency-free Prometheus backend (issue #91): what the registry
// aggregates from the Observe hooks, what the endpoint serves, and the
// cardinality rules that keep a scrape endpoint from becoming the outage.
//
// The exposition assertions are deliberately literal. A metrics endpoint has
// no in-process consumer to catch a format slip — the failure surfaces as a
// scrape Prometheus silently rejects, hours later, on a dashboard nobody is
// watching yet.

#include "smithy/server/metrics.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "smithy/server/middleware.h"

namespace smithy::server {
namespace {

using std::chrono::microseconds;

RequestObservation Served(std::string method, std::string operation, int status,
                          microseconds duration) {
  return RequestObservation{.method = std::move(method),
                            .target = "/ignored",
                            .operation = std::move(operation),
                            .trace_parent = "",
                            .status = status,
                            .duration = duration};
}

// Every test that expects a registry to record anything has to turn it on:
// MetricsOptions::enabled is false by default so that a metrics stack that
// is linked in but not switched on costs nothing. The DisabledRegistry tests
// below pin that default and what it buys.
MetricsOptions Enabled() {
  MetricsOptions options;
  options.enabled = true;
  options.service_name = "todo-service";
  return options;
}

// The label prefix every built-in series carries, spelled once.
const std::string kService = R"(service_name="todo-service")";

// The exposition is line-oriented, so assertions read best as "this exact
// line is present" rather than as substring soup.
bool HasLine(const std::string& exposition, const std::string& line) {
  const std::string padded = "\n" + exposition;
  return padded.find("\n" + line + "\n") != std::string::npos;
}

http::HttpRequest Get(std::string target) {
  http::HttpRequest request;
  request.method = "GET";
  request.target = std::move(target);
  return request;
}

// A terminal handler standing in for the generated router.
http::RequestHandler Handler(int status = 200, std::string operation = "GetThing") {
  return [status, operation = std::move(operation)](const http::HttpRequest&) {
    http::HttpResponse response;
    response.status = status;
    response.operation = operation;
    return response;
  };
}

// ---------------------------------------------------------------------------
// What the registry aggregates.
// ---------------------------------------------------------------------------

TEST(MetricsRegistryTest, CountsRequestsByMethodOperationAndStatus) {
  MetricsRegistry registry(Enabled());
  registry.Record(Served("GET", "GetThing", 200, microseconds(1000)));
  registry.Record(Served("GET", "GetThing", 200, microseconds(2000)));
  registry.Record(Served("POST", "PutThing", 500, microseconds(3000)));

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="GetThing"} 2)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="POST",route="PutThing"} 1)"))
      << exposition;
}

TEST(MetricsRegistryTest, EmitsTheFamilyHeadersEvenBeforeAnyTraffic) {
  // A freshly started server should still describe its shape, so a scrape
  // configured against it is verifiable before the first request arrives.
  const std::string exposition = MetricsRegistry(Enabled()).Expose();
  EXPECT_TRUE(HasLine(exposition, "# TYPE http_server_requests_total counter")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE http_server_request_duration_microseconds histogram"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE http_server_requests_active_gauge gauge")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE http_server_requests_success_total counter"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE http_server_requests_failure_total counter"))
      << exposition;
  // The gauge gets no zero baseline: its label is the method, and which
  // methods a service will see is not knowable before it sees them.
  EXPECT_EQ(exposition.find("http_server_requests_active_gauge{"), std::string::npos) << exposition;
}

TEST(MetricsRegistryTest, HistogramBucketsAreCumulativeAndEndAtInf) {
  MetricsRegistry registry(Enabled());
  registry.Record(Served("GET", "GetThing", 200, microseconds(100)));       // the le="100" bucket
  registry.Record(Served("GET", "GetThing", 200, microseconds(2500)));      // the le="2500" bucket
  registry.Record(Served("GET", "GetThing", 200, microseconds(50000000)));  // past the ladder

  const std::string exposition = registry.Expose();
  const std::string labels = R"(service_name="todo-service",http_method="GET",route="GetThing")";
  // Buckets are upper-inclusive, which is what `le` means: exactly 100µs
  // belongs in the 100 bucket rather than the one above it.
  EXPECT_TRUE(HasLine(
      exposition, "http_server_request_duration_microseconds_bucket{" + labels + R"(,le="100"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "http_server_request_duration_microseconds_bucket{" + labels +
                                      R"(,le="2500"} 2)"))
      << exposition;
  // The last finite bound still holds 2: the 50s observation is past it and
  // lands only in +Inf.
  EXPECT_TRUE(HasLine(exposition, "http_server_request_duration_microseconds_bucket{" + labels +
                                      R"(,le="10000000"} 2)"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "http_server_request_duration_microseconds_bucket{" + labels +
                                      R"(,le="+Inf"} 3)"))
      << exposition;
  EXPECT_TRUE(
      HasLine(exposition, "http_server_request_duration_microseconds_count{" + labels + "} 3"))
      << exposition;
  EXPECT_TRUE(
      HasLine(exposition, "http_server_request_duration_microseconds_sum{" + labels + "} 50002600"))
      << exposition;
}

TEST(MetricsRegistryTest, SubMillisecondLatenciesSurviveTheMicrosecondHook) {
  // The hook is microseconds precisely so cache hits and loopback don't
  // report as zero (#92), and the exposition's unit is microseconds too, so
  // the value travels undivided.
  MetricsRegistry registry(Enabled());
  registry.Record(Served("GET", "GetThing", 200, microseconds(1)));
  EXPECT_TRUE(
      HasLine(registry.Expose(),
              R"(http_server_request_duration_microseconds_sum{service_name="todo-service",)"
              R"(http_method="GET",route="GetThing"} 1)"))
      << registry.Expose();
}

TEST(MetricsRegistryTest, DispatchFailuresCountUnderAnEmptyOperation) {
  // 404/405/400 never reached an operation, so the label is empty rather
  // than inventing one — and the target that caused it is deliberately not
  // a label at all.
  MetricsRegistry registry(Enabled());
  registry.Record(Served("GET", "", 404, microseconds(100)));
  EXPECT_TRUE(HasLine(
      registry.Expose(),
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="unmatched"} 1)"))
      << registry.Expose();
}

TEST(MetricsRegistryTest, RecordsConcurrentlyWithoutLosingCounts) {
  MetricsRegistry registry(Enabled());
  constexpr int kThreads = 8;
  constexpr int kPerThread = 500;
  std::vector<std::thread> threads;
  threads.reserve(kThreads);
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&registry] {
      for (int i = 0; i < kPerThread; ++i) {
        registry.Record(Served("GET", "GetThing", 200, microseconds(1000)));
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_TRUE(HasLine(
      registry.Expose(),
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="GetThing"} )" +
          std::to_string(kThreads * kPerThread)))
      << registry.Expose();
}

// ---------------------------------------------------------------------------
// Cardinality: the failure mode a metrics endpoint dies of.
// ---------------------------------------------------------------------------

TEST(MetricsRegistryTest, AnInventedMethodCollapsesInsteadOfMintingASeries) {
  // The method comes off the wire, so a loop of `curl -X <random>` is a
  // memory-exhaustion vector if it reaches the label set verbatim.
  MetricsRegistry registry(Enabled());
  for (int i = 0; i < 100; ++i) {
    registry.Record(Served("BOGUS" + std::to_string(i), "", 405, microseconds(10)));
  }
  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="CUSTOM",route="unmatched"} 100)"))
      << exposition;
  EXPECT_EQ(exposition.find("BOGUS"), std::string::npos) << exposition;
}

TEST(MetricsRegistryTest, LowercaseMethodIsNotFoldedIntoTheRealOne) {
  // HTTP methods are case-sensitive (RFC 9110 §9.1): a "get" the server
  // rejected must not report as served GET traffic.
  MetricsRegistry registry(Enabled());
  registry.Record(Served("get", "", 405, microseconds(10)));
  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="CUSTOM",route="unmatched"} 1)"))
      << exposition;
}

TEST(MetricsRegistryTest, TheSeriesCapStopsGrowthAndSaysSoOutLoud) {
  // The backstop for an unbounded operation stamped by a hand-written
  // handler: stop minting, and expose the drops so it can be alerted on
  // rather than discovered as an OOM.
  MetricsOptions options = Enabled();
  options.max_series = 4;
  MetricsRegistry registry(options);
  for (int i = 0; i < 50; ++i) {
    registry.Record(Served("GET", "Op" + std::to_string(i), 200, microseconds(10)));
  }
  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="Op0"} 1)"))
      << exposition;
  EXPECT_EQ(exposition.find(R"(route="Op49")"), std::string::npos) << exposition;
  // Four combinations fit; the remaining 46 observations are refused, and
  // each is counted exactly once even though both families turned it away.
  EXPECT_TRUE(
      HasLine(exposition, "metrics_observations_dropped_total{service_name=\"todo-service\"} 46"))
      << exposition;
}

TEST(MetricsRegistryTest, LabelValuesAreEscapedSoTheScrapeStaysParseable) {
  // An operation is bounded by the model, but a hand-written handler can
  // stamp anything; an unescaped quote would corrupt the whole scrape.
  MetricsRegistry registry(Enabled());
  registry.Record(Served("GET", R"(We"ird\Op)", 200, microseconds(10)));
  EXPECT_TRUE(HasLine(
      registry.Expose(),
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="We\"ird\\Op"} 1)"))
      << registry.Expose();
}

// ---------------------------------------------------------------------------
// The in-flight gauge.
// ---------------------------------------------------------------------------

TEST(MetricsRegistryTest, InFlightRisesOnStartAndFallsOnCompletion) {
  MetricsRegistry registry(Enabled());
  registry.RecordStart(RequestStart{.method = "GET", .target = "/a"});
  registry.RecordStart(RequestStart{.method = "GET", .target = "/b"});
  EXPECT_TRUE(HasLine(registry.Expose(),
                      R"(http_server_requests_active_gauge{service_name="todo-service",)"
                      R"(http_method="GET"} 2)"))
      << registry.Expose();

  registry.Record(Served("GET", "GetThing", 200, microseconds(10)));
  EXPECT_TRUE(HasLine(registry.Expose(),
                      R"(http_server_requests_active_gauge{service_name="todo-service",)"
                      R"(http_method="GET"} 1)"))
      << registry.Expose();
}

TEST(MetricsRegistryTest, CompletionsWithoutStartsLeaveTheGaugeAtZero) {
  // RecordStart is optional; an unpaired completion must not drive the gauge
  // negative, which would render as a nonsense dashboard forever after.
  MetricsRegistry registry(Enabled());
  registry.Record(Served("GET", "GetThing", 200, microseconds(10)));
  registry.Record(Served("GET", "GetThing", 200, microseconds(10)));
  // The series does not exist at all rather than reading -2: nothing ever
  // started, so there is no method key to have driven negative.
  EXPECT_EQ(registry.Expose().find("http_server_requests_active_gauge{"), std::string::npos)
      << registry.Expose();
}

// ---------------------------------------------------------------------------
// Transport rejections: the 413/431 written before any middleware exists.
// ---------------------------------------------------------------------------

TEST(MetricsRegistryTest, ARejectionIsCountedLikeAnyOtherServedRequest) {
  // Without this an over-limit flood is invisible in the counters: the
  // transport answers these before the handler chain RecordMetrics wraps.
  MetricsRegistry registry(Enabled());
  registry.RecordRejection("POST", 413);
  registry.RecordRejection("POST", 413);
  EXPECT_TRUE(HasLine(
      registry.Expose(),
      R"(http_server_requests_total{service_name="todo-service",http_method="POST",route="unmatched"} 2)"))
      << registry.Expose();
}

TEST(MetricsRegistryTest, ARejectionBeforeTheMethodParsedIsNotAnInventedVerb) {
  // A 431 can fire mid-headers, before the method token was read. "never
  // parsed" and "client invented a verb" are different diagnoses, so they
  // must not share the "other" bucket.
  MetricsRegistry registry(Enabled());
  registry.RecordRejection("", 431);
  registry.RecordRejection("BREW", 431);
  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition, R"lit(http_server_requests_total{service_name="todo-service",)lit"
                                  R"lit(http_method="(unparsed)",route="unmatched"} 1)lit"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="CUSTOM",route="unmatched"} 1)"))
      << exposition;
}

TEST(MetricsRegistryTest, ARejectionFilesNoLatencyAndMovesNoGauge) {
  // The improvement over recording a zero observation: a request refused at
  // parse time has no service latency, and a flood of zeros would drag
  // rate(_sum)/rate(_count) down — making the latency panel look its best
  // exactly while the service is being hammered. It was also never in
  // flight through a handler, so the gauge must not move either.
  MetricsRegistry registry(Enabled());
  registry.Record(Served("POST", "AddThing", 200, microseconds(200000)));  // 0.2s
  for (int i = 0; i < 50; ++i) {
    registry.RecordRejection("POST", 413);
  }

  const std::string exposition = registry.Expose();
  // One real observation, and the mean is still that observation.
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_request_duration_microseconds_count{service_name="todo-service",http_method="POST",route="AddThing"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_request_duration_microseconds_sum{service_name="todo-service",http_method="POST",route="AddThing"} 200000)"))
      << exposition;
  // No latency series was minted for the rejections at all.
  EXPECT_EQ(
      exposition.find(
          R"(http_server_request_duration_microseconds_count{service_name="todo-service",http_method="POST",route="unmatched"})"),
      std::string::npos)
      << exposition;
  EXPECT_EQ(exposition.find("http_server_requests_active_gauge{"), std::string::npos) << exposition;
}

TEST(MetricsRegistryTest, TheRejectionSinkFeedsTheRegistry) {
  // The shape a consumer wires into BeastServerTransport::Options, checked
  // against a stand-in with the same fields — :server keeps no Beast dep.
  struct Rejected {
    int status = 0;
    std::string peer_address{};
    std::string method{};
    std::string target{};
  };
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  auto sink = RecordRejections(registry);
  sink(Rejected{.status = 413, .method = "PUT", .target = "/upload/8f3a2b"});

  const std::string exposition = registry->Expose();
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="PUT",route="unmatched"} 1)"))
      << exposition;
  // The target is dropped: a flood against distinct paths mints no series.
  EXPECT_EQ(exposition.find("8f3a2b"), std::string::npos) << exposition;
}

// ---------------------------------------------------------------------------
// Application metrics.
// ---------------------------------------------------------------------------

TEST(MetricsRegistryTest, ACustomCounterJoinsTheSameScrape) {
  MetricsRegistry registry(Enabled());
  auto orders = registry.NewCounter("orders_processed_total", "Orders processed.");
  orders.Increment();
  orders.Increment({{"region", "us-east"}}, 4);

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition, "# HELP orders_processed_total Orders processed.")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE orders_processed_total counter")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "orders_processed_total 1")) << exposition;
  EXPECT_TRUE(HasLine(exposition, R"(orders_processed_total{region="us-east"} 4)")) << exposition;
  // The built-in families are still there, whole.
  EXPECT_TRUE(HasLine(exposition, "# TYPE http_server_requests_total counter")) << exposition;
}

TEST(MetricsRegistryTest, AGaugeGoesUpAndDown) {
  MetricsRegistry registry(Enabled());
  auto depth = registry.NewGauge("queue_depth", "Pending jobs.");
  depth.Set(10);
  depth.Increment(5);
  depth.Decrement(3);
  EXPECT_TRUE(HasLine(registry.Expose(), "queue_depth 12")) << registry.Expose();
}

TEST(MetricsRegistryTest, ACustomHistogramExposesBucketsSumAndCount) {
  MetricsRegistry registry(Enabled());
  auto sizes = registry.NewHistogram("batch_size", "Rows per batch.", {10.0, 100.0});
  sizes.Observe(5);
  sizes.Observe(50);
  sizes.Observe(500);

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition, R"(batch_size_bucket{le="10"} 1)")) << exposition;
  EXPECT_TRUE(HasLine(exposition, R"(batch_size_bucket{le="100"} 2)")) << exposition;
  EXPECT_TRUE(HasLine(exposition, R"(batch_size_bucket{le="+Inf"} 3)")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "batch_size_sum 555")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "batch_size_count 3")) << exposition;
}

TEST(MetricsRegistryTest, LabelOrderDoesNotSplitASeries) {
  // Sorting by name is what keeps {a,b} and {b,a} one series; without it a
  // caller that swapped two labels would silently double-count.
  MetricsRegistry registry(Enabled());
  auto hits = registry.NewCounter("cache_hits_total", "Cache hits.");
  hits.Increment({{"tier", "hot"}, {"region", "eu"}});
  hits.Increment({{"region", "eu"}, {"tier", "hot"}});
  EXPECT_TRUE(HasLine(registry.Expose(), R"(cache_hits_total{region="eu",tier="hot"} 2)"))
      << registry.Expose();
}

TEST(MetricsRegistryTest, ACustomLabelValueIsEscaped) {
  // All three the exposition format requires: quote, backslash, newline. An
  // unescaped one corrupts the whole scrape, not just this line.
  MetricsRegistry registry(Enabled());
  auto errors = registry.NewCounter("job_errors_total", "Job errors.");
  errors.Increment({{"reason", "quote\" back\\slash\nnewline"}});
  EXPECT_TRUE(
      HasLine(registry.Expose(), R"(job_errors_total{reason="quote\" back\\slash\nnewline"} 1)"))
      << registry.Expose();
}

TEST(MetricsRegistryTest, AnUnboundedCustomLabelIsCappedAndAttributed) {
  // The whole point of the per-family cap: a label taken from unbounded data
  // costs that family its budget and says so, instead of the process.
  MetricsOptions options = Enabled();
  options.max_series = 4;
  MetricsRegistry registry(options);
  auto seen = registry.NewCounter("user_events_total", "User events.");
  for (int i = 0; i < 50; ++i) {
    seen.Increment({{"user_id", std::to_string(i)}});
  }
  const std::string exposition = registry.Expose();
  EXPECT_EQ(exposition.find(R"(user_id="49")"), std::string::npos) << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(metrics_observations_dropped_total{service_name="todo-service",metric="user_events_total"} 46)"))
      << exposition;
}

// The zero baseline. A series nobody has touched is absent from the scrape,
// and a counter whose first exported sample is its first event's value hides
// that event forever — increase() has nothing earlier to measure against, so
// the panel reads zero, which looks like an answer.

TEST(MetricsRegistryTest, ADeclaredSeriesExportsAtZeroBeforeAnyEvent) {
  MetricsRegistry registry(Enabled());
  auto orders = registry.NewCounter("orders_processed_total", "Orders.");
  orders.Declare({{"region", "us-east"}});

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition, R"(orders_processed_total{region="us-east"} 0)")) << exposition;
}

TEST(MetricsRegistryTest, DeclaringDoesNotDisturbASeriesThatHasEvents) {
  // Idempotent, and harmless after the fact: re-declaring must not reset a
  // counter that has already counted something.
  MetricsRegistry registry(Enabled());
  auto orders = registry.NewCounter("orders_processed_total", "Orders.");
  orders.Increment(7);
  orders.Declare();
  orders.Declare();
  EXPECT_TRUE(HasLine(registry.Expose(), "orders_processed_total 7")) << registry.Expose();
}

TEST(MetricsRegistryTest, ADeclaredHistogramIsEmptyRatherThanAnObservationOfZero) {
  // The distinction that matters: a record-only API can only baseline a
  // histogram by observing 0, which biases rate(_sum)/rate(_count). Writing
  // the exposition directly means the declared series can be genuinely
  // empty — every bucket, _sum and _count at 0 — so the first real
  // observation is the only one the mean ever sees.
  MetricsRegistry registry(Enabled());
  auto sizes = registry.NewHistogram("batch_size", "Rows per batch.", {10.0});
  sizes.Declare();

  std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition, R"(batch_size_bucket{le="10"} 0)")) << exposition;
  EXPECT_TRUE(HasLine(exposition, R"(batch_size_bucket{le="+Inf"} 0)")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "batch_size_sum 0")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "batch_size_count 0")) << exposition;

  // One observation of 4 must read as a mean of 4, not 2 — which is what a
  // baseline recorded as an observation would have produced.
  sizes.Observe(4);
  exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition, "batch_size_sum 4")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "batch_size_count 1")) << exposition;
}

TEST(MetricsRegistryTest, ADeclaredGaugeReadsZeroRatherThanBeingAbsent) {
  MetricsRegistry registry(Enabled());
  auto depth = registry.NewGauge("queue_depth", "Pending jobs.");
  depth.Declare();
  EXPECT_TRUE(HasLine(registry.Expose(), "queue_depth 0")) << registry.Expose();
}

TEST(MetricsRegistryTest, DeclaringRespectsTheSeriesCap) {
  // Declaration is series creation, so it cannot be a way around the cap.
  MetricsOptions options = Enabled();
  options.max_series = 2;
  MetricsRegistry registry(options);
  auto seen = registry.NewCounter("user_events_total", "User events.");
  for (int i = 0; i < 10; ++i) {
    seen.Declare({{"user_id", std::to_string(i)}});
  }
  const std::string exposition = registry.Expose();
  EXPECT_EQ(exposition.find(R"(user_id="9")"), std::string::npos) << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(metrics_observations_dropped_total{service_name="todo-service",metric="user_events_total"} 8)"))
      << exposition;
}

TEST(MetricsRegistryTest, ReMintingTheSameFamilyReturnsTheSameSeries) {
  // A helper handing out a handle repeatedly must not fork the family.
  MetricsRegistry registry(Enabled());
  auto first = registry.NewCounter("widgets_total", "Widgets.");
  auto second = registry.NewCounter("widgets_total", "Widgets.");
  first.Increment();
  second.Increment();
  EXPECT_TRUE(HasLine(registry.Expose(), "widgets_total 2")) << registry.Expose();
}

TEST(MetricsRegistryDeathTest, RegisteringAnInvalidOrCollidingNameAborts) {
  // Each of these emits a scrape Prometheus rejects in full, and nothing
  // in-process would notice — so they fail at registration (ADR-0009).
  EXPECT_DEATH(
      { MetricsRegistry(Enabled()).NewCounter("bad-name", "Dashes are not name characters."); },
      "");
  EXPECT_DEATH(
      {
        MetricsRegistry(Enabled()).NewCounter("http_server_requests_total", "Shadows a built-in.");
      },
      "");
  EXPECT_DEATH(
      {
        MetricsRegistry registry(Enabled());
        registry.NewCounter("thing_total", "One help string.");
        registry.NewGauge("thing_total", "One help string.");
      },
      "");
}

TEST(MetricsRegistryDeathTest, AnInvalidLabelNameAborts) {
  EXPECT_DEATH(
      {
        MetricsRegistry registry(Enabled());
        registry.NewCounter("things_total", "Things.").Increment({{"not a name", "v"}});
      },
      "");
}

TEST(MetricsRegistryTest, AHandleOutlivingItsRegistryIsInert) {
  // Handles share ownership of the family, so a stray one left in a
  // long-lived lambda updates something nobody exposes rather than dangling.
  Counter orphan = [] {
    MetricsRegistry registry(Enabled());
    return registry.NewCounter("orphan_total", "Orphaned.");
  }();
  orphan.Increment();  // must not crash under ASan
}

// ---------------------------------------------------------------------------
// The composed middleware.
// ---------------------------------------------------------------------------

TEST(MetricsEndpointTest, ServesTheExpositionWithThePrometheusContentType) {
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler = Chain({MetricsEndpoint(registry)}, Handler());

  const http::HttpResponse response = handler(Get("/metrics"));
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.headers.Get("content-type"), "text/plain; version=0.0.4; charset=utf-8");
  EXPECT_TRUE(HasLine(response.body, "# TYPE http_server_requests_total counter")) << response.body;
}

TEST(MetricsEndpointTest, OtherPathsPassThroughToTheHandler) {
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler = Chain({MetricsEndpoint(registry)}, Handler(201, "MakeThing"));

  const http::HttpResponse response = handler(Get("/things"));
  EXPECT_EQ(response.status, 201);
  EXPECT_EQ(response.operation, "MakeThing");
}

TEST(MetricsEndpointTest, IgnoresTheQueryStringOnItsOwnPath) {
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler = Chain({MetricsEndpoint(registry)}, Handler());
  EXPECT_EQ(handler(Get("/metrics?collect=all")).status, 200);
}

TEST(MetricsEndpointTest, AHeadIsAnsweredLikeTheGetBodyIncluded) {
  // The transport withholds the octets and keeps the length (RFC 9110
  // §9.3.2); emptying the body here would answer a false Content-Length.
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler = Chain({MetricsEndpoint(registry)}, Handler());

  http::HttpRequest head = Get("/metrics");
  head.method = "HEAD";
  const http::HttpResponse response = handler(head);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body, handler(Get("/metrics")).body);
}

TEST(MetricsEndpointTest, ARequestOnADifferentMethodFallsThrough) {
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler = Chain({MetricsEndpoint(registry)}, Handler(201, "MakeThing"));

  http::HttpRequest post = Get("/metrics");
  post.method = "POST";
  EXPECT_EQ(handler(post).status, 201);
}

TEST(MetricsEndpointTest, TheCanonicalChainRecordsTrafficButNotScrapes) {
  // The composition the header documents: the endpoint outside the recorder,
  // so a scrape answers without inflating the request rate it reports.
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler =
      Chain({MetricsEndpoint(registry), RecordMetrics(registry)}, Handler(200, "GetThing"));

  handler(Get("/things"));
  handler(Get("/things"));
  const std::string exposition = handler(Get("/metrics")).body;

  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="GetThing"} 2)"))
      << exposition;
  // Nothing recorded for the scrape itself: no /metrics route series.
  EXPECT_EQ(exposition.find(R"(route="/metrics")"), std::string::npos) << exposition;
}

TEST(MetricsEndpointTest, RecordMetricsCarriesTheOperationAndStatusFromTheResponse) {
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler =
      Chain({MetricsEndpoint(registry), RecordMetrics(registry)}, Handler(503, "GetThing"));

  handler(Get("/things"));
  EXPECT_TRUE(HasLine(
      handler(Get("/metrics")).body,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="GetThing"} 1)"));
}

TEST(MetricsEndpointTest, HealthProbesAreSeparableFromDispatchFailures) {
  // The reason HealthEndpoint labels its own path. Kubernetes polls a probe
  // every few seconds, so it is often the highest-volume "route" a service
  // has. Sharing the empty operation with 404s means the probe drowns the
  // signal in `http_server_requests_total{route="unmatched"}` and the 404 rate
  // cannot be read at all — and the probe's own latency, which is not the
  // service's, contaminates the same duration series.
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler =
      Chain({MetricsEndpoint(registry), RecordMetrics(registry), HealthEndpoint("/livez"),
             HealthEndpoint("/readyz", {{"db", [] { return false; }}})},
            [](const http::HttpRequest&) {
              http::HttpResponse response;  // the router's 404: no operation to stamp
              response.status = 404;
              return response;
            });

  handler(Get("/livez"));
  handler(Get("/readyz"));
  handler(Get("/nope"));

  const std::string exposition = handler(Get("/metrics")).body;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="/livez"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="/readyz"} 1)"))
      << exposition;
  // The 404 keeps the empty operation, and now means only that.
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="unmatched"} 1)"))
      << exposition;
  // Each probe has its own latency series, so `route!~"/livez|/readyz"` is
  // expressible; before the label none of these three could be told apart.
  // It is also what prom_proxy's `route!="/health"` subtraction depends on.
  EXPECT_TRUE(HasLine(
      exposition, R"(http_server_request_duration_microseconds_count{service_name="todo-service",)"
                  R"(http_method="GET",route="/livez"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition, R"(http_server_request_duration_microseconds_count{service_name="todo-service",)"
                  R"(http_method="GET",route="/readyz"} 1)"))
      << exposition;
}

TEST(MetricsEndpointTest, TheEndpointLabelsItselfWhenDeliberatelyRecorded) {
  // Inverted from the documented order on purpose: a user who wants scrape
  // volume as a signal gets a named series rather than an unlabeled one.
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler =
      Chain({RecordMetrics(registry), MetricsEndpoint(registry)}, Handler());

  handler(Get("/metrics"));
  const std::string exposition = handler(Get("/metrics")).body;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="/metrics"} 1)"))
      << exposition;
}

TEST(MetricsEndpointTest, AThrowingHandlerStillCompletesItsObservation) {
  // Observe pairs start and complete even when dispatch throws (reporting
  // 500 with an empty operation) — the gauge must come back down, or an
  // in-flight panel climbs forever after the first handler bug.
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler = Chain(
      {MetricsEndpoint(registry), RecordMetrics(registry)},
      [](const http::HttpRequest&) -> http::HttpResponse { throw std::runtime_error("bug"); });

  EXPECT_THROW(handler(Get("/things")), std::runtime_error);
  const std::string exposition = handler(Get("/metrics")).body;
  // The start did fire, so the series exists — and it came back down.
  EXPECT_TRUE(HasLine(exposition,
                      R"(http_server_requests_active_gauge{service_name="todo-service",)"
                      R"(http_method="GET"} 0)"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, R"(http_server_requests_total{service_name="todo-service",)"
                                  R"(http_method="GET",route="unmatched"} 1)"))
      << exposition;
  // A thrown handler reports 500, a failure by the 400 boundary.
  EXPECT_TRUE(HasLine(exposition,
                      R"(http_server_requests_failure_total{service_name="todo-service",)"
                      R"(http_method="GET",route="unmatched"} 1)"))
      << exposition;
}

// ---------------------------------------------------------------------------
// Off by default, and free while it is off.
// ---------------------------------------------------------------------------

TEST(DisabledRegistryTest, IsTheDefaultAndRecordsNothing) {
  // The default. A service can link, construct and wire the whole metrics
  // stack and still ship with it dark.
  MetricsRegistry registry;
  EXPECT_FALSE(registry.enabled());

  registry.Record(Served("GET", "GetThing", 200, microseconds(1000)));
  registry.RecordStart(RequestStart{.method = "GET", .target = "/things"});
  registry.RecordRejection("POST", 413);
  // Not "the families with no samples" — nothing at all. An empty 200 on
  // /metrics reads to Prometheus as a live target reporting no series, which
  // is what a service whose metrics have gone silent also looks like.
  EXPECT_EQ(registry.Expose(), "");
}

TEST(DisabledRegistryTest, HandlesAreInertRatherThanUnusable) {
  // Application code should not have to branch: the handles it already holds
  // keep working and simply record nothing, so `enabled` is a deployment
  // decision rather than a code-structure one.
  MetricsRegistry registry;
  auto orders = registry.NewCounter("orders_total", "Orders.");
  auto depth = registry.NewGauge("queue_depth", "Pending.");
  auto sizes = registry.NewHistogram("payload_bytes", "Payloads.", {10, 100});

  orders.Increment();
  orders.Increment({{"region", "us-east"}}, 5);
  orders.Declare({{"region", "eu-west"}});
  depth.Set(7);
  depth.Increment();
  depth.Decrement();
  depth.Declare();
  sizes.Observe(42);
  sizes.Declare();

  EXPECT_EQ(registry.Expose(), "");
}

TEST(DisabledRegistryTest, TheMiddlewareComposeToTheIdentity) {
  // The actual "zero cost" claim, and the reason it is not a per-request
  // branch: a disabled registry contributes no wrapper, so Chain hands back
  // the very handler it was given.
  //
  // A plain function is the terminal on purpose. std::function::target only
  // answers for the exact stored type, so `target<Fn>()` is non-null exactly
  // while the function is still what the chain calls, and goes null the
  // moment anything wraps it — which is what the enabled half below shows.
  using Fn = http::HttpResponse (*)(const http::HttpRequest&);
  const Fn terminal = [](const http::HttpRequest&) { return http::HttpResponse{}; };

  auto off = std::make_shared<MetricsRegistry>();
  const http::RequestHandler composed =
      Chain({MetricsEndpoint(off), RecordMetrics(off)}, http::RequestHandler(terminal));
  ASSERT_NE(composed.target<Fn>(), nullptr)
      << "a disabled registry wrapped the handler instead of composing away";
  EXPECT_EQ(*composed.target<Fn>(), terminal);

  auto on = std::make_shared<MetricsRegistry>(Enabled());
  const http::RequestHandler wrapped =
      Chain({MetricsEndpoint(on), RecordMetrics(on)}, http::RequestHandler(terminal));
  EXPECT_EQ(wrapped.target<Fn>(), nullptr)
      << "an enabled registry has to wrap, or the assertion above proves nothing";
}

TEST(DisabledRegistryTest, TheMetricsPathFallsThroughToTheRouter) {
  // Follows from composing away: /metrics is not a route this server has, so
  // it answers like any other unmodeled path rather than serving an empty
  // scrape that would look like a healthy target with nothing to say.
  auto registry = std::make_shared<MetricsRegistry>();
  http::RequestHandler handler =
      Chain({MetricsEndpoint(registry), RecordMetrics(registry)}, Handler(404, ""));
  const http::HttpResponse response = handler(Get("/metrics"));
  EXPECT_EQ(response.status, 404);
  EXPECT_EQ(response.body, "");
}

TEST(DisabledRegistryDeathTest, RegistrationStillValidates) {
  // The check that must not wait for someone to turn metrics on: if a bad
  // name only aborted when enabled, enabling it in production would be the
  // first time anyone found out.
  EXPECT_DEATH({ MetricsRegistry().NewCounter("bad-name", "Dashes are not names."); }, "");
  EXPECT_DEATH(
      { MetricsRegistry().NewCounter("http_server_requests_total", "Shadows a built-in."); }, "");
  EXPECT_DEATH(
      {
        MetricsRegistry registry;
        registry.NewCounter("thing_total", "One help string.");
        registry.NewGauge("thing_total", "One help string.");
      },
      "");
}

// ---------------------------------------------------------------------------
// The MoonBase serving contract, which is the only exposition this emits.
// Every literal below is pinned on the MoonBase side by
// //domains/platform/libs/otel_contract. These tests are what stops this rail
// drifting off it silently — and silence is how such a drift shows up: an
// empty dashboard panel, indistinguishable from a quiet service.
// ---------------------------------------------------------------------------

TEST(ExpositionContractTest, ExportsTheFiveSharedFamiliesUnderTheirPinnedNames) {
  // The names //domains/platform/libs/otel_contract pins across MoonBase's
  // three emitter rails, with the descriptions it pins with them: a
  // collector merging series by name keeps the first description it sees and
  // logs a conflict for every later one that disagrees.
  MetricsRegistry registry(Enabled());

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition, "# HELP http_server_requests_total HTTP requests received"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE http_server_requests_total counter")) << exposition;
  EXPECT_TRUE(HasLine(exposition,
                      "# HELP http_server_requests_success_total HTTP requests completed "
                      "successfully (2xx-3xx)"))
      << exposition;
  EXPECT_TRUE(
      HasLine(exposition,
              "# HELP http_server_requests_failure_total HTTP requests that returned 4xx or 5xx"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition,
                      "# HELP http_server_requests_active_gauge HTTP requests currently in flight"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE http_server_requests_active_gauge gauge")) << exposition;
  EXPECT_TRUE(HasLine(exposition,
                      "# HELP http_server_request_duration_microseconds HTTP request duration in "
                      "microseconds"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE http_server_request_duration_microseconds histogram"))
      << exposition;
  // The registry's own health rides along under a name of its own, outside
  // the contract.
  EXPECT_TRUE(HasLine(exposition, "# TYPE metrics_observations_dropped_total counter"))
      << exposition;
}

TEST(ExpositionContractTest, LabelsEverySeriesTheWayTheDashboardsSelect) {
  // prom_proxy selects `{service_name="x",route!="/health"}` on every query
  // it makes, so all three have to be present and spelled this way.
  MetricsRegistry registry(Enabled());
  registry.Record(Served("GET", "GetThing", 200, microseconds(1500)));

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="GetThing"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition, R"(http_server_request_duration_microseconds_count{service_name="todo-service",)"
                  R"(http_method="GET",route="GetThing"} 1)"))
      << exposition;
  // Microseconds, not seconds: the value is the hook's own unit, undivided.
  EXPECT_TRUE(HasLine(
      exposition, R"(http_server_request_duration_microseconds_sum{service_name="todo-service",)"
                  R"(http_method="GET",route="GetThing"} 1500)"))
      << exposition;
}

TEST(ExpositionContractTest, SuccessAndFailureSplitAtFourHundred) {
  // ErrorRatePercent is failure/(success+failure), so the split has to land
  // where the rest of the fleet draws it: 2xx-3xx succeeded, 4xx and 5xx did
  // not. The three counters are views of one tally, so they cannot disagree.
  MetricsRegistry registry(Enabled());
  registry.Record(Served("GET", "GetThing", 200, microseconds(100)));
  registry.Record(Served("GET", "GetThing", 301, microseconds(100)));
  registry.Record(Served("GET", "GetThing", 404, microseconds(100)));
  registry.Record(Served("GET", "GetThing", 500, microseconds(100)));

  const std::string exposition = registry.Expose();
  const std::string labels = R"(service_name="todo-service",http_method="GET",route="GetThing")";
  EXPECT_TRUE(HasLine(exposition, "http_server_requests_total{" + labels + "} 4")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "http_server_requests_success_total{" + labels + "} 2"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "http_server_requests_failure_total{" + labels + "} 2"))
      << exposition;
  // Status is not a label in this dialect — the outcome counters carry it,
  // and keeping both would multiply every series by the codes observed.
  EXPECT_EQ(exposition.find("status="), std::string::npos) << exposition;
}

TEST(ExpositionContractTest, TheActiveGaugeIsKeyedByMethodAndNeverByRoute) {
  // It moves at request start, before dispatch, where no bounded route is
  // known. Every rail leaves the route off it for that reason, and
  // prom_proxy's `route!="/health"` matcher passes a series without the
  // label through untouched — which is why the same filter is safe on it.
  MetricsRegistry registry(Enabled());
  registry.RecordStart(RequestStart{.method = "GET", .target = "/things/1"});
  registry.RecordStart(RequestStart{.method = "POST", .target = "/things"});
  registry.RecordStart(RequestStart{.method = "GET", .target = "/things/2"});

  std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_active_gauge{service_name="todo-service",http_method="GET"} 2)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_active_gauge{service_name="todo-service",http_method="POST"} 1)"))
      << exposition;
  EXPECT_EQ(
      exposition.find("active_gauge{service_name=\"todo-service\",http_method=\"GET\",route="),
      std::string::npos)
      << exposition;

  // And it comes back down, per method, when those requests complete.
  registry.Record(Served("GET", "GetThing", 200, microseconds(10)));
  registry.Record(Served("GET", "GetThing", 200, microseconds(10)));
  exposition = registry.Expose();
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_active_gauge{service_name="todo-service",http_method="GET"} 0)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_active_gauge{service_name="todo-service",http_method="POST"} 1)"))
      << exposition;
}

TEST(ExpositionContractTest, UsesTheRouteAndMethodSentinelsTheOtherRailsAgreedOn) {
  // Three constants that have to be byte-equal across the rails, because a
  // fleet-wide "unmatched traffic" query only means one thing if every
  // service spells it the same way.
  MetricsRegistry registry(Enabled());
  registry.Record(Served("GET", "", 404, microseconds(10)));
  registry.Record(Served("BREW", "GetThing", 200, microseconds(10)));
  registry.RecordRejection("", 431);

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="unmatched"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="CUSTOM",route="GetThing"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"line(http_server_requests_total{service_name="todo-service",http_method="(unparsed)",route="unmatched"} 1)line"))
      << exposition;
  // Never the empty route: `route!="/health"` would match it, so unrouted
  // traffic would silently join the serving numbers.
  EXPECT_EQ(exposition.find(R"(route="")"), std::string::npos) << exposition;
}

TEST(ExpositionContractTest, TheHealthProbeLandsOnTheRouteThePanelsSubtract) {
  // The composition that makes the /health literal real: prom_proxy
  // subtracts route!="/health" from every serving number and charts that
  // route on its own Probes tile, so a service that never reports it reads
  // as having no probe rather than as a healthy one. HealthEndpoint's
  // default path is already the literal.
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler = Chain(
      {MetricsEndpoint(registry), RecordMetrics(registry), HealthEndpoint()}, Handler(404, ""));

  handler(Get("/health"));
  handler(Get("/things/1"));

  const std::string exposition = handler(Get("/metrics")).body;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="/health"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(http_server_requests_total{service_name="todo-service",http_method="GET",route="unmatched"} 1)"))
      << exposition;
}

TEST(ExpositionContractTest, UsesTheMicrosecondBucketLadderTheRailsShare) {
  // histogram_quantile reads `le` off bucket counts, so p95 only compares
  // like with like when the boundaries match. These are pinned equal across
  // the three rails by //domains/platform/libs/otel_contract; a service on a
  // different ladder charts a quantile computed against different bins than
  // everything beside it.
  MetricsRegistry registry(Enabled());
  EXPECT_EQ(HttpLatencyBuckets(),
            (std::vector<double>{100, 250, 500, 1000, 2500, 5000, 10000, 25000, 50000, 100000,
                                 250000, 500000, 1000000, 2500000, 10000000}));

  registry.Record(Served("GET", "GetThing", 200, microseconds(100)));
  const std::string exposition = registry.Expose();
  const std::string labels = R"(service_name="todo-service",http_method="GET",route="GetThing")";
  // Buckets are upper-inclusive, which is what `le` means: exactly 100µs
  // belongs in the 100 bucket, not the one above it.
  EXPECT_TRUE(HasLine(
      exposition, "http_server_request_duration_microseconds_bucket{" + labels + R"(,le="100"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "http_server_request_duration_microseconds_bucket{" + labels +
                                      R"(,le="10000000"} 1)"))
      << exposition;
}

TEST(ExpositionContractTest, ApplicationMetricsStillShareTheScrape) {
  // Switching dialects changes the built-in vocabulary, not the endpoint: a
  // service's own numbers still ride the same target.
  MetricsRegistry registry(Enabled());
  auto orders = registry.NewCounter("orders_processed_total", "Orders processed.");
  orders.Increment();

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition, "orders_processed_total 1")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE http_server_requests_total counter")) << exposition;
}

TEST(ExpositionContractDeathTest, EveryBuiltInNameIsReserved) {
  // All six, not just the ones a test happened to name: a family shadowing
  // any of them appears twice with two TYPE lines, which Prometheus rejects
  // whole rather than per line.
  for (const std::string name :
       {"http_server_requests_total", "http_server_requests_success_total",
        "http_server_requests_failure_total", "http_server_requests_active_gauge",
        "http_server_request_duration_microseconds", "metrics_observations_dropped_total"}) {
    EXPECT_DEATH(
        { MetricsRegistry(Enabled()).NewCounter(name, "Shadows a built-in."); }, "")
        << name;
  }
}

TEST(ExpositionContractDeathTest, AnEnabledRegistryWithoutAServiceNameAborts) {
  // Every dashboard query selects on service_name, so a service reporting
  // the empty string is scraped, stored, and absent from all of them —
  // success everywhere except the panel nobody is watching yet.
  EXPECT_DEATH({ MetricsRegistry registry(MetricsOptions{.enabled = true}); }, "");
  // Disabled it is not required: nothing is exposed to be unfindable.
  MetricsRegistry disabled{};
  EXPECT_FALSE(disabled.enabled());
}

// Four pins adapted from MoonBase's own rails (aura/middleware_test.cc and
// futility/otel/http_metrics_test.cc). Each catches a class of drift that
// the per-value assertions above would miss, and each is asserted here
// against the rendered scrape rather than against a recording sink — which
// is strictly stronger, since the scrape is what Prometheus actually reads.

// Every sample line of a built-in family, stripped of its value.
std::vector<std::string> BuiltInSampleLines(const std::string& exposition) {
  std::vector<std::string> lines;
  std::istringstream stream(exposition);
  for (std::string line; std::getline(stream, line);) {
    if (line.starts_with("http_server_")) {
      lines.push_back(line);
    }
  }
  return lines;
}

TEST(ExpositionContractTest, NoSeriesCarriesTheOldMethodSpelling) {
  // futility's #1305 pin, which it needed because that rail alone had
  // historically spelled the label `method`. A single stray old spelling
  // forks every dashboard series for this service — the query selects
  // http_method, finds nothing, and charts an empty panel. Swept across all
  // five families rather than asserted per call site, so a family added
  // later cannot quietly reintroduce it.
  MetricsRegistry registry(Enabled());
  registry.RecordStart(RequestStart{.method = "GET", .target = "/things"});
  registry.Record(Served("GET", "GetThing", 200, microseconds(1000)));
  registry.Record(Served("POST", "PutThing", 500, microseconds(1000)));
  registry.RecordRejection("PUT", 413);

  const std::vector<std::string> lines = BuiltInSampleLines(registry.Expose());
  ASSERT_FALSE(lines.empty());
  for (const std::string& line : lines) {
    EXPECT_EQ(line.find("{method=\""), std::string::npos) << line;
    EXPECT_EQ(line.find(",method=\""), std::string::npos) << line;
    EXPECT_NE(line.find("service_name=\"todo-service\""), std::string::npos) << line;
    EXPECT_NE(line.find("http_method=\""), std::string::npos) << line;
  }
}

TEST(ExpositionContractTest, AQueryStringDoesNotDefeatTheHealthRoute) {
  // The probe is polled with a query string by plenty of orchestrators. If
  // that pushed it off the /health route, prom_proxy's subtraction would
  // stop matching and the probe's volume would silently rejoin the serving
  // numbers — the exact arithmetic error the route label exists to prevent.
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler = Chain(
      {MetricsEndpoint(registry), RecordMetrics(registry), HealthEndpoint()}, Handler(404, ""));

  handler(Get("/health?probe=1"));
  EXPECT_TRUE(HasLine(handler(Get("/metrics")).body,
                      R"(http_server_requests_total{service_name="todo-service",)"
                      R"(http_method="GET",route="/health"} 1)"))
      << handler(Get("/metrics")).body;
}

TEST(ExpositionContractTest, ScannerPathsCollapseIntoOneSeries) {
  // The same cardinality rule the cap backstops, stated the way an operator
  // meets it: a scanner walking distinct paths must not mint a series per
  // path. The cap would eventually stop it, but only after the damage —
  // collapsing at the label is what keeps it from starting.
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler =
      Chain({MetricsEndpoint(registry), RecordMetrics(registry)}, Handler(404, ""));

  for (const std::string target : {"/wp-login.php", "/admin/config", "/v1/nope?x=1"}) {
    handler(Get(target));
  }

  const std::string exposition = handler(Get("/metrics")).body;
  EXPECT_TRUE(HasLine(exposition, R"(http_server_requests_total{service_name="todo-service",)"
                                  R"(http_method="GET",route="unmatched"} 3)"))
      << exposition;
  for (const std::string fragment : {"wp-login", "admin/config", "v1/nope"}) {
    EXPECT_EQ(exposition.find(fragment), std::string::npos)
        << "a scanned path reached a label: " << exposition;
  }
}

TEST(ExpositionContractTest, InventedMethodsCollapseOnTheGaugeToo) {
  // The method label is bounded on the counters (asserted above), but the
  // gauge is keyed by method as well and moves at request *start* — so a
  // flood of invented verbs would mint a gauge series each unless the same
  // normalization runs there. Lowercase "get" is deliberately in the set:
  // methods are case-sensitive, so it is an invented token, not GET.
  MetricsRegistry registry(Enabled());
  for (const std::string method : {"FOOBAR1", "FOOBAR2", "get"}) {
    registry.RecordStart(RequestStart{.method = method, .target = "/echo"});
  }

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition,
                      R"(http_server_requests_active_gauge{service_name="todo-service",)"
                      R"(http_method="CUSTOM"} 3)"))
      << exposition;
  for (const std::string token : {"FOOBAR1", "FOOBAR2", R"(http_method="get")"}) {
    EXPECT_EQ(exposition.find(token), std::string::npos) << exposition;
  }
}

// The name of the metric a sample line reports, or "" for a comment or a
// blank. Everything before the '{' or the space, which is what the format
// requires to be grouped.
std::string SampleMetricName(const std::string& line) {
  if (line.empty() || line.starts_with('#')) {
    return "";
  }
  const std::size_t end = line.find_first_of("{ ");
  return end == std::string::npos ? line : line.substr(0, end);
}

// The first metric name whose sample lines are split into two or more
// blocks, or "" when every family is contiguous.
std::string FirstSplitFamily(const std::string& exposition) {
  std::vector<std::string> order;
  std::istringstream stream(exposition);
  for (std::string line; std::getline(stream, line);) {
    const std::string name = SampleMetricName(line);
    if (!name.empty() && (order.empty() || order.back() != name)) {
      order.push_back(name);
    }
  }
  std::set<std::string> seen;
  for (const std::string& name : order) {
    if (!seen.insert(name).second) {
      return name;
    }
  }
  return "";
}

TEST(ExpositionContractTest, EveryFamilyIsContiguous) {
  // Prometheus 0.0.4 requires all lines of a metric to arrive as one group.
  // The drop counter used to be emitted unlabeled, then the application
  // families, then its own per-family attributions — splitting it in two,
  // which a strict parser rejects and a lenient one silently mis-stores.
  MetricsOptions options = Enabled();
  options.max_series = 1;
  MetricsRegistry registry(options);
  registry.RecordStart(RequestStart{.method = "GET", .target = "/things"});
  registry.Record(Served("GET", "GetThing", 200, microseconds(1000)));
  registry.Record(Served("POST", "PutThing", 500, microseconds(1000)));  // over the cap

  auto counter = registry.NewCounter("widgets_total", "Widgets.");
  auto gauge = registry.NewGauge("queue_depth", "Pending.");
  auto sizes = registry.NewHistogram("payload_bytes", "Payloads.", {10, 100});
  counter.Increment();
  gauge.Set(3);
  sizes.Observe(42);
  for (int i = 0; i < 5; ++i) {
    counter.Increment({{"shard", std::to_string(i)}});  // over the cap, attributed
  }

  const std::string exposition = registry.Expose();
  EXPECT_EQ(FirstSplitFamily(exposition), "") << exposition;
  // And the attribution is still there, next to the unlabeled total.
  EXPECT_TRUE(HasLine(exposition,
                      R"(metrics_observations_dropped_total{service_name="todo-service",)"
                      R"(metric="widgets_total"} 5)"))
      << exposition;
}

TEST(ExpositionContractTest, HelpTextIsEscapedSoItCannotForgeALine) {
  // HELP runs to the end of the line and an application family's help is a
  // caller's string. A newline in it would end the HELP line early and let
  // whatever follows pose as its own directive.
  MetricsRegistry registry(Enabled());
  auto counter = registry.NewCounter("widgets_total", "Widgets.\n# TYPE forged_total counter");
  counter.Increment();

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition, R"(# HELP widgets_total Widgets.\n# TYPE forged_total counter)"))
      << exposition;
  EXPECT_FALSE(HasLine(exposition, "# TYPE forged_total counter"))
      << "a newline in help forged a TYPE line: " << exposition;

  // A backslash is escaped too, or it would eat the character after it.
  MetricsRegistry other(Enabled());
  auto slashed = other.NewCounter("paths_total", R"(Paths like C:\temp.)");
  slashed.Increment();
  EXPECT_TRUE(HasLine(other.Expose(), R"(# HELP paths_total Paths like C:\\temp.)"))
      << other.Expose();
}

TEST(MetricsRegistryTest, TheCapCannotSplitACountFromItsLatency) {
  // The counters and the histogram share one key and one admission, so a
  // route that is past the cap is refused from all four families at once.
  // They were once keyed differently — counters by {method,route,status},
  // the histogram by {method,route} — so the counter map filled first, and
  // past that point a new status on an already-admitted route was refused
  // by the counters while the histogram, whose key already existed, kept
  // recording. requests_total silently stopped counting while _count rose.
  MetricsOptions options = Enabled();
  options.max_series = 2;
  MetricsRegistry registry(options);
  registry.Record(Served("GET", "GetThing", 200, microseconds(1000)));
  registry.Record(Served("GET", "PutThing", 200, microseconds(1000)));  // cap now full

  // A new status on an already-admitted route: same key, so it counts.
  registry.Record(Served("GET", "GetThing", 500, microseconds(1000)));
  // A genuinely new route: refused, and counted as refused.
  registry.Record(Served("GET", "Unseen", 200, microseconds(1000)));

  const std::string exposition = registry.Expose();
  const std::string labels = R"(service_name="todo-service",http_method="GET",route="GetThing")";
  EXPECT_TRUE(HasLine(exposition, "http_server_requests_total{" + labels + "} 2")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "http_server_requests_success_total{" + labels + "} 1"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "http_server_requests_failure_total{" + labels + "} 1"))
      << exposition;
  // The number that used to drift: the histogram agrees with the counter.
  EXPECT_TRUE(
      HasLine(exposition, "http_server_request_duration_microseconds_count{" + labels + "} 2"))
      << exposition;

  EXPECT_EQ(exposition.find(R"(route="Unseen")"), std::string::npos) << exposition;
  EXPECT_TRUE(
      HasLine(exposition, R"(metrics_observations_dropped_total{service_name="todo-service"} 1)"))
      << exposition;
}

TEST(MetricsRegistryTest, ARejectedRouteIsCountedWithoutInventingAHistogram) {
  // Rejections share the unmatched route with 404s. The route is counted,
  // and the histogram counts only the requests that were actually timed —
  // an all-zero histogram would claim an observation nobody filed.
  MetricsRegistry registry(Enabled());
  registry.RecordRejection("PUT", 413);
  registry.RecordRejection("PUT", 413);

  const std::string exposition = registry.Expose();
  const std::string labels = R"(service_name="todo-service",http_method="PUT",route="unmatched")";
  EXPECT_TRUE(HasLine(exposition, "http_server_requests_total{" + labels + "} 2")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "http_server_requests_failure_total{" + labels + "} 2"))
      << exposition;
  EXPECT_EQ(exposition.find("http_server_request_duration_microseconds_count{" + labels),
            std::string::npos)
      << exposition;
}

TEST(MetricsRegistryDeathTest, AnUnusableHistogramLadderAborts) {
  // The guide promises this, and the built-in ladder gets it for free by
  // being a constant. An unsorted ladder yields cumulative buckets that
  // disagree with themselves — plausible nonsense on a dashboard rather
  // than a loud failure (ADR-0009).
  EXPECT_DEATH({ MetricsRegistry(Enabled()).NewHistogram("a_bytes", "A.", {}); }, "");
  const std::vector<double> descending = {100, 10};
  EXPECT_DEATH({ MetricsRegistry(Enabled()).NewHistogram("b_bytes", "B.", descending); }, "");
  const std::vector<double> repeated = {10, 10};
  EXPECT_DEATH({ MetricsRegistry(Enabled()).NewHistogram("c_bytes", "C.", repeated); }, "");
  const std::vector<double> infinite = {10, std::numeric_limits<double>::infinity()};
  EXPECT_DEATH({ MetricsRegistry(Enabled()).NewHistogram("d_bytes", "D.", infinite); }, "");
}

}  // namespace
}  // namespace smithy::server
