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
//   auto metrics = std::make_shared<smithy::server::MetricsRegistry>(
//       smithy::server::MetricsOptions{.enabled = true,
//                                      .service_name = "todo-service"});
//   transport.Start(smithy::server::Chain({MetricsEndpoint(metrics),
//                                          RecordMetrics(metrics),
//                                          HealthEndpoint()},
//                                         server.Handler()));
//
// Order matters, and this one is deliberate: the endpoint sits OUTSIDE the
// recorder, so scrapes answer without being counted as served traffic. Put
// RecordMetrics first instead and every scrape inflates your own request
// rate — at whatever interval Prometheus polls.
//
// The five built-in families, labeled by service_name, http_method and route:
//
//   http_server_requests_total                counter
//   http_server_requests_success_total        counter  (status < 400)
//   http_server_requests_failure_total        counter  (status >= 400)
//   http_server_requests_active_gauge         gauge    (no route; see below)
//   http_server_request_duration_microseconds histogram (+ _sum/_count)
//
// plus `metrics_observations_dropped_total`, the registry's own health.
//
// This is not a vocabulary of our own invention. It is MoonBase's shared HTTP
// serving contract, spoken identically by its Java (yodel), Rust
// (server_pal), and C++ (futility/otel, behind aura) emitters and pinned
// across them by //domains/platform/libs/otel_contract — names, descriptions,
// label sets, route sentinels, and bucket boundaries alike. Those services
// are who scrapes this, and their dashboards (prom_proxy) query these names
// with these labels. A service here that invented its own spelling would
// simply not appear on them.
//
// So the exposition is deliberately NOT configurable. A knob here is a way
// for one service to drift off the contract, and the failure is silent: the
// dashboard renders an empty panel, which looks like a quiet service rather
// than a misconfigured one. If a second fleet ever needs a different dialect,
// that is the point to design one — not before.
//
// Consequences of the contract worth knowing before reading the code:
//
//   - The status code is not a label. The outcome rides on the success and
//     failure counters, which are two views of the same tally the total sums
//     — so the three can never disagree, and no series is multiplied by the
//     codes a service happens to return.
//   - The active gauge carries no route. It moves at request start, before
//     dispatch, where nothing bounded is known about the path; every rail
//     leaves the route off it for that reason, and prom_proxy's negative
//     `route!="/health"` matcher passes a series without the label through
//     untouched, which is what makes the same filter safe on it.
//   - Durations are microseconds, on the ladder the rails pin equal.
//     `histogram_quantile` reads `le` off bucket counts, so a service on a
//     different ladder charts a quantile computed against different bins
//     than everything beside it.
//
// Application metrics join the same scrape through NewCounter / NewGauge /
// NewHistogram; see MetricsRegistry below.

// The HTTP latency bucket boundaries, in microseconds (MoonBase #1286,
// pinned equal across its three emitter rails by
// //domains/platform/libs/otel_contract). Exported because an application
// histogram that measures a request-shaped duration should land on the same
// ladder; one measuring anything else should pass its own.
inline const std::vector<double>& HttpLatencyBuckets() {
  static const std::vector<double> kBuckets = {100,    250,    500,     1000,    2500,
                                               5000,   10000,  25000,   50000,   100000,
                                               250000, 500000, 1000000, 2500000, 10000000};
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

// How a registry behaves. Everything about *what* it exposes is fixed by the
// contract described at the top of this header; what is left is whether it
// runs at all, who it says it is, and the cardinality backstop.
//
// `enabled` is false, so a registry costs nothing until something turns it
// on. Disabled, RecordMetrics and MetricsEndpoint compose to the identity —
// not a wrapper that checks a flag, but no wrapper at all, so a served
// request runs the same call chain it would if metrics had never been
// written. Registration still validates: a bad metric name or a type
// collision aborts whether or not the registry is enabled, so switching it
// on in production is never the first time those checks run.
struct MetricsOptions {
  // Nothing is recorded, exposed, or composed until this is true.
  bool enabled = false;

  // The `service_name` label on every built-in series. Required when
  // enabled, and an empty one aborts (ADR-0009): every dashboard query
  // selects on it, so a service reporting the empty string is scraped,
  // stored, and invisible — the failure mode that looks like success.
  std::string service_name{};

  // Bounds the distinct {method,route,status} and {method,route}
  // combinations retained, and separately the series of each application
  // family; see the cardinality note on MetricsRegistry.
  std::size_t max_series = 4096;
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
  // `buckets` has no default on purpose: the built-in ladder is in
  // microseconds and shaped for request latency, and silently inheriting it
  // for a histogram of bytes or queue depth would produce a chart whose bins
  // mean nothing. Pass HttpLatencyBuckets() for a request-shaped duration.
  Histogram NewHistogram(std::string name, std::string help, std::vector<double> buckets);

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
