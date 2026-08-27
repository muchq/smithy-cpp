#include "smithy/server/middleware.h"

#include <cctype>
#include <cstddef>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "smithy/core/exception_guard.h"
#include "smithy/core/fatal.h"

namespace smithy::server {

http::RequestHandler Chain(std::vector<Middleware> middleware, http::RequestHandler handler) {
  // Wrap inside-out so the first middleware ends up outermost.
  for (const Middleware& wrap : middleware | std::views::reverse) {
    handler = wrap(std::move(handler));
  }
  return handler;
}

Middleware Guard(std::function<bool(const http::HttpRequest&)> admit,
                 std::function<http::HttpResponse(const http::HttpRequest&)> reject) {
  return [admit = std::move(admit), reject = std::move(reject)](http::RequestHandler next) {
    return [admit, reject, next = std::move(next)](const http::HttpRequest& request) {
      if (!admit(request)) {
        return reject(request);
      }
      return next(request);
    };
  };
}

std::function<http::HttpResponse(const http::HttpRequest&)> TooManyRequests(
    std::optional<std::chrono::seconds> retry_after) {
  return [retry_after](const http::HttpRequest&) {
    http::HttpResponse response;
    response.status = 429;
    response.headers.Set("content-type", "application/json");
    if (retry_after.has_value()) {
      response.headers.Set("retry-after", std::to_string(retry_after->count()));
    }
    response.body = R"({"error":"Too many requests"})";
    return response;
  };
}

Middleware PerClientRateLimit(std::function<bool(const std::string& client)> allow,
                              http::TrustedProxies trusted,
                              std::optional<std::chrono::seconds> retry_after) {
  if (allow == nullptr) {
    // A null policy would throw std::bad_function_call per request — fail
    // fast at composition like HealthEndpoint and Observe do (ADR-0009).
    smithy::internal::Fatal("smithy::server::PerClientRateLimit: allow must not be null");
  }
  return Guard(
      [allow = std::move(allow), trusted = std::move(trusted)](const http::HttpRequest& request) {
        const http::DerivedClient client = http::DeriveClient(request, trusted);
        return client.source == http::DerivedClient::Source::kUnknown || allow(client.address);
      },
      TooManyRequests(retry_after));
}

namespace {

// A throwing observation sink (e.g. a metrics backend under backpressure)
// must not discard a response or unwind into the transport thread; swallow
// it after logging. Contain() makes this a no-op wrapper under
// -fno-exceptions, where the sink cannot throw.
template <typename Callback, typename Observation>
void CallContained(const Callback& callback, const Observation& observation, const char* which) {
  smithy::internal::Contain(
      [&] { callback(observation); },
      [&](const char* what) {
        if (what != nullptr) {
          std::clog << "smithy: " << which << " callback threw: " << what << "\n";
        } else {
          std::clog << "smithy: " << which << " callback threw a non-std exception\n";
        }
      });
}

// Same containment policy for readiness probes: a throw is a failing
// dependency, never an unwind into the transport — but the exception
// message is the one clue to why /readyz is flapping, so keep the log
// trail.
bool ProbeContained(const ReadinessCheck& check) {
  return smithy::internal::Contain(
      [&] { return check.probe(); },
      [&](const char* what) -> bool {
        if (what != nullptr) {
          std::clog << "smithy: readiness probe '" << check.name << "' threw: " << what << "\n";
        } else {
          std::clog << "smithy: readiness probe '" << check.name << "' threw a non-std exception\n";
        }
        return false;
      });
}

}  // namespace

Middleware HealthEndpoint(std::string path, std::vector<ReadinessCheck> checks) {
  // Composition-time validation, like Observe's null-sink check: a null
  // probe would otherwise present as a permanent dependency outage, and a
  // name with JSON syntax in it would corrupt the failing list exactly when
  // monitoring is parsing it.
  for (const ReadinessCheck& check : checks) {
    if (check.probe == nullptr) {
      smithy::internal::Fatal("smithy::server::HealthEndpoint: check '" + check.name +
                              "' has a null probe");
    }
    for (const char c : check.name) {
      if (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) {
        smithy::internal::Fatal("smithy::server::HealthEndpoint: check name '" + check.name +
                                "' contains a quote, backslash, or control character");
      }
    }
  }
  return [path = std::move(path), checks = std::move(checks)](http::RequestHandler next) {
    return [path, checks, next = std::move(next)](const http::HttpRequest& request) {
      const std::string_view target(request.target);
      if ((request.method == "GET" || request.method == "HEAD") &&
          target.substr(0, target.find('?')) == path) {
        // Probes run per request: a readiness endpoint that caches would
        // keep answering 200 while a dependency is down.
        std::string failing;
        for (const ReadinessCheck& check : checks) {
          if (!ProbeContained(check)) {
            if (!failing.empty()) {
              failing += ',';
            }
            failing += '"';
            failing += check.name;
            failing += '"';
          }
        }
        http::HttpResponse response;
        response.status = failing.empty() ? 200 : 503;
        response.headers.Set("content-type", "application/json");
        // Probes are labeled with the endpoint's own path. Without it they
        // report as the empty operation, which is also what 404s and 405s
        // report — so probe traffic and dispatch failures land in one series
        // and neither can be read. The path is fixed at composition, never
        // off the wire, so this adds one series per composed endpoint.
        response.operation = path;
        // Set for HEAD too. Framing is the transport's, which withholds the
        // octets and keeps the length (RFC 9110 §9.3.2); emptying the body
        // here would answer Content-Length: 0 instead — a false claim about
        // the resource, and the only question a HEAD asks.
        response.body = failing.empty() ? R"({"status":"healthy"})"
                                        : R"({"status":"unhealthy","failing":[)" + failing + "]}";
        return response;
      }
      return next(request);
    };
  };
}

Middleware Observe(std::function<void(const RequestObservation&)> on_complete,
                   std::function<void(const RequestStart&)> on_start,
                   std::function<std::chrono::steady_clock::time_point()> now) {
  if (on_complete == nullptr) {
    smithy::internal::Fatal("smithy::server::Observe: on_complete may not be null");
  }
  if (now == nullptr) {
    now = [] { return std::chrono::steady_clock::now(); };
  }
  return [on_complete = std::move(on_complete), on_start = std::move(on_start),
          now = std::move(now)](http::RequestHandler next) {
    return [on_complete, on_start, now, next = std::move(next)](const http::HttpRequest& request) {
      if (on_start != nullptr) {
        CallContained(on_start, RequestStart{request.method, request.target}, "Observe on_start");
      }
      RequestObservation observation;
      observation.method = request.method;
      observation.target = request.target;
      observation.trace_parent = request.headers.Get("traceparent").value_or("");
      const auto start = now();
      http::HttpResponse response;
#if defined(__cpp_exceptions)
      try {
        response = next(request);
      } catch (...) {
        // Keep start/complete paired when dispatch throws: report a 500
        // completion, then let the exception continue to the transport's
        // containment (server_dispatch.h). Under -fno-exceptions next()
        // cannot throw, so this pairing is unreachable and compiled out.
        observation.status = 500;
        observation.duration = std::chrono::duration_cast<std::chrono::microseconds>(now() - start);
        CallContained(on_complete, observation, "Observe on_complete");
        throw;
      }
#else
      response = next(request);
#endif
      observation.operation = response.operation;
      observation.status = response.status;
      observation.duration = std::chrono::duration_cast<std::chrono::microseconds>(now() - start);
      CallContained(on_complete, observation, "Observe on_complete");
      return response;
    };
  };
}

namespace {

http::HttpResponse Unauthorized() {
  http::HttpResponse response;
  response.status = 401;
  return response;
}

// The credential after "<scheme> " (scheme matched case-insensitively), or
// nullopt when the value does not carry that scheme.
std::optional<std::string> StripScheme(const std::string& value, const std::string& scheme) {
  const std::size_t prefix = scheme.size() + 1;
  if (value.size() <= prefix || value[scheme.size()] != ' ') {
    return std::nullopt;
  }
  for (std::size_t i = 0; i < scheme.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(value[i])) !=
        std::tolower(static_cast<unsigned char>(scheme[i]))) {
      return std::nullopt;
    }
  }
  return value.substr(prefix);
}

}  // namespace

Middleware RequireBearerAuth(std::function<bool(const std::string&)> validator) {
  return RequireApiKeyHeader("authorization", "Bearer", std::move(validator));
}

Middleware RequireApiKeyHeader(std::string header_name, std::string scheme,
                               std::function<bool(const std::string&)> validator) {
  return [header_name = std::move(header_name), scheme = std::move(scheme),
          validator = std::move(validator)](http::RequestHandler next) {
    return
        [header_name, scheme, validator, next = std::move(next)](const http::HttpRequest& request) {
          std::optional<std::string> credential = request.headers.Get(header_name);
          if (credential.has_value() && !scheme.empty()) {
            credential = StripScheme(*credential, scheme);
          }
          if (!credential.has_value() || !validator(*credential)) {
            return Unauthorized();
          }
          return next(request);
        };
  };
}

}  // namespace smithy::server
