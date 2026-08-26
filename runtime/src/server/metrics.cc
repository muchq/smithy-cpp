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
std::string_view NormalizeMethod(std::string_view method) {
  static constexpr std::array<std::string_view, 9> kKnown = {
      "GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS", "TRACE", "CONNECT"};
  const auto* found = std::ranges::find(kKnown, method);
  return found == kKnown.end() ? std::string_view("other") : *found;
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

}  // namespace internal

MetricsRegistry::MetricsRegistry(std::size_t max_series, std::vector<double> latency_buckets)
    : max_series_(max_series), buckets_(std::move(latency_buckets)) {
  // Composition-time validation (ADR-0009). An unsorted or non-finite ladder
  // does not fail loudly at scrape time — it silently produces cumulative
  // buckets that disagree with themselves, which a dashboard renders as
  // plausible nonsense.
  for (std::size_t i = 0; i < buckets_.size(); ++i) {
    if (!std::isfinite(buckets_[i])) {
      smithy::internal::Fatal(
          "smithy::server::MetricsRegistry: latency buckets must all be finite (the +Inf bucket is "
          "implicit)");
    }
    if (i > 0 && buckets_[i] <= buckets_[i - 1]) {
      smithy::internal::Fatal(
          "smithy::server::MetricsRegistry: latency buckets must be strictly ascending");
    }
  }
}

void MetricsRegistry::RecordStart(const RequestStart& start) {
  (void)start;  // method/target are not gauge labels; see the header's note
  const std::lock_guard<std::mutex> lock(mutex_);
  ++in_flight_;
}

void MetricsRegistry::Record(const RequestObservation& observation) {
  // Seconds is the Prometheus base unit, and the division is the only place
  // the microsecond hook meets the float histogram.
  const double seconds = std::chrono::duration<double>(observation.duration).count();
  const CountKey count_key{.method = std::string(NormalizeMethod(observation.method)),
                           .operation = observation.operation,
                           .status = observation.status};
  const LatencyKey latency_key{.method = count_key.method, .operation = count_key.operation};

  const std::lock_guard<std::mutex> lock(mutex_);
  // Only decrement a gauge that was incremented: without RecordStart wired up
  // the gauge stays at zero rather than counting downward forever.
  if (in_flight_ > 0) {
    --in_flight_;
  }

  // One observation refused is one increment, whichever family had to turn
  // it away — the counter answers "how much traffic am I blind to", so
  // counting it once per family would overstate the gap.
  bool dropped = false;
  if (auto found = counts_.find(count_key); found != counts_.end()) {
    ++found->second;
  } else if (counts_.size() < max_series_) {
    counts_.emplace(count_key, 1);
  } else {
    dropped = true;
  }

  auto latency = latencies_.find(latency_key);
  if (latency == latencies_.end() && latencies_.size() >= max_series_) {
    dropped = true;
  } else {
    if (latency == latencies_.end()) {
      latency =
          latencies_
              .emplace(latency_key,
                       HistogramData{.counts = std::vector<std::uint64_t>(buckets_.size(), 0)})
              .first;
    }
    HistogramData& histogram = latency->second;
    histogram.sum_seconds += seconds;
    ++histogram.count;
    // The first bucket at or above the value; a value past the last one
    // lands only in +Inf, which the exposition takes from `count`.
    const auto bucket = std::ranges::lower_bound(buckets_, seconds);
    if (bucket != buckets_.end()) {
      ++histogram.counts[static_cast<std::size_t>(bucket - buckets_.begin())];
    }
  }
  if (dropped) {
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
  // rejects whole.
  for (const std::string_view reserved :
       {"smithy_http_requests_total", "smithy_http_request_duration_seconds",
        "smithy_http_requests_in_flight", "smithy_metrics_observations_dropped_total"}) {
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
  family->max_series = max_series_;
  families_.emplace(family->name, family);
  return family;
}

Counter MetricsRegistry::NewCounter(std::string name, std::string help) {
  return Counter(
      Register(std::move(name), std::move(help), internal::MetricFamily::Kind::kCounter, {}));
}

Gauge MetricsRegistry::NewGauge(std::string name, std::string help) {
  return Gauge(
      Register(std::move(name), std::move(help), internal::MetricFamily::Kind::kGauge, {}));
}

Histogram MetricsRegistry::NewHistogram(std::string name, std::string help,
                                        std::vector<double> buckets) {
  return Histogram(Register(std::move(name), std::move(help),
                            internal::MetricFamily::Kind::kHistogram, std::move(buckets)));
}

std::string MetricsRegistry::Expose() const {
  std::string out;
  const std::lock_guard<std::mutex> lock(mutex_);

  // Families are emitted whole and in order — std::map keeps every series of
  // a family contiguous, which the format requires. Headers print even with
  // no samples yet, so a freshly started server still describes its shape.
  AppendFamilyHeader(out, "smithy_http_requests_total", "counter",
                     "Total HTTP requests served, by method, Smithy operation, and status code.");
  for (const auto& [key, value] : counts_) {
    out += "smithy_http_requests_total{method=\"";
    out += EscapeLabel(key.method);
    out += "\",operation=\"";
    out += EscapeLabel(key.operation);
    out += "\",status=\"";
    out += std::to_string(key.status);
    out += "\"} ";
    out += std::to_string(value);
    out += '\n';
  }

  AppendFamilyHeader(out, "smithy_http_request_duration_seconds", "histogram",
                     "Request latency in seconds, by method and Smithy operation.");
  for (const auto& [key, histogram] : latencies_) {
    const std::string labels = "method=\"" + EscapeLabel(key.method) + "\",operation=\"" +
                               EscapeLabel(key.operation) + "\"";
    std::uint64_t cumulative = 0;
    for (std::size_t i = 0; i < buckets_.size(); ++i) {
      cumulative += histogram.counts[i];
      out += "smithy_http_request_duration_seconds_bucket{";
      out += labels;
      out += ",le=\"";
      out += FormatNumber(buckets_[i]);
      out += "\"} ";
      out += std::to_string(cumulative);
      out += '\n';
    }
    // +Inf is the total by definition, which also covers values past the
    // last finite bucket.
    out += "smithy_http_request_duration_seconds_bucket{";
    out += labels;
    out += ",le=\"+Inf\"} ";
    out += std::to_string(histogram.count);
    out += "\nsmithy_http_request_duration_seconds_sum{";
    out += labels;
    out += "} ";
    out += FormatNumber(histogram.sum_seconds);
    out += "\nsmithy_http_request_duration_seconds_count{";
    out += labels;
    out += "} ";
    out += std::to_string(histogram.count);
    out += '\n';
  }

  AppendFamilyHeader(out, "smithy_http_requests_in_flight", "gauge",
                     "Requests currently being served.");
  out += "smithy_http_requests_in_flight ";
  out += std::to_string(in_flight_);
  out += '\n';

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
