#ifndef SMITHY_SERVER_METRICS_H_
#define SMITHY_SERVER_METRICS_H_

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "smithy/core/fatal.h"
#include "smithy/server/middleware.h"

namespace smithy::server {

// A dependency-free Prometheus backend for the server hooks (issue #91).
//
// The runtime bundles no telemetry SDK by design (docs/production-guide.md),
// but the Prometheus text exposition format needs no client library at all —
// it is a few lines of text over HTTP. So the turnkey path is two middleware
// composed around the generated handler: RecordMetrics feeds a registry from
// the same Observe hook everything else uses, and MetricsEndpoint serves what
// the registry holds.
//
//   auto metrics = std::make_shared<smithy::server::MetricsRegistry>();
//   transport.Start(smithy::server::Chain({MetricsEndpoint(metrics),
//                                          RecordMetrics(metrics)},
//                                         server.Handler()));
//
// Order matters, and this one is deliberate: the endpoint sits OUTSIDE the
// recorder, so scrapes answer without being counted as served traffic. Put
// RecordMetrics first instead and every scrape inflates your own request
// rate — at whatever interval Prometheus polls.
//
// The built-in families, under Prometheus's own conventional names — the
// library is not what is being measured, so it does not appear in them:
//
//   http_requests_total{method,operation,status}    counter
//   http_request_duration_seconds{method,operation} histogram (+ _sum/_count)
//   http_requests_in_flight                         gauge
//
// All of them are configurable; see MetricsOptions.
//
// Status is the exact code rather than a class: it is bounded either way,
// and `{status=~"5.."}` recovers the class at query time while the reverse
// direction loses information that matters at 3am.
//
// Application metrics join the same scrape through NewCounter / NewGauge /
// NewHistogram; see MetricsRegistry below.

// The bucket boundaries of `http_request_duration_seconds`, in
// seconds — Prometheus's own default ladder, which is tuned for exactly this
// shape of measurement (sub-millisecond to ten seconds).
inline const std::vector<double>& DefaultLatencyBuckets() {
  static const std::vector<double> kBuckets = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25,
                                               0.5,   1.0,  2.5,   5.0,  10.0};
  return kBuckets;
}

// The labels of one application-metric sample. Names are code constants and
// are validated (an invalid one aborts, ADR-0009 — it would otherwise emit a
// scrape Prometheus rejects wholesale); values are data and are escaped.
// Order does not matter: labels are sorted by name, so {a,b} and {b,a} are
// the same series rather than two.
using MetricLabels = std::vector<std::pair<std::string, std::string>>;

namespace internal {

// One application-metric family: its identity, and every sample under it
// keyed by rendered label text. Held by shared_ptr so a handle that outlives
// its registry updates a family nobody exposes rather than dangling.
struct MetricFamily {
  enum class Kind { kCounter, kGauge, kHistogram };

  struct Sample {
    // Counter/gauge value, or the histogram's running sum.
    double value = 0.0;
    // Histogram only: observation count and per-bucket counts, parallel to
    // `buckets` and accumulated into cumulative form at exposition time.
    std::uint64_t count = 0;
    std::vector<std::uint64_t> bucket_counts{};
  };

  std::string name;
  std::string help;
  Kind kind = Kind::kCounter;
  std::vector<double> buckets;
  std::size_t max_series = 0;

  mutable std::mutex mutex;
  std::map<std::string, Sample> samples;
  std::uint64_t dropped = 0;

  // `set` replaces the value (a gauge Set); otherwise it adds to it.
  void Add(const MetricLabels& labels, double amount, bool set);
  void Observe(const MetricLabels& labels, double value);
  // Materializes a series at its zero without recording an event.
  void Declare(const MetricLabels& labels);
};

}  // namespace internal

// A monotonically increasing count. Cheap to copy; every copy addresses the
// same family.
class Counter {
 public:
  void Increment(double amount = 1.0) { Increment(MetricLabels{}, amount); }
  void Increment(const MetricLabels& labels, double amount = 1.0) {
    // A handle from a disabled registry holds no family. The branch is what
    // makes an always-compiled call site free when metrics are off; the
    // argument is not, so guard a hot call site whose labels are themselves
    // expensive with MetricsRegistry::enabled().
    if (family_ == nullptr) {
      return;
    }
    family_->Add(labels, amount, /*set=*/false);
  }
  // Exports this series as 0 from startup; see the zero-baseline note on
  // MetricsRegistry. Idempotent, and harmless once events have arrived.
  void Declare(const MetricLabels& labels = {}) {
    if (family_ != nullptr) {
      family_->Declare(labels);
    }
  }

 private:
  friend class MetricsRegistry;
  explicit Counter(std::shared_ptr<internal::MetricFamily> family) : family_(std::move(family)) {}
  std::shared_ptr<internal::MetricFamily> family_;
};

// A value that goes up and down.
class Gauge {
 public:
  void Set(double value) { Set(MetricLabels{}, value); }
  void Set(const MetricLabels& labels, double value) {
    // Inert when the registry is disabled; see Counter::Increment.
    if (family_ != nullptr) {
      family_->Add(labels, value, /*set=*/true);
    }
  }
  void Increment(double amount = 1.0) { Increment(MetricLabels{}, amount); }
  void Increment(const MetricLabels& labels, double amount = 1.0) {
    if (family_ != nullptr) {
      family_->Add(labels, amount, /*set=*/false);
    }
  }
  void Decrement(double amount = 1.0) { Increment(MetricLabels{}, -amount); }
  void Decrement(const MetricLabels& labels, double amount = 1.0) { Increment(labels, -amount); }
  // Exports this series as 0 from startup; see the zero-baseline note on
  // MetricsRegistry. Idempotent.
  void Declare(const MetricLabels& labels = {}) {
    if (family_ != nullptr) {
      family_->Declare(labels);
    }
  }

 private:
  friend class MetricsRegistry;
  explicit Gauge(std::shared_ptr<internal::MetricFamily> family) : family_(std::move(family)) {}
  std::shared_ptr<internal::MetricFamily> family_;
};

// A distribution over configured buckets, exposed with _bucket/_sum/_count.
class Histogram {
 public:
  void Observe(double value) { Observe(MetricLabels{}, value); }
  void Observe(const MetricLabels& labels, double value) {
    // Inert when the registry is disabled; see Counter::Increment.
    if (family_ != nullptr) {
      family_->Observe(labels, value);
    }
  }
  // Exports this series as an empty distribution — every bucket, `_sum` and
  // `_count` at 0 — from startup. Unlike a histogram behind a record-only
  // API, this is not an observation of 0: it adds nothing to `_sum` or
  // `_count`, so the windowed mean `rate(_sum)/rate(_count)` is unbiased.
  // Idempotent.
  void Declare(const MetricLabels& labels = {}) {
    if (family_ != nullptr) {
      family_->Declare(labels);
    }
  }

 private:
  friend class MetricsRegistry;
  explicit Histogram(std::shared_ptr<internal::MetricFamily> family) : family_(std::move(family)) {}
  std::shared_ptr<internal::MetricFamily> family_;
};

// Which unit the built-in latency histogram records in. Seconds is
// Prometheus's own base unit and the default; microseconds exists because a
// fleet that already has microsecond dashboards cannot read seconds without
// rewriting every query it has.
enum class LatencyUnit { kSeconds, kMicroseconds };

// The microsecond bucket ladder MoonBase's three emitter rails share
// (MoonBase #1286, pinned equal across them by
// //domains/platform/libs/otel_contract). Bucket layouts only compare like
// with like: `histogram_quantile` reads `le` off bucket counts, so a service
// joining an existing dashboard has to land on the same boundaries or its
// quantiles are computed against a different ladder than everything beside
// it. Use it with LatencyUnit::kMicroseconds.
inline const std::vector<double>& AuraLatencyBuckets() {
  static const std::vector<double> kBuckets = {100,    250,    500,     1000,    2500,
                                               5000,   10000,  25000,   50000,   100000,
                                               250000, 500000, 1000000, 2500000, 10000000};
  return kBuckets;
}

// How a registry behaves and what its built-in families are called.
//
// `enabled` is false, so a registry costs nothing until something turns it
// on. Disabled, RecordMetrics and MetricsEndpoint compose to the identity —
// not a wrapper that checks a flag, but no wrapper at all, so a served
// request runs the same call chain it would if metrics had never been
// written. Registration still validates: a bad metric name or a type
// collision aborts whether or not the registry is enabled, so switching it
// on in production is never the first time those checks run.
//
// The names and labels are configurable because an exposition format is a
// contract with whatever is already scraping. The defaults are this
// library's own; MetricsOptions::Aura() is the vocabulary MoonBase's
// dashboards read. Everything is individually overridable for a fleet that
// speaks neither.
struct MetricsOptions {
  // Nothing is recorded, exposed, or composed until this is true.
  bool enabled = false;

  // Bounds the distinct {method,route,status} and {method,route}
  // combinations retained, and separately the series of each application
  // family; see the cardinality note on MetricsRegistry.
  std::size_t max_series = 4096;

  // Family names. An empty success/failure name means that family is not
  // emitted at all — the default, since the status label already carries the
  // outcome and `{status=~"5.."}` recovers it at query time.
  std::string requests_total_name = "http_requests_total";
  std::string requests_success_name{};
  std::string requests_failure_name{};
  std::string request_duration_name = "http_request_duration_seconds";
  std::string requests_in_flight_name = "http_requests_in_flight";
  // The registry's own health: observations refused after a family hit the
  // series cap. Alert on it being non-zero rather than discovering the cap
  // as an OOM.
  std::string observations_dropped_name = "metrics_observations_dropped_total";

  // HELP text. Part of the contract when these names are shared with another
  // emitter: a collector merging series by name keeps the first description
  // it sees and logs a conflict for every later one that disagrees.
  std::string requests_total_help =
      "Total HTTP requests served, by method, Smithy operation, and status code.";
  std::string requests_success_help = "HTTP requests completed successfully (2xx-3xx)";
  std::string requests_failure_help = "HTTP requests that returned 4xx or 5xx";
  std::string request_duration_help = "Request latency in seconds, by method and Smithy operation.";
  std::string requests_in_flight_help = "Requests currently being served.";
  std::string observations_dropped_help =
      "Observations dropped after the registry hit its series cap.";

  // Label names. An empty status_label drops that label, which aggregates
  // the counter over status codes — the shape to use when success and
  // failure counters carry the outcome instead.
  std::string method_label = "method";
  std::string route_label = "operation";
  std::string status_label = "status";

  // Labels added to every built-in series, for a scrape that has to identify
  // the service in the metric itself rather than in the scrape target — a
  // dashboard selecting `{service_name="..."}` across a fleet, say.
  MetricLabels constant_labels{};

  // The in-flight gauge carries no route on purpose: it moves at request
  // start, before dispatch, where nothing bounded is known about the path.
  // It can still be labeled by method, which is known that early.
  bool in_flight_by_method = false;

  // The vocabulary for values the request itself did not supply. A request
  // that reached no operation reports `unrouted_route`; a method outside the
  // nine RFC 9110 verbs reports `nonstandard_method`; a request rejected
  // before its method was parsed reports `unparsed_method`. All three are
  // constants, which is what keeps the label set bounded.
  std::string unrouted_route{};
  std::string nonstandard_method = "other";
  std::string unparsed_method = "unparsed";

  LatencyUnit latency_unit = LatencyUnit::kSeconds;
  std::vector<double> latency_buckets = DefaultLatencyBuckets();

  // The exposition MoonBase's prom_proxy dashboards already query, so a
  // smithy-cpp service can replace an aura/futility, yodel, or server_pal
  // one without touching a dashboard: the five http_server_* families, the
  // service_name/http_method/route label set, the route vocabulary
  // ("unmatched" for unrouted, "/health" for the probe — which
  // HealthEndpoint's default path already produces), the CUSTOM and
  // (unparsed) method sentinels, and the shared microsecond bucket ladder.
  //
  // Still off unless you also set `enabled`.
  //
  // Compose HealthEndpoint() inside RecordMetrics for the probe route to
  // exist at all: prom_proxy subtracts `route!="/health"` from every serving
  // number and charts that route on its own tile, so a service that does not
  // report it reads as having no probe rather than as a healthy one.
  static MetricsOptions Aura(std::string service_name);
};

// A thread-safe aggregate of served requests, exposable as Prometheus text.
//
// Cardinality is the failure mode a metrics endpoint actually dies of, so
// the label set is chosen to be bounded by construction rather than by
// convention:
//
//   - `target` is never a label. It carries path parameters and query
//     strings, so one series per distinct URL is one series per request id.
//     `operation` is the bounded stand-in — the generated router stamps it
//     from the model, and it is empty for the 404/405/400 dispatch failures
//     that never reached an operation.
//   - `method` arrives from the wire, so it is whatever a client typed.
//     Anything outside the standard set collapses to "other" rather than
//     minting a series per invented verb.
//   - Past `max_series` distinct label combinations the registry stops
//     minting new ones and counts each refused observation once in
//     `metrics_observations_dropped_total`. With the two rules above
//     the cap should be unreachable; it is the backstop for a handler that
//     stamps its own unbounded operation, and it fails visibly (a counter
//     you can alert on) rather than by exhausting memory.
//
// Application metrics share the same scrape and the same protections. Mint a
// family once, keep the handle, and use it from anywhere:
//
//   auto orders = metrics->NewCounter("orders_processed_total",
//                                     "Orders processed.");
//   orders.Increment({{"region", "us-east"}});
//
// The cap applies per family, so a label chosen from unbounded data (a user
// id, an order id) costs that family its own series budget and shows up in
// the dropped counter — it cannot take the process down with it.
//
// Declare the series whose labels are known at startup:
//
//   orders.Declare({{"region", "us-east"}});
//
// A series that has never been touched does not exist in the scrape, and a
// counter that springs into existence already carrying its first event's
// value hides that event forever: `increase()` and `rate()` measure the
// change *between* samples, so with nothing earlier to measure from, the
// first one shows no increase at all. The panel reads zero, which is worse
// than a missing tile because it looks like an answer. Declaring exports the
// series as 0 from startup so the first real event is a visible step.
//
// Declare the label sets that are known up front — outcomes, error kinds,
// regions. A label carrying request data has no series to declare (and is
// the cardinality problem above); bound it to a known kind and declare that.
// The built-in unlabeled families are always exported, so they are already
// baselined; the per-operation ones cannot be, since this registry never
// sees the model.
class MetricsRegistry {
 public:
  // Disabled unless `options.enabled`; see MetricsOptions.
  explicit MetricsRegistry(MetricsOptions options = {});

  // Whether this registry records anything. Worth branching on only around a
  // call site whose label arguments are themselves expensive to build — the
  // handles are already inert, and the built-in middleware compose away.
  bool enabled() const { return options_.enabled; }

  // Feed from Observe's on_complete: counts the request, files its latency,
  // and decrements the in-flight gauge. Safe from concurrent request threads.
  void Record(const RequestObservation& observation);

  // Feed from Observe's on_start: increments the in-flight gauge. Optional —
  // without it the gauge stays at zero and the other families are unaffected.
  void RecordStart(const RequestStart& start);

  // Counts a request the transport rejected before any handler chain ran —
  // the 413/431 an over-limit body or header set earns while the parser is
  // still reading. RecordMetrics cannot see these: it is middleware, and the
  // transport answers these before middleware exists, so without this an
  // over-limit flood is invisible in the request counters entirely.
  //
  // It counts and nothing more. The in-flight gauge never moves, because
  // such a request was never in flight through a handler; and no latency is
  // filed, because a request refused at parse time has no service latency to
  // report. Filing it as a zero observation would be worse than filing
  // nothing: a flood of them drags `rate(_sum)/rate(_count)` toward zero, so
  // the latency panel would look its best exactly while the service is being
  // hammered.
  //
  // `method` is whatever the parser had reached — normalized like every
  // other method label, and empty becomes "unparsed" rather than "other",
  // since a 431 can fire mid-headers and "never parsed" is a different
  // diagnosis from "invented verb". The rejected target is deliberately
  // dropped: a flood against distinct paths must not mint a series each.
  void RecordRejection(std::string_view method, int status);

  // Mints an application metric family. The name must be a valid Prometheus
  // metric name, and must not collide with a family already registered (the
  // built-ins included) under a different type or help text — both abort at
  // registration (ADR-0009), because either produces a scrape Prometheus
  // rejects in full, and a metrics endpoint has no in-process consumer to
  // notice. Re-minting the same name with the same type and help returns a
  // handle to the same family, so a helper can hand one out repeatedly
  // without callers coordinating.
  Counter NewCounter(std::string name, std::string help);
  Gauge NewGauge(std::string name, std::string help);
  Histogram NewHistogram(std::string name, std::string help,
                         std::vector<double> buckets = DefaultLatencyBuckets());

  // The Prometheus text exposition format (version 0.0.4), ready to serve.
  // Empty when the registry is disabled.
  std::string Expose() const;

 private:
  std::shared_ptr<internal::MetricFamily> Register(std::string name, std::string help,
                                                   internal::MetricFamily::Kind kind,
                                                   std::vector<double> buckets);

  struct CountKey {
    std::string method;
    std::string route;
    int status = 0;

    friend bool operator<(const CountKey& a, const CountKey& b) {
      return std::tie(a.method, a.route, a.status) < std::tie(b.method, b.route, b.status);
    }
  };

  struct LatencyKey {
    std::string method;
    std::string route;

    friend bool operator<(const LatencyKey& a, const LatencyKey& b) {
      return std::tie(a.method, a.route) < std::tie(b.method, b.route);
    }
  };

  // One histogram: per-bucket counts (parallel to buckets_, non-cumulative
  // here and accumulated at exposition time) plus the sum and count the
  // format also carries.
  struct HistogramData {
    std::vector<std::uint64_t> counts{};
    // In whichever unit options_.latency_unit names.
    double sum = 0.0;
    std::uint64_t count = 0;
  };

  // Renders `{a="1",b="2"}` from the constant labels plus what is passed,
  // dropping any pair whose name is empty. Callers hold mutex_.
  std::string BuiltInLabels(const MetricLabels& labels) const;

  MetricsOptions options_;
  mutable std::mutex mutex_;
  std::map<CountKey, std::uint64_t> counts_;
  std::map<LatencyKey, HistogramData> latencies_;
  // Always keyed by method, whether or not the gauge is exposed that way:
  // the unlabeled form is the sum, and the key set is bounded by the method
  // vocabulary.
  std::map<std::string, std::int64_t> in_flight_;
  std::uint64_t observations_dropped_ = 0;
  // Sorted by name so each family's samples stay contiguous in the output.
  std::map<std::string, std::shared_ptr<internal::MetricFamily>> families_;
};

// The recording half: Observe wired to `registry`. Implemented in terms of
// Observe rather than beside it, so the request timing has exactly one
// implementation and cannot drift from what the logging hook reports. A null
// registry aborts at composition time (ADR-0009) — a metrics endpoint that
// silently reports nothing is worse than one that never starts.
//
// A disabled registry composes to the identity: the returned middleware
// hands back the handler it was given, so nothing wraps the request path and
// a served request pays nothing at all — no timing, no lock, not even an
// extra call frame.
Middleware RecordMetrics(std::shared_ptr<MetricsRegistry> registry);

// A ready-made sink for `BeastServerTransport::Options::on_rejected`:
//
//   options.on_rejected = smithy::server::RecordRejections(metrics);
//
// Generic in the rejection type so this header — and `:server` with it —
// keeps no dependency on the Beast transport that defines it. A null
// registry aborts (ADR-0009).
inline auto RecordRejections(std::shared_ptr<MetricsRegistry> registry) {
  if (registry == nullptr) {
    smithy::internal::Fatal("smithy::server::RecordRejections: registry may not be null");
  }
  return [registry = std::move(registry)](const auto& rejected) {
    registry->RecordRejection(rejected.method, rejected.status);
  };
}

// The serving half: answers GET or HEAD <path> (query string ignored) with
// the registry's exposition; every other request passes through to the next
// handler. A HEAD is answered like the GET, body included — the transport
// withholds the octets and keeps the length (RFC 9110 §9.3.2), which is the
// only question a HEAD asks. A null registry aborts at composition time.
//
// The response carries <path> as its HttpResponse::operation, which the
// documented composition never reads — it matters only if you deliberately
// put the endpoint inside the recorder to measure scrape volume.
//
// A disabled registry composes to the identity, so <path> is not served at
// all — it reaches the router like any other unmodeled path, and answers
// whatever that answers (a 404). A disabled endpoint that returned an empty
// 200 would read to Prometheus as a live target reporting no series, which
// is indistinguishable from a service whose metrics have all gone quiet.
//
// The endpoint is unauthenticated: it is middleware, so gate it the way you
// gate anything else — compose Guard or RequireBearerAuth outside it, or
// bind the scrape listener somewhere the internet cannot reach.
Middleware MetricsEndpoint(std::shared_ptr<MetricsRegistry> registry,
                           std::string path = "/metrics");

}  // namespace smithy::server

#endif  // SMITHY_SERVER_METRICS_H_
