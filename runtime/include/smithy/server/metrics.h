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

// The bucket boundaries of `smithy_http_request_duration_seconds`, in
// seconds — Prometheus's own default ladder, which is tuned for exactly this
// shape of measurement (sub-millisecond to ten seconds).
inline const std::vector<double>& DefaultLatencyBuckets() {
  static const std::vector<double> kBuckets = {0.005, 0.01, 0.025, 0.05, 0.1, 0.25,
                                               0.5,   1.0,  2.5,   5.0,  10.0};
  return kBuckets;
}

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
//     `smithy_metrics_observations_dropped_total`. With the two rules above the
//     cap should be unreachable; it is the backstop for a handler that
//     stamps its own unbounded operation, and it fails visibly (a counter
//     you can alert on) rather than by exhausting memory.
class MetricsRegistry {
 public:
  // max_series bounds the distinct {method,operation,status} and
  // {method,operation} combinations retained; see the cardinality note above.
  explicit MetricsRegistry(std::size_t max_series = 4096,
                           std::vector<double> latency_buckets = DefaultLatencyBuckets());

  // Feed from Observe's on_complete: counts the request, files its latency,
  // and decrements the in-flight gauge. Safe from concurrent request threads.
  void Record(const RequestObservation& observation);

  // Feed from Observe's on_start: increments the in-flight gauge. Optional —
  // without it the gauge stays at zero and the other families are unaffected.
  void RecordStart(const RequestStart& start);

  // The Prometheus text exposition format (version 0.0.4), ready to serve.
  std::string Expose() const;

 private:
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
  struct Histogram {
    std::vector<std::uint64_t> counts;
    double sum_seconds = 0.0;
    std::uint64_t count = 0;
  };

  mutable std::mutex mutex_;
  std::size_t max_series_;
  std::vector<double> buckets_;
  std::map<CountKey, std::uint64_t> counts_;
  std::map<LatencyKey, Histogram> latencies_;
  std::int64_t in_flight_ = 0;
  std::uint64_t observations_dropped_ = 0;
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
