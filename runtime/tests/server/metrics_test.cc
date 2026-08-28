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

// Every test that expects a registry to record anything has to turn it on:
// MetricsOptions::enabled is false by default so that a metrics stack that
// is linked in but not switched on costs nothing. The DisabledRegistry tests
// below pin that default and what it buys.
MetricsOptions Enabled() {
  MetricsOptions options;
  options.enabled = true;
  return options;
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
  MetricsRegistry registry(Enabled());
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
  const std::string exposition = MetricsRegistry(Enabled()).Expose();
  EXPECT_TRUE(HasLine(exposition, "# TYPE smithy_http_requests_total counter")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE smithy_http_request_duration_seconds histogram"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE smithy_http_requests_in_flight gauge")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "smithy_http_requests_in_flight 0")) << exposition;
}

TEST(MetricsRegistryTest, HistogramBucketsAreCumulativeAndEndAtInf) {
  MetricsOptions options = Enabled();
  options.latency_buckets = {0.01, 0.1};
  MetricsRegistry registry(options);
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
  MetricsRegistry registry(Enabled());
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
  MetricsRegistry registry(Enabled());
  registry.Record(Served("GET", "", 404, microseconds(100)));
  EXPECT_TRUE(HasLine(registry.Expose(),
                      R"(smithy_http_requests_total{method="GET",operation="",status="404"} 1)"))
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
  MetricsRegistry registry(Enabled());
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
  MetricsRegistry registry(Enabled());
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
  MetricsOptions options = Enabled();
  options.max_series = 4;
  MetricsRegistry registry(options);
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
  MetricsRegistry registry(Enabled());
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
  MetricsRegistry registry(Enabled());
  registry.RecordStart(RequestStart{.method = "GET", .target = "/a"});
  registry.RecordStart(RequestStart{.method = "GET", .target = "/b"});
  EXPECT_TRUE(HasLine(registry.Expose(), "smithy_http_requests_in_flight 2")) << registry.Expose();

  registry.Record(Served("GET", "GetThing", 200, microseconds(10)));
  EXPECT_TRUE(HasLine(registry.Expose(), "smithy_http_requests_in_flight 1")) << registry.Expose();
}

TEST(MetricsRegistryTest, CompletionsWithoutStartsLeaveTheGaugeAtZero) {
  // RecordStart is optional; an unpaired completion must not drive the gauge
  // negative, which would render as a nonsense dashboard forever after.
  MetricsRegistry registry(Enabled());
  registry.Record(Served("GET", "GetThing", 200, microseconds(10)));
  registry.Record(Served("GET", "GetThing", 200, microseconds(10)));
  EXPECT_TRUE(HasLine(registry.Expose(), "smithy_http_requests_in_flight 0")) << registry.Expose();
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
  EXPECT_TRUE(HasLine(registry.Expose(),
                      R"(smithy_http_requests_total{method="POST",operation="",status="413"} 2)"))
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
  EXPECT_TRUE(HasLine(
      exposition, R"(smithy_http_requests_total{method="unparsed",operation="",status="431"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition,
                      R"(smithy_http_requests_total{method="other",operation="",status="431"} 1)"))
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
      R"(smithy_http_request_duration_seconds_count{method="POST",operation="AddThing"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition,
      R"(smithy_http_request_duration_seconds_sum{method="POST",operation="AddThing"} 0.2)"))
      << exposition;
  // No latency series was minted for the rejections at all.
  EXPECT_EQ(
      exposition.find(R"(smithy_http_request_duration_seconds_count{method="POST",operation=""})"),
      std::string::npos)
      << exposition;
  EXPECT_TRUE(HasLine(exposition, "smithy_http_requests_in_flight 0")) << exposition;
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
  EXPECT_TRUE(HasLine(exposition,
                      R"(smithy_http_requests_total{method="PUT",operation="",status="413"} 1)"))
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
  EXPECT_TRUE(HasLine(exposition, "# TYPE smithy_http_requests_total counter")) << exposition;
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
      exposition, R"(smithy_metrics_observations_dropped_total{metric="user_events_total"} 46)"))
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
  EXPECT_TRUE(HasLine(exposition,
                      R"(smithy_metrics_observations_dropped_total{metric="user_events_total"} 8)"))
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
        MetricsRegistry(Enabled()).NewCounter("smithy_http_requests_total", "Shadows a built-in.");
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
  EXPECT_TRUE(HasLine(response.body, "# TYPE smithy_http_requests_total counter")) << response.body;
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

  EXPECT_TRUE(
      HasLine(exposition,
              R"(smithy_http_requests_total{method="GET",operation="GetThing",status="200"} 2)"))
      << exposition;
  // Nothing recorded for the scrape itself: no empty-operation series.
  EXPECT_EQ(exposition.find(R"(operation="",status="200")"), std::string::npos) << exposition;
}

TEST(MetricsEndpointTest, RecordMetricsCarriesTheOperationAndStatusFromTheResponse) {
  auto registry = std::make_shared<MetricsRegistry>(Enabled());
  http::RequestHandler handler =
      Chain({MetricsEndpoint(registry), RecordMetrics(registry)}, Handler(503, "GetThing"));

  handler(Get("/things"));
  EXPECT_TRUE(
      HasLine(handler(Get("/metrics")).body,
              R"(smithy_http_requests_total{method="GET",operation="GetThing",status="503"} 1)"));
}

TEST(MetricsEndpointTest, HealthProbesAreSeparableFromDispatchFailures) {
  // The reason HealthEndpoint labels its own path. Kubernetes polls a probe
  // every few seconds, so it is often the highest-volume "route" a service
  // has. Sharing the empty operation with 404s means the probe drowns the
  // signal in `smithy_http_requests_total{operation=""}` and the 404 rate
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
      exposition, R"(smithy_http_requests_total{method="GET",operation="/livez",status="200"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(
      exposition, R"(smithy_http_requests_total{method="GET",operation="/readyz",status="503"} 1)"))
      << exposition;
  // The 404 keeps the empty operation, and now means only that.
  EXPECT_TRUE(HasLine(exposition,
                      R"(smithy_http_requests_total{method="GET",operation="",status="404"} 1)"))
      << exposition;
  // Each probe has its own latency series, so `operation!~"/livez|/readyz"`
  // is expressible; before the label none of these three could be told apart.
  EXPECT_TRUE(HasLine(exposition, R"(smithy_http_request_duration_seconds_count{method="GET",)"
                                  R"(operation="/livez"} 1)"))
      << exposition;
  EXPECT_TRUE(HasLine(exposition, R"(smithy_http_request_duration_seconds_count{method="GET",)"
                                  R"(operation="/readyz"} 1)"))
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
  EXPECT_TRUE(
      HasLine(exposition,
              R"(smithy_http_requests_total{method="GET",operation="/metrics",status="200"} 1)"))
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
  EXPECT_TRUE(HasLine(exposition, "smithy_http_requests_in_flight 0")) << exposition;
  EXPECT_TRUE(HasLine(exposition,
                      R"(smithy_http_requests_total{method="GET",operation="",status="500"} 1)"))
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
      { MetricsRegistry().NewCounter("smithy_http_requests_total", "Shadows a built-in."); }, "");
  EXPECT_DEATH(
      {
        MetricsRegistry registry;
        registry.NewCounter("thing_total", "One help string.");
        registry.NewGauge("thing_total", "One help string.");
      },
      "");
  EXPECT_DEATH(
      {
        MetricsOptions options;
        options.method_label = "http-method";
        MetricsRegistry registry(options);
      },
      "");
  MetricsOptions descending;
  descending.latency_buckets = {1.0, 0.5};
  EXPECT_DEATH({ MetricsRegistry registry(descending); }, "");
}

// ---------------------------------------------------------------------------
// The aura/MoonBase dialect.
// ---------------------------------------------------------------------------

TEST(AuraCompatibilityTest, ExportsTheFiveSharedFamiliesUnderTheirPinnedNames) {
  // The names //domains/platform/libs/otel_contract pins across MoonBase's
  // three emitter rails, with the descriptions it pins with them: a
  // collector merging series by name keeps the first description it sees and
  // logs a conflict for every later one that disagrees.
  MetricsOptions options = MetricsOptions::Aura("todo-service");
  options.enabled = true;
  MetricsRegistry registry(options);

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
  // The smithy_ names are gone entirely, not emitted alongside: two families
  // for one measurement double-count anything that sums across them.
  EXPECT_EQ(exposition.find("smithy_http_"), std::string::npos) << exposition;
}

TEST(AuraCompatibilityTest, LabelsEverySeriesTheWayTheDashboardsSelect) {
  // prom_proxy selects `{service_name="x",route!="/health"}` on every query
  // it makes, so all three have to be present and spelled this way.
  MetricsOptions options = MetricsOptions::Aura("todo-service");
  options.enabled = true;
  MetricsRegistry registry(options);
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

TEST(AuraCompatibilityTest, SuccessAndFailureSplitAtFourHundred) {
  // ErrorRatePercent is failure/(success+failure), so the split has to land
  // where the rest of the fleet draws it: 2xx-3xx succeeded, 4xx and 5xx did
  // not. The three counters are views of one tally, so they cannot disagree.
  MetricsOptions options = MetricsOptions::Aura("todo-service");
  options.enabled = true;
  MetricsRegistry registry(options);
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

TEST(AuraCompatibilityTest, TheActiveGaugeIsKeyedByMethodAndNeverByRoute) {
  // It moves at request start, before dispatch, where no bounded route is
  // known. Every rail leaves the route off it for that reason, and
  // prom_proxy's `route!="/health"` matcher passes a series without the
  // label through untouched — which is why the same filter is safe on it.
  MetricsOptions options = MetricsOptions::Aura("todo-service");
  options.enabled = true;
  MetricsRegistry registry(options);
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

TEST(AuraCompatibilityTest, UsesTheRouteAndMethodSentinelsTheOtherRailsAgreedOn) {
  // Three constants that have to be byte-equal across the rails, because a
  // fleet-wide "unmatched traffic" query only means one thing if every
  // service spells it the same way.
  MetricsOptions options = MetricsOptions::Aura("todo-service");
  options.enabled = true;
  MetricsRegistry registry(options);
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

TEST(AuraCompatibilityTest, TheHealthProbeLandsOnTheRouteThePanelsSubtract) {
  // The composition that makes the /health literal real: prom_proxy
  // subtracts route!="/health" from every serving number and charts that
  // route on its own Probes tile, so a service that never reports it reads
  // as having no probe rather than as a healthy one. HealthEndpoint's
  // default path is already the literal.
  MetricsOptions options = MetricsOptions::Aura("todo-service");
  options.enabled = true;
  auto registry = std::make_shared<MetricsRegistry>(options);
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

TEST(AuraCompatibilityTest, UsesTheMicrosecondBucketLadderTheRailsShare) {
  // histogram_quantile reads `le` off bucket counts, so p95 only compares
  // like with like when the boundaries match. These are pinned equal across
  // the three rails by //domains/platform/libs/otel_contract; a service on a
  // different ladder charts a quantile computed against different bins than
  // everything beside it.
  MetricsOptions options = MetricsOptions::Aura("todo-service");
  options.enabled = true;
  MetricsRegistry registry(options);
  EXPECT_EQ(options.latency_buckets,
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

TEST(AuraCompatibilityTest, ApplicationMetricsStillShareTheScrape) {
  // Switching dialects changes the built-in vocabulary, not the endpoint: a
  // service's own numbers still ride the same target.
  MetricsOptions options = MetricsOptions::Aura("todo-service");
  options.enabled = true;
  MetricsRegistry registry(options);
  auto orders = registry.NewCounter("orders_processed_total", "Orders processed.");
  orders.Increment();

  const std::string exposition = registry.Expose();
  EXPECT_TRUE(HasLine(exposition, "orders_processed_total 1")) << exposition;
  EXPECT_TRUE(HasLine(exposition, "# TYPE http_server_requests_total counter")) << exposition;
}

TEST(AuraCompatibilityDeathTest, ShadowingARenamedBuiltInStillAborts) {
  // The reserved set follows the configured names rather than the defaults,
  // so the dialect cannot open a hole in the collision check.
  MetricsOptions options = MetricsOptions::Aura("todo-service");
  options.enabled = true;
  EXPECT_DEATH(
      {
        MetricsRegistry registry(options);
        registry.NewCounter("http_server_requests_success_total", "Shadows a built-in.");
      },
      "");
  // And the old default names are no longer reserved in this dialect, since
  // nothing emits them any more.
  MetricsRegistry registry(options);
  auto shadow = registry.NewCounter("smithy_http_requests_total", "Free in this dialect.");
  shadow.Increment();
  EXPECT_TRUE(HasLine(registry.Expose(), "smithy_http_requests_total 1")) << registry.Expose();
}

}  // namespace
}  // namespace smithy::server
