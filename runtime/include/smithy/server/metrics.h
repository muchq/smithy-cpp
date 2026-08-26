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
// The metric families, all prefixed `smithy_http_`:
//
//   requests_total{method,operation,status}    counter
//   request_duration_seconds{method,operation} histogram (+ _sum, _count)
//   requests_in_flight                         gauge
//
// Status is the exact code rather than a class: it is bounded either way,
// and `{status=~"5.."}` recovers the class at query time while the reverse
// direction loses information that matters at 3am.
//
// Application metrics join the same scrape through NewCounter / NewGauge /
// NewHistogram; see MetricsRegistry below.

// The bucket boundaries of `smithy_http_request_duration_seconds`, in
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
    family_->Add(labels, amount, /*set=*/false);
  }
  // Exports this series as 0 from startup; see the zero-baseline note on
  // MetricsRegistry. Idempotent, and harmless once events have arrived.
  void Declare(const MetricLabels& labels = {}) { family_->Declare(labels); }

 private:
  friend class MetricsRegistry;
  explicit Counter(std::shared_ptr<internal::MetricFamily> family) : family_(std::move(family)) {}
  std::shared_ptr<internal::MetricFamily> family_;
};

// A value that goes up and down.
class Gauge {
 public:
  void Set(double value) { Set(MetricLabels{}, value); }
  void Set(const MetricLabels& labels, double value) { family_->Add(labels, value, /*set=*/true); }
  void Increment(double amount = 1.0) { Increment(MetricLabels{}, amount); }
  void Increment(const MetricLabels& labels, double amount = 1.0) {
    family_->Add(labels, amount, /*set=*/false);
  }
  void Decrement(double amount = 1.0) { Increment(MetricLabels{}, -amount); }
  void Decrement(const MetricLabels& labels, double amount = 1.0) { Increment(labels, -amount); }
  // Exports this series as 0 from startup; see the zero-baseline note on
  // MetricsRegistry. Idempotent.
  void Declare(const MetricLabels& labels = {}) { family_->Declare(labels); }

 private:
  friend class MetricsRegistry;
  explicit Gauge(std::shared_ptr<internal::MetricFamily> family) : family_(std::move(family)) {}
  std::shared_ptr<internal::MetricFamily> family_;
};

// A distribution over configured buckets, exposed with _bucket/_sum/_count.
class Histogram {
 public:
  void Observe(double value) { Observe(MetricLabels{}, value); }
  void Observe(const MetricLabels& labels, double value) { family_->Observe(labels, value); }
  // Exports this series as an empty distribution — every bucket, `_sum` and
  // `_count` at 0 — from startup. Unlike a histogram behind a record-only
  // API, this is not an observation of 0: it adds nothing to `_sum` or
  // `_count`, so the windowed mean `rate(_sum)/rate(_count)` is unbiased.
  // Idempotent.
  void Declare(const MetricLabels& labels = {}) { family_->Declare(labels); }

 private:
  friend class MetricsRegistry;
  explicit Histogram(std::shared_ptr<internal::MetricFamily> family) : family_(std::move(family)) {}
  std::shared_ptr<internal::MetricFamily> family_;
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
//     `smithy_metrics_observations_dropped_total`. With the two rules above
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
  // max_series bounds the distinct {method,operation,status} and
  // {method,operation} combinations retained, and separately the series of
  // each application family; see the cardinality note above.
  explicit MetricsRegistry(std::size_t max_series = 4096,
                           std::vector<double> latency_buckets = DefaultLatencyBuckets());

  // Feed from Observe's on_complete: counts the request, files its latency,
  // and decrements the in-flight gauge. Safe from concurrent request threads.
  void Record(const RequestObservation& observation);

  // Feed from Observe's on_start: increments the in-flight gauge. Optional —
  // without it the gauge stays at zero and the other families are unaffected.
  void RecordStart(const RequestStart& start);

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
  std::string Expose() const;

 private:
  std::shared_ptr<internal::MetricFamily> Register(std::string name, std::string help,
                                                   internal::MetricFamily::Kind kind,
                                                   std::vector<double> buckets);

  struct CountKey {
    std::string method;
    std::string operation;
    int status = 0;

    friend bool operator<(const CountKey& a, const CountKey& b) {
      return std::tie(a.method, a.operation, a.status) < std::tie(b.method, b.operation, b.status);
    }
  };

  struct LatencyKey {
    std::string method;
    std::string operation;

    friend bool operator<(const LatencyKey& a, const LatencyKey& b) {
      return std::tie(a.method, a.operation) < std::tie(b.method, b.operation);
    }
  };

  // One histogram: per-bucket counts (parallel to buckets_, non-cumulative
  // here and accumulated at exposition time) plus the sum and count the
  // format also carries.
  struct HistogramData {
    std::vector<std::uint64_t> counts{};
    double sum_seconds = 0.0;
    std::uint64_t count = 0;
  };

  mutable std::mutex mutex_;
  std::size_t max_series_;
  std::vector<double> buckets_;
  std::map<CountKey, std::uint64_t> counts_;
  std::map<LatencyKey, HistogramData> latencies_;
  std::int64_t in_flight_ = 0;
  std::uint64_t observations_dropped_ = 0;
  // Sorted by name so each family's samples stay contiguous in the output.
  std::map<std::string, std::shared_ptr<internal::MetricFamily>> families_;
};

// The recording half: Observe wired to `registry`. Implemented in terms of
// Observe rather than beside it, so the request timing has exactly one
// implementation and cannot drift from what the logging hook reports. A null
// registry aborts at composition time (ADR-0009) — a metrics endpoint that
// silently reports nothing is worse than one that never starts.
Middleware RecordMetrics(std::shared_ptr<MetricsRegistry> registry);

// The serving half: answers GET or HEAD <path> (query string ignored) with
// the registry's exposition; every other request passes through to the next
// handler. A HEAD is answered like the GET, body included — the transport
// withholds the octets and keeps the length (RFC 9110 §9.3.2), which is the
// only question a HEAD asks. A null registry aborts at composition time.
//
// The endpoint is unauthenticated: it is middleware, so gate it the way you
// gate anything else — compose Guard or RequireBearerAuth outside it, or
// bind the scrape listener somewhere the internet cannot reach.
Middleware MetricsEndpoint(std::shared_ptr<MetricsRegistry> registry,
                           std::string path = "/metrics");

}  // namespace smithy::server

#endif  // SMITHY_SERVER_METRICS_H_
