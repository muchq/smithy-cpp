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
std::string NormalizeMethod(std::string_view method, const std::string& nonstandard) {
  static constexpr std::array<std::string_view, 9> kKnown = {
      "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "TRACE", "CONNECT"};
  const auto* found = std::ranges::find(kKnown, method);
  return found == kKnown.end() ? nonstandard : std::string(*found);
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

void AppendFamilyHeader(std::string& out, std::string_view name, std::string_view type,
                        std::string_view help) {
  out += "# HELP ";
  out += name;
  out += ' ';
  out += help;
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

MetricsOptions MetricsOptions::Aura(std::string service_name) {
  // The exposition MoonBase's three emitter rails share and its prom_proxy
  // dashboards query. Every literal here is pinned on the MoonBase side —
  // the names and descriptions by //domains/platform/libs/otel_contract, the
  // route vocabulary by its label test, the buckets by its bucket test — so
  // treat this whole function as a transcription, not a design.
  MetricsOptions options;
  options.requests_total_name = "http_server_requests_total";
  options.requests_success_name = "http_server_requests_success_total";
  options.requests_failure_name = "http_server_requests_failure_total";
  options.request_duration_name = "http_server_request_duration_microseconds";
  options.requests_in_flight_name = "http_server_requests_active_gauge";

  options.requests_total_help = "HTTP requests received";
  options.requests_success_help = "HTTP requests completed successfully (2xx-3xx)";
  options.requests_failure_help = "HTTP requests that returned 4xx or 5xx";
  options.request_duration_help = "HTTP request duration in microseconds";
  options.requests_in_flight_help = "HTTP requests currently in flight";

  options.method_label = "http_method";
  options.route_label = "route";
  // The outcome rides on the success and failure counters instead. Keeping
  // status as well would multiply every series by the codes seen for no
  // gain: the dashboards aggregate with sum() and never select on it.
  options.status_label = "";
  options.constant_labels = {{"service_name", std::move(service_name)}};
  options.in_flight_by_method = true;

  options.unrouted_route = "unmatched";
  options.nonstandard_method = "CUSTOM";
  options.unparsed_method = "(unparsed)";

  options.latency_unit = LatencyUnit::kMicroseconds;
  options.latency_buckets = AuraLatencyBuckets();
  return options;
}

MetricsRegistry::MetricsRegistry(MetricsOptions options) : options_(std::move(options)) {
  // Composition-time validation (ADR-0009), and deliberately not conditional
  // on `enabled`: a name or ladder that would corrupt the scrape must abort
  // on the first run either way, so that turning metrics on in production is
  // never the first time these run.
  //
  // An unsorted or non-finite ladder does not fail loudly at scrape time. It
  // silently produces cumulative buckets that disagree with themselves,
  // which a dashboard renders as plausible nonsense.
  const std::vector<double>& buckets = options_.latency_buckets;
  for (std::size_t i = 0; i < buckets.size(); ++i) {
    if (!std::isfinite(buckets[i])) {
      smithy::internal::Fatal(
          "smithy::server::MetricsRegistry: latency buckets must all be finite (the +Inf bucket is "
          "implicit)");
    }
    if (i > 0 && buckets[i] <= buckets[i - 1]) {
      smithy::internal::Fatal(
          "smithy::server::MetricsRegistry: latency buckets must be strictly ascending");
    }
  }
  // Every configured name reaches the exposition verbatim, so an invalid one
  // yields a scrape Prometheus rejects in full — with no in-process consumer
  // to notice. The success and failure names are optional; the rest are not.
  for (const std::string* name : {&options_.requests_total_name, &options_.request_duration_name,
                                  &options_.requests_in_flight_name}) {
    if (!ValidName(*name, /*allow_colon=*/true)) {
      smithy::internal::Fatal("smithy::server::MetricsRegistry: invalid metric name '" + *name +
                              "'");
    }
  }
  for (const std::string* name :
       {&options_.requests_success_name, &options_.requests_failure_name}) {
    if (!name->empty() && !ValidName(*name, /*allow_colon=*/true)) {
      smithy::internal::Fatal("smithy::server::MetricsRegistry: invalid metric name '" + *name +
                              "'");
    }
  }
  for (const std::string* label : {&options_.method_label, &options_.route_label}) {
    if (!ValidName(*label, /*allow_colon=*/false)) {
      smithy::internal::Fatal("smithy::server::MetricsRegistry: invalid label name '" + *label +
                              "'");
    }
  }
  if (!options_.status_label.empty() && !ValidName(options_.status_label, /*allow_colon=*/false)) {
    smithy::internal::Fatal("smithy::server::MetricsRegistry: invalid label name '" +
                            options_.status_label + "'");
  }
  for (const auto& [name, value] : options_.constant_labels) {
    (void)value;
    if (!ValidName(name, /*allow_colon=*/false)) {
      smithy::internal::Fatal("smithy::server::MetricsRegistry: invalid label name '" + name + "'");
    }
  }
}

std::string MetricsRegistry::BuiltInLabels(const MetricLabels& labels) const {
  // Not RenderLabels: these are emitted in a fixed order (constants, then
  // method, route, status) rather than sorted, so the built-in families read
  // the way the header documents them. Prometheus does not care about label
  // order; a human reading a scrape does.
  std::string out;
  const auto append = [&out](const std::string& name, const std::string& value) {
    if (name.empty()) {
      return;
    }
    if (!out.empty()) {
      out += ',';
    }
    out += name;
    out += "=\"";
    out += EscapeLabel(value);
    out += '"';
  };
  for (const auto& [name, value] : options_.constant_labels) {
    append(name, value);
  }
  for (const auto& [name, value] : labels) {
    append(name, value);
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
  std::string method = NormalizeMethod(start.method, options_.nonstandard_method);
  const std::lock_guard<std::mutex> lock(mutex_);
  ++in_flight_[std::move(method)];
}

void MetricsRegistry::Record(const RequestObservation& observation) {
  if (!options_.enabled) {
    return;
  }
  // Seconds is the Prometheus base unit and the default; a fleet whose
  // dashboards are already written against microseconds cannot read seconds
  // without rewriting every query. This is the only place the microsecond
  // hook meets the float histogram either way.
  const double duration = options_.latency_unit == LatencyUnit::kMicroseconds
                              ? static_cast<double>(observation.duration.count())
                              : std::chrono::duration<double>(observation.duration).count();
  // A request that reached no operation reports the configured constant
  // rather than nothing, so a fleet whose dashboards select on a sentinel
  // ("unmatched") can say so instead of matching the empty string.
  const CountKey count_key{
      .method = NormalizeMethod(observation.method, options_.nonstandard_method),
      .route = observation.operation.empty() ? options_.unrouted_route : observation.operation,
      .status = observation.status};
  const LatencyKey latency_key{.method = count_key.method, .route = count_key.route};

  const std::lock_guard<std::mutex> lock(mutex_);
  // Only decrement a gauge that was incremented: without RecordStart wired up
  // the gauge stays at zero rather than counting downward forever.
  if (auto in_flight = in_flight_.find(count_key.method);
      in_flight != in_flight_.end() && in_flight->second > 0) {
    --in_flight->second;
  }

  // One observation refused is one increment, whichever family had to turn
  // it away — the counter answers "how much traffic am I blind to", so
  // counting it once per family would overstate the gap.
  bool dropped = false;
  if (auto found = counts_.find(count_key); found != counts_.end()) {
    ++found->second;
  } else if (counts_.size() < options_.max_series) {
    counts_.emplace(count_key, 1);
  } else {
    dropped = true;
  }

  auto latency = latencies_.find(latency_key);
  if (latency == latencies_.end() && latencies_.size() >= options_.max_series) {
    dropped = true;
  } else {
    if (latency == latencies_.end()) {
      latency = latencies_
                    .emplace(latency_key, HistogramData{.counts = std::vector<std::uint64_t>(
                                                            options_.latency_buckets.size(), 0)})
                    .first;
    }
    HistogramData& histogram = latency->second;
    histogram.sum += duration;
    ++histogram.count;
    // The first bucket at or above the value; a value past the last one
    // lands only in +Inf, which the exposition takes from `count`. Buckets
    // are upper-inclusive, which is what `le` means.
    const auto bucket = std::ranges::lower_bound(options_.latency_buckets, duration);
    if (bucket != options_.latency_buckets.end()) {
      ++histogram.counts[static_cast<std::size_t>(bucket - options_.latency_buckets.begin())];
    }
  }
  if (dropped) {
    ++observations_dropped_;
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
  const CountKey key{.method = method.empty()
                                   ? options_.unparsed_method
                                   : NormalizeMethod(method, options_.nonstandard_method),
                     .route = options_.unrouted_route,
                     .status = status};
  const std::lock_guard<std::mutex> lock(mutex_);
  if (auto found = counts_.find(key); found != counts_.end()) {
    ++found->second;
  } else if (counts_.size() < options_.max_series) {
    counts_.emplace(key, 1);
  } else {
    ++observations_dropped_;
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
  for (const std::string& reserved :
       {options_.requests_total_name, options_.requests_success_name,
        options_.requests_failure_name, options_.request_duration_name,
        options_.requests_in_flight_name,
        std::string("smithy_metrics_observations_dropped_total")}) {
    if (!reserved.empty() && name == reserved) {
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

  // Families are emitted whole and in order — std::map keeps every series of
  // a family contiguous, which the format requires. Headers print even with
  // no samples yet, so a freshly started server still describes its shape.
  //
  // The request counters are three views of one tally rather than three
  // tallies: success and failure are derived from the same status-keyed
  // counts the total sums, so they cannot disagree with it or with each
  // other, and they need no drop accounting of their own.
  struct Outcome {
    std::uint64_t total = 0;
    std::uint64_t success = 0;
    std::uint64_t failure = 0;
  };
  std::map<LatencyKey, Outcome> by_route;
  for (const auto& [key, value] : counts_) {
    Outcome& outcome = by_route[LatencyKey{.method = key.method, .route = key.route}];
    outcome.total += value;
    // The 400 boundary is the one the rest of the fleet already draws:
    // 2xx-3xx succeeded, 4xx and 5xx did not.
    if (key.status < 400) {
      outcome.success += value;
    } else {
      outcome.failure += value;
    }
  }
  const auto route_labels = [this](const LatencyKey& key) {
    return BuiltInLabels({{options_.method_label, key.method}, {options_.route_label, key.route}});
  };

  AppendFamilyHeader(out, options_.requests_total_name, "counter", options_.requests_total_help);
  if (options_.status_label.empty()) {
    for (const auto& [key, outcome] : by_route) {
      AppendSample(out, options_.requests_total_name, "", route_labels(key),
                   std::to_string(outcome.total));
    }
  } else {
    for (const auto& [key, value] : counts_) {
      AppendSample(out, options_.requests_total_name, "",
                   BuiltInLabels({{options_.method_label, key.method},
                                  {options_.route_label, key.route},
                                  {options_.status_label, std::to_string(key.status)}}),
                   std::to_string(value));
    }
  }

  if (!options_.requests_success_name.empty()) {
    AppendFamilyHeader(out, options_.requests_success_name, "counter",
                       options_.requests_success_help);
    for (const auto& [key, outcome] : by_route) {
      AppendSample(out, options_.requests_success_name, "", route_labels(key),
                   std::to_string(outcome.success));
    }
  }
  if (!options_.requests_failure_name.empty()) {
    AppendFamilyHeader(out, options_.requests_failure_name, "counter",
                       options_.requests_failure_help);
    for (const auto& [key, outcome] : by_route) {
      AppendSample(out, options_.requests_failure_name, "", route_labels(key),
                   std::to_string(outcome.failure));
    }
  }

  AppendFamilyHeader(out, options_.request_duration_name, "histogram",
                     options_.request_duration_help);
  for (const auto& [key, histogram] : latencies_) {
    const std::string labels = route_labels(key);
    const std::string prefix = labels.empty() ? std::string() : labels + ",";
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < options_.latency_buckets.size(); ++i) {
      cumulative += histogram.counts[i];
      AppendSample(out, options_.request_duration_name, "_bucket",
                   prefix + "le=\"" + FormatNumber(options_.latency_buckets[i]) + "\"",
                   std::to_string(cumulative));
    }
    // +Inf is the total by definition, which also covers values past the
    // last finite bucket.
    AppendSample(out, options_.request_duration_name, "_bucket", prefix + "le=\"+Inf\"",
                 std::to_string(histogram.count));
    AppendSample(out, options_.request_duration_name, "_sum", labels, FormatNumber(histogram.sum));
    AppendSample(out, options_.request_duration_name, "_count", labels,
                 std::to_string(histogram.count));
  }

  AppendFamilyHeader(out, options_.requests_in_flight_name, "gauge",
                     options_.requests_in_flight_help);
  if (options_.in_flight_by_method) {
    // No zero baseline here: the method labels are not known until traffic
    // arrives, so there is no series to declare. The unlabeled form below
    // can be baselined and is.
    for (const auto& [method, count] : in_flight_) {
      AppendSample(out, options_.requests_in_flight_name, "",
                   BuiltInLabels({{options_.method_label, method}}), std::to_string(count));
    }
  } else {
    std::int64_t total = 0;
    for (const auto& [method, count] : in_flight_) {
      (void)method;
      total += count;
    }
    AppendSample(out, options_.requests_in_flight_name, "", BuiltInLabels({}),
                 std::to_string(total));
  }

  AppendFamilyHeader(out, "smithy_metrics_observations_dropped_total", "counter",
                     "Observations dropped after the registry hit its series cap.");
  out += "smithy_metrics_observations_dropped_total ";
  out += std::to_string(observations_dropped_);
  out += '\n';
  // Application families last, each whole and in name order; their samples
  // are already keyed by rendered labels, so a family's series are
  // contiguous the way the format requires. `dropped` rides on the family's
  // own line rather than the built-in counter, so a runaway label on one
  // application metric is attributable to it.
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
    if (family->dropped != 0) {
      out += "smithy_metrics_observations_dropped_total{metric=\"" + EscapeLabel(name) + "\"} ";
      out += std::to_string(family->dropped);
      out += '\n';
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
