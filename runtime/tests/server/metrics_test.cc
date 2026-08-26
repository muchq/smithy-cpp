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
#include <memory>
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
  MetricsRegistry registry;
  registry.Record(Served("GET", "GetThing", 200, microseconds(1000)));
  registry.Record(Served("GET", "GetThing", 200, microseconds(2000)));
  registry.Record(Served("POST", "PutThing", 500, microseconds(3000)));

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(
      HasLine(exposition,
              R"(smithy_http_requests_total{method="GET",operation="GetThing",status="200"} 2)"))
      << exposition;
  EXPECT_TRUE(
      HasLine(exposition,
              R"(smithy_http_requests_total{method="POST",operation="PutThing",status="500"} 1)"))
      << exposition;
}

TEST(MetricsRegistryTest, EmitsTheFamilyHeadersEvenBeforeAnyTraffic) {
  // A freshly started server should still describe its shape, so a scrape
  // configured against it is verifiable before the first request arrives.
  const std::string exposition = MetricsRegistry().Expose();
  EXPECT_TRUE(HasLine(exposition, "# TYPE smithy_http_requests_total counter")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE smithy_http_request_duration_seconds histogram"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE smithy_http_requests_in_flight gauge")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "smithy_http_requests_in_flight 0")) << exposition;
}

TEST(MetricsRegistryTest, HistogramBucketsAreCumulativeAndEndAtInf) {
  MetricsRegistry registry(4096, {0.01, 0.1});
  registry.Record(Served("GET", "GetThing", 200, microseconds(5000)));    // 0.005s -> first bucket
  registry.Record(Served("GET", "GetThing", 200, microseconds(50000)));   // 0.05s  -> second
  registry.Record(Served("GET", "GetThing", 200, microseconds(500000)));  // 0.5s   -> only +Inf

  const std::string exposition = registry.Expose();
  const std::string labels = R"(method="GET",operation="GetThing")";
  EXPECT_TRUE(HasLine(exposition,
                      "smithy_http_request_duration_seconds_bucket{" + labels + R"(,le="0.01"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition,
                      "smithy_http_request_duration_seconds_bucket{" + labels + R"(,le="0.1"} 2)"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition,
                      "smithy_http_request_duration_seconds_bucket{" + labels + R"(,le="+Inf"} 3)"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "smithy_http_request_duration_seconds_count{" + labels + "} 3"))
      << exposition;
  // 0.005 + 0.05 + 0.5, formatted without trailing-zero noise.
  EXPECT_TRUE(HasLine(exposition, "smithy_http_request_duration_seconds_sum{" + labels + "} 0.555"))
      << exposition;
}

TEST(MetricsRegistryTest, SubMillisecondLatenciesSurviveTheMicrosecondHook) {
  // The hook is microseconds precisely so cache hits and loopback don't
  // report as zero (#92); the seconds conversion must not undo that.
  MetricsRegistry registry;
  registry.Record(Served("GET", "GetThing", 200, microseconds(1)));
  EXPECT_TRUE(HasLine(
      registry.Expose(),
      R"(smithy_http_request_duration_seconds_sum{method="GET",operation="GetThing"} 0.000001)"))
      << registry.Expose();
}

TEST(MetricsRegistryTest, DispatchFailuresCountUnderAnEmptyOperation) {
  // 404/405/400 never reached an operation, so the label is empty rather
  // than inventing one — and the target that caused it is deliberately not
  // a label at all.
  MetricsRegistry registry;
  registry.Record(Served("GET", "", 404, microseconds(100)));
  EXPECT_TRUE(HasLine(registry.Expose(),
                      R"(smithy_http_requests_total{method="GET",operation="",status="404"} 1)"))
      << registry.Expose();
}

TEST(MetricsRegistryTest, RecordsConcurrentlyWithoutLosingCounts) {
  MetricsRegistry registry;
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
  EXPECT_TRUE(
      HasLine(registry.Expose(),
              R"(smithy_http_requests_total{method="GET",operation="GetThing",status="200"} )" +
                  std::to_string(kThreads * kPerThread)))
      << registry.Expose();
}

// ---------------------------------------------------------------------------
// Cardinality: the failure mode a metrics endpoint dies of.
// ---------------------------------------------------------------------------

TEST(MetricsRegistryTest, AnInventedMethodCollapsesInsteadOfMintingASeries) {
  // The method comes off the wire, so a loop of `curl -X <random>` is a
  // memory-exhaustion vector if it reaches the label set verbatim.
  MetricsRegistry registry;
  for (int i = 0; i < 100; ++i) {
    registry.Record(Served("BOGUS" + std::to_string(i), "", 405, microseconds(10)));
  }
  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(
      exposition, R"(smithy_http_requests_total{method="other",operation="",status="405"} 100)"))
      << exposition;
  EXPECT_EQ(exposition.find("BOGUS"), std::string::npos) << exposition;
}

TEST(MetricsRegistryTest, LowercaseMethodIsNotFoldedIntoTheRealOne) {
  // HTTP methods are case-sensitive (RFC 9110 §9.1): a "get" the server
  // rejected must not report as served GET traffic.
  MetricsRegistry registry;
  registry.Record(Served("get", "", 405, microseconds(10)));
  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition,
                      R"(smithy_http_requests_total{method="other",operation="",status="405"} 1)"))
      << exposition;
}

TEST(MetricsRegistryTest, TheSeriesCapStopsGrowthAndSaysSoOutLoud) {
  // The backstop for an unbounded operation stamped by a hand-written
  // handler: stop minting, and expose the drops so it can be alerted on
  // rather than discovered as an OOM.
  MetricsRegistry registry(/*max_series=*/4);
  for (int i = 0; i < 50; ++i) {
    registry.Record(Served("GET", "Op" + std::to_string(i), 200, microseconds(10)));
  }
  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition,
                      R"(smithy_http_requests_total{method="GET",operation="Op0",status="200"} 1)"))
      << exposition;
  EXPECT_EQ(exposition.find(R"(operation="Op49")"), std::string::npos) << exposition;
  // Four combinations fit; the remaining 46 observations are refused, and
  // each is counted exactly once even though both families turned it away.
  EXPECT_TRUE(HasLine(exposition, "smithy_metrics_observations_dropped_total 46")) << exposition;
}

TEST(MetricsRegistryTest, LabelValuesAreEscapedSoTheScrapeStaysParseable) {
  // An operation is bounded by the model, but a hand-written handler can
  // stamp anything; an unescaped quote would corrupt the whole scrape.
  MetricsRegistry registry;
  registry.Record(Served("GET", R"(We"ird\Op)", 200, microseconds(10)));
  EXPECT_TRUE(
      HasLine(registry.Expose(),
              R"(smithy_http_requests_total{method="GET",operation="We\"ird\\Op",status="200"} 1)"))
      << registry.Expose();
}

// ---------------------------------------------------------------------------
// The in-flight gauge.
// ---------------------------------------------------------------------------

TEST(MetricsRegistryTest, InFlightRisesOnStartAndFallsOnCompletion) {
  MetricsRegistry registry;
  registry.RecordStart(RequestStart{.method = "GET", .target = "/a"});
  registry.RecordStart(RequestStart{.method = "GET", .target = "/b"});
  EXPECT_TRUE(HasLine(registry.Expose(), "smithy_http_requests_in_flight 2")) << registry.Expose();

  registry.Record(Served("GET", "GetThing", 200, microseconds(10)));
  EXPECT_TRUE(HasLine(registry.Expose(), "smithy_http_requests_in_flight 1")) << registry.Expose();
}

TEST(MetricsRegistryTest, CompletionsWithoutStartsLeaveTheGaugeAtZero) {
  // RecordStart is optional; an unpaired completion must not drive the gauge
  // negative, which would render as a nonsense dashboard forever after.
  MetricsRegistry registry;
  registry.Record(Served("GET", "GetThing", 200, microseconds(10)));
  registry.Record(Served("GET", "GetThing", 200, microseconds(10)));
  EXPECT_TRUE(HasLine(registry.Expose(), "smithy_http_requests_in_flight 0")) << registry.Expose();
}

// ---------------------------------------------------------------------------
// Application metrics.
// ---------------------------------------------------------------------------

TEST(MetricsRegistryTest, ACustomCounterJoinsTheSameScrape) {
  MetricsRegistry registry;
  auto orders = registry.NewCounter("orders_processed_total", "Orders processed.");
  orders.Increment();
  orders.Increment({{"region", "us-east"}}, 4);

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition, "# HELP orders_processed_total Orders processed.")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE orders_processed_total counter")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "orders_processed_total 1")) << exposition;
  EXPECT_TRUE(HasLine(exposition, R"(orders_processed_total{region="us-east"} 4)")) << exposition;
  // The built-in families are still there, whole.
  EXPECT_TRUE(HasLine(exposition, "# TYPE smithy_http_requests_total counter")) << exposition;
}

TEST(MetricsRegistryTest, AGaugeGoesUpAndDown) {
  MetricsRegistry registry;
  auto depth = registry.NewGauge("queue_depth", "Pending jobs.");
  depth.Set(10);
  depth.Increment(5);
  depth.Decrement(3);
  EXPECT_TRUE(HasLine(registry.Expose(), "queue_depth 12")) << registry.Expose();
}

TEST(MetricsRegistryTest, ACustomHistogramExposesBucketsSumAndCount) {
  MetricsRegistry registry;
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
  MetricsRegistry registry;
  auto hits = registry.NewCounter("cache_hits_total", "Cache hits.");
  hits.Increment({{"tier", "hot"}, {"region", "eu"}});
  hits.Increment({{"region", "eu"}, {"tier", "hot"}});
  EXPECT_TRUE(HasLine(registry.Expose(), R"(cache_hits_total{region="eu",tier="hot"} 2)"))
      << registry.Expose();
}

TEST(MetricsRegistryTest, ACustomLabelValueIsEscaped) {
  // All three the exposition format requires: quote, backslash, newline. An
  // unescaped one corrupts the whole scrape, not just this line.
  MetricsRegistry registry;
  auto errors = registry.NewCounter("job_errors_total", "Job errors.");
  errors.Increment({{"reason", "quote\" back\\slash\nnewline"}});
  EXPECT_TRUE(
      HasLine(registry.Expose(), R"(job_errors_total{reason="quote\" back\\slash\nnewline"} 1)"))
      << registry.Expose();
}

TEST(MetricsRegistryTest, AnUnboundedCustomLabelIsCappedAndAttributed) {
  // The whole point of the per-family cap: a label taken from unbounded data
  // costs that family its budget and says so, instead of the process.
  MetricsRegistry registry(/*max_series=*/4);
  auto seen = registry.NewCounter("user_events_total", "User events.");
  for (int i = 0; i < 50; ++i) {
    seen.Increment({{"user_id", std::to_string(i)}});
  }
  const std::string exposition = registry.Expose();
  EXPECT_EQ(exposition.find(R"(user_id="49")"), std::string::npos) << exposition;
  EXPECT_TRUE(HasLine(
      exposition, R"(smithy_metrics_observations_dropped_total{metric="user_events_total"} 46)"))
      << exposition;
}

TEST(MetricsRegistryTest, ReMintingTheSameFamilyReturnsTheSameSeries) {
  // A helper handing out a handle repeatedly must not fork the family.
  MetricsRegistry registry;
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
      { MetricsRegistry().NewCounter("bad-name", "Dashes are not name characters."); }, "");
  EXPECT_DEATH(
      { MetricsRegistry().NewCounter("smithy_http_requests_total", "Shadows a built-in."); }, "");
  EXPECT_DEATH(
      {
        MetricsRegistry registry;
        registry.NewCounter("thing_total", "One help string.");
        registry.NewGauge("thing_total", "One help string.");
      },
      "");
}

TEST(MetricsRegistryDeathTest, AnInvalidLabelNameAborts) {
  EXPECT_DEATH(
      {
        MetricsRegistry registry;
        registry.NewCounter("things_total", "Things.").Increment({{"not a name", "v"}});
      },
      "");
}

TEST(MetricsRegistryTest, AHandleOutlivingItsRegistryIsInert) {
  // Handles share ownership of the family, so a stray one left in a
  // long-lived lambda updates something nobody exposes rather than dangling.
  Counter orphan = [] {
    MetricsRegistry registry;
    return registry.NewCounter("orphan_total", "Orphaned.");
  }();
  orphan.Increment();  // must not crash under ASan
}

// ---------------------------------------------------------------------------
// The composed middleware.
// ---------------------------------------------------------------------------

TEST(MetricsEndpointTest, ServesTheExpositionWithThePrometheusContentType) {
  auto registry = std::make_shared<MetricsRegistry>();
  http::RequestHandler handler = Chain({MetricsEndpoint(registry)}, Handler());

  const http::HttpResponse response = handler(Get("/metrics"));
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.headers.Get("content-type"), "text/plain; version=0.0.4; charset=utf-8");
  EXPECT_TRUE(HasLine(response.body, "# TYPE smithy_http_requests_total counter")) << response.body;
}

TEST(MetricsEndpointTest, OtherPathsPassThroughToTheHandler) {
  auto registry = std::make_shared<MetricsRegistry>();
  http::RequestHandler handler = Chain({MetricsEndpoint(registry)}, Handler(201, "MakeThing"));

  const http::HttpResponse response = handler(Get("/things"));
  EXPECT_EQ(response.status, 201);
  EXPECT_EQ(response.operation, "MakeThing");
}

TEST(MetricsEndpointTest, IgnoresTheQueryStringOnItsOwnPath) {
  auto registry = std::make_shared<MetricsRegistry>();
  http::RequestHandler handler = Chain({MetricsEndpoint(registry)}, Handler());
  EXPECT_EQ(handler(Get("/metrics?collect=all")).status, 200);
}

TEST(MetricsEndpointTest, AHeadIsAnsweredLikeTheGetBodyIncluded) {
  // The transport withholds the octets and keeps the length (RFC 9110
  // §9.3.2); emptying the body here would answer a false Content-Length.
  auto registry = std::make_shared<MetricsRegistry>();
  http::RequestHandler handler = Chain({MetricsEndpoint(registry)}, Handler());

  http::HttpRequest head = Get("/metrics");
  head.method = "HEAD";
  const http::HttpResponse response = handler(head);
  EXPECT_EQ(response.status, 200);
  EXPECT_EQ(response.body, handler(Get("/metrics")).body);
}

TEST(MetricsEndpointTest, ARequestOnADifferentMethodFallsThrough) {
  auto registry = std::make_shared<MetricsRegistry>();
  http::RequestHandler handler = Chain({MetricsEndpoint(registry)}, Handler(201, "MakeThing"));

  http::HttpRequest post = Get("/metrics");
  post.method = "POST";
  EXPECT_EQ(handler(post).status, 201);
}

TEST(MetricsEndpointTest, TheCanonicalChainRecordsTrafficButNotScrapes) {
  // The composition the header documents: the endpoint outside the recorder,
  // so a scrape answers without inflating the request rate it reports.
  auto registry = std::make_shared<MetricsRegistry>();
  http::RequestHandler handler =
      Chain({MetricsEndpoint(registry), RecordMetrics(registry)}, Handler(200, "GetThing"));

  handler(Get("/things"));
  handler(Get("/things"));
  const std::string exposition = handler(Get("/metrics")).body;

  EXPECT_TRUE(
      HasLine(exposition,
              R"(smithy_http_requests_total{method="GET",operation="GetThing",status="200"} 2)"))
      << exposition;
  // Nothing recorded for the scrape itself: no empty-operation series.
  EXPECT_EQ(exposition.find(R"(operation="",status="200")"), std::string::npos) << exposition;
}

TEST(MetricsEndpointTest, RecordMetricsCarriesTheOperationAndStatusFromTheResponse) {
  auto registry = std::make_shared<MetricsRegistry>();
  http::RequestHandler handler =
      Chain({MetricsEndpoint(registry), RecordMetrics(registry)}, Handler(503, "GetThing"));

  handler(Get("/things"));
  EXPECT_TRUE(
      HasLine(handler(Get("/metrics")).body,
              R"(smithy_http_requests_total{method="GET",operation="GetThing",status="503"} 1)"));
}

TEST(MetricsEndpointTest, AThrowingHandlerStillCompletesItsObservation) {
  // Observe pairs start and complete even when dispatch throws (reporting
  // 500 with an empty operation) — the gauge must come back down, or an
  // in-flight panel climbs forever after the first handler bug.
  auto registry = std::make_shared<MetricsRegistry>();
  http::RequestHandler handler = Chain(
      {MetricsEndpoint(registry), RecordMetrics(registry)},
      [](const http::HttpRequest&) -> http::HttpResponse { throw std::runtime_error("bug"); });

  EXPECT_THROW(handler(Get("/things")), std::runtime_error);
  const std::string exposition = handler(Get("/metrics")).body;
  EXPECT_TRUE(HasLine(exposition, "smithy_http_requests_in_flight 0")) << exposition;
  EXPECT_TRUE(HasLine(exposition,
                      R"(smithy_http_requests_total{method="GET",operation="",status="500"} 1)"))
      << exposition;
}

}  // namespace
}  // namespace smithy::server
