#include "smithy/server/metrics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <utility>

#include "smithy/core/fatal.h"

namespace smithy::server {
namespace {

// The exposition format's own escaping for label values: backslash, double
// quote, and newline (docs: "Prometheus text format", label_value). Applied
// to every label even where the value is already bounded — a handler that
// stamps its own operation reaches this too, and a stray quote there would
// otherwise produce a scrape the server cannot parse.
std::string EscapeLabel(std::string_view value) {
  std::string escaped;
  escaped.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '"':
        escaped += "\\\"";
        break;
      case '\n':
        escaped += "\\n";
        break;
      default:
        escaped += c;
    }
  }
  return escaped;
}

// The request method is whatever the client typed, so it cannot be a label
// as-is: `curl -X <random>` in a loop would mint a series per invented verb
// until the process runs out of memory. The standard set passes through and
// everything else shares one bucket. Case-sensitive, because HTTP methods
// are (RFC 9110 §9.1) — "get" is not GET, and folding it in would report
// traffic the server actually rejected as if it had been served.
// The contract, transcribed. Every literal below is pinned on the MoonBase
// side by //domains/platform/libs/otel_contract; treat this block as data
// copied from there rather than as names chosen here.
constexpr std::string_view kRequestsTotal = "http_server_requests_total";
constexpr std::string_view kRequestsSuccess = "http_server_requests_success_total";
constexpr std::string_view kRequestsFailure = "http_server_requests_failure_total";
constexpr std::string_view kRequestsActive = "http_server_requests_active_gauge";
constexpr std::string_view kRequestDuration = "http_server_request_duration_microseconds";
constexpr std::string_view kObservationsDropped = "metrics_observations_dropped_total";

constexpr std::string_view kRequestsTotalHelp = "HTTP requests received";
constexpr std::string_view kRequestsSuccessHelp = "HTTP requests completed successfully (2xx-3xx)";
constexpr std::string_view kRequestsFailureHelp = "HTTP requests that returned 4xx or 5xx";
constexpr std::string_view kRequestsActiveHelp = "HTTP requests currently in flight";
constexpr std::string_view kRequestDurationHelp = "HTTP request duration in microseconds";
constexpr std::string_view kObservationsDroppedHelp =
    "Observations dropped after the registry hit its series cap.";

constexpr std::string_view kServiceLabel = "service_name";
constexpr std::string_view kMethodLabel = "http_method";
constexpr std::string_view kRouteLabel = "route";

// A request that reached no operation. Never the empty string: prom_proxy
// subtracts `route!="/health"` from every serving number, and that matcher
// matches the empty string too — unrouted traffic would silently join the
// serving figures instead of being visible as its own thing.
constexpr std::string_view kUnmatchedRoute = "unmatched";
// A method outside the nine RFC 9110 verbs, and one the transport rejected
// before a method token existed at all (a 431 can fire mid-headers). Kept
// distinct because "never parsed" and "client invented a verb" are different
// diagnoses.
constexpr std::string_view kCustomMethod = "CUSTOM";
constexpr std::string_view kUnparsedMethod = "(unparsed)";

std::string NormalizeMethod(std::string_view method) {
  static constexpr std::array<std::string_view, 9> kKnown = {
      "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "TRACE", "CONNECT"};
  const auto* found = std::ranges::find(kKnown, method);
  return std::string(found == kKnown.end() ? kCustomMethod : *found);
}

// Prometheus numbers: plain decimal, no trailing zero noise. Six decimals is
// exactly the input granularity (RequestObservation::duration is
// microseconds), so nothing is rounded away that was ever measured.
std::string FormatNumber(double value) {
  if (std::isinf(value)) {
    return value > 0 ? "+Inf" : "-Inf";
  }
  std::array<char, 64> buffer{};
  const int written = std::snprintf(buffer.data(), buffer.size(), "%.6f", value);
  if (written <= 0) {
    return "0";
  }
  std::string text(buffer.data(), static_cast<std::size_t>(written));
  if (text.find('.') != std::string::npos) {
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  return text.empty() ? "0" : text;
}

// Prometheus metric names are [a-zA-Z_:][a-zA-Z0-9_:]*, label names the same
// without the colon. Both are code constants here, so an invalid one is a
// programming error caught on the first run rather than data to sanitize —
// and letting one through corrupts the whole scrape, not just its own line.
bool ValidName(std::string_view name, bool allow_colon) {
  if (name.empty()) return false;
  const auto valid = [allow_colon](char c, bool first) {
    if (c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return true;
    if (allow_colon && c == ':') return true;
    return !first && c >= '0' && c <= '9';
  };
  if (!valid(name.front(), true)) return false;
  return std::ranges::all_of(name.substr(1), [&](char c) { return valid(c, false); });
}

// Renders a label set into the inner text of `{...}`, sorted by name so the
// same labels in a different order address the same series instead of
// silently minting a second one.
std::string RenderLabels(const MetricLabels& labels) {
  std::vector<std::pair<std::string, std::string>> sorted(labels.begin(), labels.end());
  std::ranges::sort(sorted, [](const auto& a, const auto& b) { return a.first < b.first; });
  std::string out;
  for (const auto& [name, value] : sorted) {
    if (!ValidName(name, /*allow_colon=*/false)) {
      smithy::internal::Fatal("smithy::server::MetricsRegistry: invalid label name '" + name + "'");
    }
    if (!out.empty()) out += ',';
    out += name;
    out += "=\"";
    out += EscapeLabel(value);
    out += '"';
  }
  return out;
}

// Appends `<name><suffix>{labels} <value>` and a newline, omitting the braces
// when the sample carries no labels.
void AppendSample(std::string& out, std::string_view name, std::string_view suffix,
                  std::string_view labels, std::string_view value) {
  out += name;
  out += suffix;
  if (!labels.empty()) {
    out += '{';
    out += labels;
    out += '}';
  }
  out += ' ';
  out += value;
  out += '\n';
}

// HELP runs to the end of the line, so a backslash or a newline in it
// corrupts the scrape the same way an unescaped label value would — and for
// an application family the help text is a caller's string. The format
// escapes only these two here; a quote needs no escaping in HELP, unlike in
// a label value.
std::string EscapeHelp(std::string_view help) {
  std::string escaped;
  escaped.reserve(help.size());
  for (const char c : help) {
    switch (c) {
      case '\\':
        escaped += "\\\\";
        break;
      case '\n':
        escaped += "\\n";
        break;
      default:
        escaped += c;
    }
  }
  return escaped;
}

void AppendFamilyHeader(std::string& out, std::string_view name, std::string_view type,
                        std::string_view help) {
  out += "# HELP ";
  out += name;
  out += ' ';
  out += EscapeHelp(help);
  out += "\n# TYPE ";
  out += name;
  out += ' ';
  out += type;
  out += '\n';
}

}  // namespace

namespace internal {

void MetricFamily::Add(const MetricLabels& labels, double amount, bool set) {
  const std::string key = RenderLabels(labels);
  const std::lock_guard<std::mutex> lock(mutex);
  if (auto found = samples.find(key); found != samples.end()) {
    if (set) {
      found->second.value = amount;
    } else {
      found->second.value += amount;
    }
    return;
  }
  if (samples.size() >= max_series) {
    ++dropped;
    return;
  }
  samples.emplace(key, Sample{.value = amount});
}

void MetricFamily::Observe(const MetricLabels& labels, double value) {
  const std::string key = RenderLabels(labels);
  const std::lock_guard<std::mutex> lock(mutex);
  auto found = samples.find(key);
  if (found == samples.end()) {
    if (samples.size() >= max_series) {
      ++dropped;
      return;
    }
    found =
        samples.emplace(key, Sample{.bucket_counts = std::vector<std::uint64_t>(buckets.size(), 0)})
            .first;
  }
  Sample& sample = found->second;
  sample.value += value;
  ++sample.count;
  const auto bucket = std::ranges::lower_bound(buckets, value);
  if (bucket != buckets.end()) {
    ++sample.bucket_counts[static_cast<std::size_t>(bucket - buckets.begin())];
  }
}

void MetricFamily::Declare(const MetricLabels& labels) {
  const std::string key = RenderLabels(labels);
  const std::lock_guard<std::mutex> lock(mutex);
  if (samples.contains(key)) {
    return;  // idempotent, and never disturbs a series already carrying events
  }
  if (samples.size() >= max_series) {
    ++dropped;
    return;
  }
  // A counter or gauge baselines at 0; a histogram baselines as an empty
  // distribution, which costs `_sum` and `_count` nothing — the mean stays
  // unbiased, unlike recording a literal 0 observation.
  samples.emplace(key, Sample{.bucket_counts = std::vector<std::uint64_t>(buckets.size(), 0)});
}

}  // namespace internal

MetricsRegistry::MetricsRegistry(MetricsOptions options) : options_(std::move(options)) {
  // Composition-time validation (ADR-0009). An empty service_name is scraped
  // and stored exactly like a good one — every dashboard query selects on the
  // label, so the service is simply absent from all of them. That is the
  // failure mode worth aborting for: it looks like success everywhere except
  // the panel nobody is watching yet.
  if (options_.enabled && options_.service_name.empty()) {
    smithy::internal::Fatal(
        "smithy::server::MetricsRegistry: service_name is required when metrics are enabled");
  }
}

std::string MetricsRegistry::BuiltInLabels(const MetricLabels& labels) const {
  // Not RenderLabels: these are emitted in a fixed order (service_name, then
  // method, route) rather than sorted, so the built-in families read the way
  // the header documents them. Prometheus does not care about label order; a
  // human reading a scrape does.
  std::string out;
  out += kServiceLabel;
  out += "=\"";
  out += EscapeLabel(options_.service_name);
  out += '"';
  for (const auto& [name, value] : labels) {
    out += ',';
    out += name;
    out += "=\"";
    out += EscapeLabel(value);
    out += '"';
  }
  return out;
}

void MetricsRegistry::RecordStart(const RequestStart& start) {
  if (!options_.enabled) {
    return;
  }
  // The target is deliberately not read: this runs before dispatch, so the
  // only bounded thing known about the request is its method. The gauge is
  // always keyed by method — the unlabeled form is the sum over these keys —
  // and the key set is bounded by the method vocabulary.
  std::string method = NormalizeMethod(start.method);
  const std::lock_guard<std::mutex> lock(mutex_);
  ++in_flight_[std::move(method)];
}

MetricsRegistry::RouteStats* MetricsRegistry::AdmitRoute(const RouteKey& key) {
  if (auto found = routes_.find(key); found != routes_.end()) {
    return &found->second;
  }
  if (routes_.size() >= options_.max_series) {
    // The backstop for an unbounded route stamped by a hand-written handler.
    // Counted once per refused observation, not once per family: the counter
    // answers "how much traffic am I blind to", and one event is one gap.
    ++observations_dropped_;
    return nullptr;
  }
  return &routes_
              .emplace(key, RouteStats{.buckets = std::vector<std::uint64_t>(
                                           HttpLatencyBuckets().size(), 0)})
              .first->second;
}

void MetricsRegistry::Record(const RequestObservation& observation) {
  if (!options_.enabled) {
    return;
  }
  // The hook is already microseconds, which is the exposition's unit too, so
  // nothing is converted and nothing is rounded away.
  const auto duration = static_cast<double>(observation.duration.count());
  const RouteKey key{.method = NormalizeMethod(observation.method),
                     .route = observation.operation.empty() ? std::string(kUnmatchedRoute)
                                                            : observation.operation};

  const std::lock_guard<std::mutex> lock(mutex_);
  // Before the cap check: a request that started must bring the gauge back
  // down whether or not its route survives admission, or a refused route
  // leaks in-flight forever. Only decrement one that was incremented —
  // without RecordStart wired up the gauge stays at zero rather than
  // counting downward.
  if (auto in_flight = in_flight_.find(key.method);
      in_flight != in_flight_.end() && in_flight->second > 0) {
    --in_flight->second;
  }

  RouteStats* stats = AdmitRoute(key);
  if (stats == nullptr) {
    return;
  }
  ++stats->requests;
  // The 400 boundary is the one the rest of the fleet already draws:
  // 2xx-3xx succeeded, 4xx and 5xx did not.
  if (observation.status < 400) {
    ++stats->success;
  } else {
    ++stats->failure;
  }
  stats->duration_sum += duration;
  ++stats->duration_count;
  // The first bucket at or above the value; a value past the last one lands
  // only in +Inf, which the exposition takes from the count. Buckets are
  // upper-inclusive, which is what `le` means.
  const auto bucket = std::ranges::lower_bound(HttpLatencyBuckets(), duration);
  if (bucket != HttpLatencyBuckets().end()) {
    ++stats->buckets[static_cast<std::size_t>(bucket - HttpLatencyBuckets().begin())];
  }
}

void MetricsRegistry::RecordRejection(std::string_view method, int status) {
  if (!options_.enabled) {
    return;
  }
  // The unparsed sentinel rather than the nonstandard one: a 431 can fire
  // before the method token was ever read, and that is a different diagnosis
  // from a client inventing a verb. Both are constants, which is what the
  // label set needs.
  const RouteKey key{
      .method = method.empty() ? std::string(kUnparsedMethod) : NormalizeMethod(method),
      .route = std::string(kUnmatchedRoute)};

  const std::lock_guard<std::mutex> lock(mutex_);
  RouteStats* stats = AdmitRoute(key);
  if (stats == nullptr) {
    return;
  }
  // Counted, and deliberately not timed: a request refused at parse time has
  // no service latency to report, and filing it as a zero observation would
  // drag rate(_sum)/rate(_count) toward zero — flattering the latency panel
  // during exactly the flood it should be exposing.
  ++stats->requests;
  if (status < 400) {
    ++stats->success;
  } else {
    ++stats->failure;
  }
}

std::shared_ptr<internal::MetricFamily> MetricsRegistry::Register(std::string name,
                                                                  std::string help,
                                                                  internal::MetricFamily::Kind kind,
                                                                  std::vector<double> buckets) {
  if (!ValidName(name, /*allow_colon=*/true)) {
    smithy::internal::Fatal("smithy::server::MetricsRegistry: invalid metric name '" + name + "'");
  }
  // The built-ins are emitted unconditionally, so a family under one of their
  // names would appear twice with two TYPE lines — a scrape Prometheus
  // rejects whole. Checked against the configured names, since those are
  // what actually reach the exposition.
  for (const std::string_view reserved :
       {kRequestsTotal, kRequestsSuccess, kRequestsFailure, kRequestsActive, kRequestDuration,
        kObservationsDropped}) {
    if (name == reserved) {
      smithy::internal::Fatal("smithy::server::MetricsRegistry: '" + name +
                              "' is one of the built-in families");
    }
  }
  const std::lock_guard<std::mutex> lock(mutex_);
  if (auto found = families_.find(name); found != families_.end()) {
    // Idempotent for an identical re-registration; a mismatch is the case
    // that would corrupt the scrape, so it aborts rather than picking one.
    const internal::MetricFamily& existing = *found->second;
    if (existing.kind != kind || existing.help != help) {
      smithy::internal::Fatal("smithy::server::MetricsRegistry: '" + name +
                              "' is already registered with a different type or help text");
    }
    return found->second;
  }
  auto family = std::make_shared<internal::MetricFamily>();
  family->name = std::move(name);
  family->help = std::move(help);
  family->kind = kind;
  family->buckets = std::move(buckets);
  family->max_series = options_.max_series;
  families_.emplace(family->name, family);
  return family;
}

// A handle from a disabled registry holds no family, so every operation on
// it is a null check. The family is still registered either way: the name
// and collision checks in Register are exactly the fail-fast that must not
// wait for someone to turn metrics on in production.
Counter MetricsRegistry::NewCounter(std::string name, std::string help) {
  auto family =
      Register(std::move(name), std::move(help), internal::MetricFamily::Kind::kCounter, {});
  return Counter(options_.enabled ? std::move(family) : nullptr);
}

Gauge MetricsRegistry::NewGauge(std::string name, std::string help) {
  auto family =
      Register(std::move(name), std::move(help), internal::MetricFamily::Kind::kGauge, {});
  return Gauge(options_.enabled ? std::move(family) : nullptr);
}

Histogram MetricsRegistry::NewHistogram(std::string name, std::string help,
                                        std::vector<double> buckets) {
  // The same fail-fast the built-in ladder gets by being a constant. An
  // unsorted or non-finite ladder does not fail loudly at scrape time; it
  // silently produces cumulative buckets that disagree with themselves,
  // which a dashboard renders as plausible nonsense (ADR-0009).
  if (buckets.empty()) {
    smithy::internal::Fatal("smithy::server::MetricsRegistry: histogram '" + name +
                            "' needs at least one bucket (the +Inf bucket is implicit)");
  }
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    if (!std::isfinite(buckets[i])) {
      smithy::internal::Fatal("smithy::server::MetricsRegistry: histogram '" + name +
                              "' buckets must all be finite (the +Inf bucket is implicit)");
    }
    if (i > 0 && buckets[i] <= buckets[i - 1]) {
      smithy::internal::Fatal("smithy::server::MetricsRegistry: histogram '" + name +
                              "' buckets must be strictly ascending");
    }
  }
  auto family = Register(std::move(name), std::move(help), internal::MetricFamily::Kind::kHistogram,
                         std::move(buckets));
  return Histogram(options_.enabled ? std::move(family) : nullptr);
}

std::string MetricsRegistry::Expose() const {
  // A disabled registry has nothing to say, and MetricsEndpoint does not
  // serve it — see the header on why an empty 200 would be worse.
  if (!options_.enabled) {
    return {};
  }
  std::string out;
  const std::lock_guard<std::mutex> lock(mutex_);

  // Families are emitted whole and in order. The format requires every line
  // of a family to be contiguous, so each family is written in one pass —
  // including the drop counter below, whose per-family samples used to trail
  // the application families and split it in two.
  const auto route_labels = [this](const RouteKey& key) {
    return BuiltInLabels(
        {{std::string(kMethodLabel), key.method}, {std::string(kRouteLabel), key.route}});
  };
  const auto counter_family = [&](std::string_view name, std::string_view help,
                                  std::uint64_t RouteStats::*field) {
    AppendFamilyHeader(out, name, "counter", help);
    for (const auto& [key, stats] : routes_) {
      AppendSample(out, name, "", route_labels(key), std::to_string(stats.*field));
    }
  };
  // Three views of one tally, read off one record, so they cannot disagree.
  counter_family(kRequestsTotal, kRequestsTotalHelp, &RouteStats::requests);
  counter_family(kRequestsSuccess, kRequestsSuccessHelp, &RouteStats::success);
  counter_family(kRequestsFailure, kRequestsFailureHelp, &RouteStats::failure);

  AppendFamilyHeader(out, kRequestDuration, "histogram", kRequestDurationHelp);
  for (const auto& [key, stats] : routes_) {
    // A route that was only ever rejected has requests but nothing timed.
    // Emitting an all-zero histogram for it would claim an observation that
    // was deliberately not filed.
    if (stats.duration_count == 0) {
      continue;
    }
    const std::string labels = route_labels(key);
    const std::string prefix = labels + ",";
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < HttpLatencyBuckets().size(); ++i) {
      cumulative += stats.buckets[i];
      AppendSample(out, kRequestDuration, "_bucket",
                   prefix + "le=\"" + FormatNumber(HttpLatencyBuckets()[i]) + "\"",
                   std::to_string(cumulative));
    }
    // +Inf is the total by definition, which also covers values past the
    // last finite bucket.
    AppendSample(out, kRequestDuration, "_bucket", prefix + "le=\"+Inf\"",
                 std::to_string(stats.duration_count));
    AppendSample(out, kRequestDuration, "_sum", labels, FormatNumber(stats.duration_sum));
    AppendSample(out, kRequestDuration, "_count", labels, std::to_string(stats.duration_count));
  }

  // No route label, and no zero baseline: the gauge moves at request start,
  // where the method is the only bounded thing known, and the methods a
  // service will see are not knowable before it sees them.
  AppendFamilyHeader(out, kRequestsActive, "gauge", kRequestsActiveHelp);
  for (const auto& [method, count] : in_flight_) {
    AppendSample(out, kRequestsActive, "", BuiltInLabels({{std::string(kMethodLabel), method}}),
                 std::to_string(count));
  }

  // The drop counter, whole: the unlabeled total and every per-family
  // attribution under one header. `dropped` rides on the family's own line
  // rather than only the built-in counter, so a runaway label on one
  // application metric is attributable to it.
  AppendFamilyHeader(out, kObservationsDropped, "counter", kObservationsDroppedHelp);
  AppendSample(out, kObservationsDropped, "", BuiltInLabels({}),
               std::to_string(observations_dropped_));
  for (const auto& [name, family] : families_) {
    const std::lock_guard<std::mutex> family_lock(family->mutex);
    if (family->dropped != 0) {
      AppendSample(out, kObservationsDropped, "", BuiltInLabels({{"metric", name}}),
                   std::to_string(family->dropped));
    }
  }

  // Application families last, each whole and in name order; their samples
  // are already keyed by rendered labels, so a family's series are
  // contiguous the way the format requires.
  for (const auto& [name, family] : families_) {
    const std::lock_guard<std::mutex> family_lock(family->mutex);
    const char* type = "counter";
    if (family->kind == internal::MetricFamily::Kind::kGauge) type = "gauge";
    if (family->kind == internal::MetricFamily::Kind::kHistogram) type = "histogram";
    AppendFamilyHeader(out, name, type, family->help);
    for (const auto& [labels, sample] : family->samples) {
      if (family->kind != internal::MetricFamily::Kind::kHistogram) {
        AppendSample(out, name, "", labels, FormatNumber(sample.value));
        continue;
      }
      // Bucket lines always carry `le`, so they always have braces.
      std::string bucket_labels;
      std::uint64_t cumulative = 0;
      for (std::size_t i = 0; i < family->buckets.size(); ++i) {
        cumulative += sample.bucket_counts[i];
        bucket_labels = labels.empty() ? std::string() : labels + ",";
        bucket_labels += "le=\"";
        bucket_labels += FormatNumber(family->buckets[i]);
        bucket_labels += '"';
        AppendSample(out, name, "_bucket", bucket_labels, std::to_string(cumulative));
      }
      bucket_labels = labels.empty() ? std::string() : labels + ",";
      bucket_labels += "le=\"+Inf\"";
      AppendSample(out, name, "_bucket", bucket_labels, std::to_string(sample.count));
      AppendSample(out, name, "_sum", labels, FormatNumber(sample.value));
      AppendSample(out, name, "_count", labels, std::to_string(sample.count));
    }
  }
  return out;
}

Middleware RecordMetrics(std::shared_ptr<MetricsRegistry> registry) {
  if (registry == nullptr) {
    smithy::internal::Fatal("smithy::server::RecordMetrics: registry may not be null");
  }
  // Disabled: compose to the identity. Not a wrapper that checks a flag per
  // request — no wrapper at all, so the composed chain is byte-for-byte the
  // handler it would have been had this middleware never been written.
  if (!registry->enabled()) {
    return [](http::RequestHandler next) { return next; };
  }
  // Built on Observe rather than beside it: the request timing then has one
  // implementation, and the numbers the endpoint serves cannot drift from
  // what the logging hook reports about the same request.
  //
  // The two captures are sequenced into locals rather than written inline as
  // arguments: the second moves the registry, and argument evaluation order
  // is unspecified, so inline the move could run first and leave the other
  // lambda holding a null.
  auto complete = [registry](const RequestObservation& observation) {
    registry->Record(observation);
  };
  auto start = [registry = std::move(registry)](const RequestStart& request) {
    registry->RecordStart(request);
  };
  return Observe(std::move(complete), std::move(start));
}

Middleware MetricsEndpoint(std::shared_ptr<MetricsRegistry> registry, std::string path) {
  if (registry == nullptr) {
    smithy::internal::Fatal("smithy::server::MetricsEndpoint: registry may not be null");
  }
  // Disabled: the path is not served at all, so it reaches the router like
  // any other unmodeled path. An empty 200 would read to Prometheus as a
  // live target reporting no series — indistinguishable from a service whose
  // metrics have all gone silent, which is the alert you least want faked.
  if (!registry->enabled()) {
    return [](http::RequestHandler next) { return next; };
  }
  return [registry = std::move(registry), path = std::move(path)](http::RequestHandler next) {
    return [registry, path, next = std::move(next)](const http::HttpRequest& request) {
      const std::string_view target(request.target);
      if ((request.method == "GET" || request.method == "HEAD") &&
          target.substr(0, target.find('?')) == path) {
        http::HttpResponse response;
        response.status = 200;
        // The version is part of the content type Prometheus negotiates on;
        // it names the exposition format, not this library.
        response.headers.Set("content-type", "text/plain; version=0.0.4; charset=utf-8");
        // Only reached when this endpoint is composed inside the recorder
        // instead of outside it; the documented order never records a scrape.
        response.operation = path;
        // Set for HEAD too: the transport withholds the octets and keeps the
        // length (RFC 9110 §9.3.2), and that length is what the HEAD asked.
        response.body = registry->Expose();
        return response;
      }
      return next(request);
    };
  };
}

}  // namespace smithy::server
