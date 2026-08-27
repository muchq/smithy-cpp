#ifndef SMITHY_SERVER_MIDDLEWARE_H_
#define SMITHY_SERVER_MIDDLEWARE_H_

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "smithy/http/forwarded.h"
#include "smithy/http/transport.h"

namespace smithy::server {

// User-supplied middleware wraps the transport-facing handler a generated
// server exposes: inspect or reject requests before dispatch, observe or
// amend responses after. Middleware composes outside the generated router,
// so it works with any HttpServerTransport:
//
//   WeatherServer server(handler);
//   transport.Start(smithy::server::Chain({AuthCheck(), Observe(log)},
//                                         server.Handler()));
using Middleware = std::function<http::RequestHandler(http::RequestHandler)>;

// Composes middleware around a handler. The first element is outermost: it
// sees the request first and the response last.
http::RequestHandler Chain(std::vector<Middleware> middleware, http::RequestHandler handler);

// Admission control outside the router: admit(request) true passes the
// request through, false short-circuits with reject(request). The policy —
// a rate limiter, an IP allowlist, a maintenance switch — is the
// application's dependency; Guard owns only the composition point and the
// short-circuit. Neither callback may be null.
// Both callbacks run on the transport's request thread, concurrently across
// requests — an injected policy (e.g. a shared rate limiter) must be
// thread-safe.
Middleware Guard(std::function<bool(const http::HttpRequest&)> admit,
                 std::function<http::HttpResponse(const http::HttpRequest&)> reject);

// Ready-made Guard rejection for rate limiting: 429 with body
// {"error":"Too many requests"}, plus a Retry-After header (seconds) when
// retry_after is set.
std::function<http::HttpResponse(const http::HttpRequest&)> TooManyRequests(
    std::optional<std::chrono::seconds> retry_after = std::nullopt);

// The composed ADR-0012 admission chain (issue #104): derive the client
// behind the configured proxy trust boundary, consult allow(client), shed
// with the shaped TooManyRequests 429 when it refuses. The
// derivation-into-admission wiring lives here, written and tested once
// (ADR-0012 records why hand-wiring it is the hazard). allow is the
// pluggable policy (a token bucket) keyed by the derived bare address; it
// runs on the transport's request thread, concurrently across requests —
// it must be thread-safe. A null allow is a contract violation and aborts
// at composition time (ADR-0009). A request with no derivable client (the in-memory
// Loopback, handler chains driven directly in tests — Source::kUnknown)
// is admitted without consulting allow, so such requests never share one
// "" bucket; compose Guard + DeriveClient directly for a stricter policy.
// Every other source IS consulted — including Source::kTrustedTier, where
// the key is the trusted tier's own address: behind a proxy that is not
// appending x-forwarded-for, all traffic shares that one bucket (the
// dashboard signal in docs/production-guide.md catches this).
Middleware PerClientRateLimit(std::function<bool(const std::string& client)> allow,
                              http::TrustedProxies trusted,
                              std::optional<std::chrono::seconds> retry_after = std::nullopt);

// A named readiness dependency: probe() returns true when it can serve.
// Probes run on the transport's request thread, once per probe request,
// concurrently across requests — they must be thread-safe and cheap (cache
// expensive checks behind the callable). A probe that throws counts as
// failing; the exception is logged, never reaching the transport. The name
// lands verbatim in the JSON failing list, so HealthEndpoint aborts at
// composition time (ADR-0009) for a name that would corrupt it (quote,
// backslash, control character) — or for a null probe, which would
// otherwise present as a permanent outage.
struct ReadinessCheck {
  std::string name;
  std::function<bool()> probe;
};

// Health endpoint: answers GET or HEAD <path> (query string ignored); every
// other request passes through to the next handler, so a model may still
// define other routes on the path. With no checks it is a static liveness
// probe: always 200 {"status":"healthy"}. With checks it is a readiness
// probe: 200 when every check passes, else 503
// {"status":"unhealthy","failing":[<names>]} — so one server typically
// composes two instances:
//
//   Chain({HealthEndpoint("/livez"),
//          HealthEndpoint("/readyz", {{"db", [&] { return db.Alive(); }}})},
//         server.Handler());
//
// A HEAD is answered like the GET, body included: the transport withholds
// the octets and keeps the length (RFC 9110 §9.3.2), and that length is
// what the HEAD was asking for.
//
// Probe responses carry <path> as their HttpResponse::operation, so a probe
// composed inside an observability chain reports as itself rather than as
// the empty operation that 404s and 405s already use.
Middleware HealthEndpoint(std::string path = "/health", std::vector<ReadinessCheck> checks = {});

// One served request, as seen from outside the router.
struct RequestObservation {
  std::string method;
  std::string target;
  // The Smithy operation that handled the request (from the generated
  // router's HttpResponse::operation annotation); empty for 404/405/400
  // dispatch failures. HealthEndpoint and MetricsEndpoint report their own
  // path here, so `operation="/health"` can be filtered out of a latency
  // panel and an empty operation means a dispatch failure and nothing else.
  std::string operation;
  // The request's W3C traceparent header, verbatim, for log correlation.
  // Never empty for requests served through a transport: the ingress mints a
  // root context when the client sent none (ADR-0011; server_dispatch.h).
  // Empty only when the handler chain is driven directly in tests. See
  // smithy/http/trace_context.h to parse it.
  std::string trace_parent;
  int status = 0;
  // Microseconds so fast-path latencies (cache hits, loopback) don't report
  // as zero; duration_cast to coarser units at the metrics boundary if
  // needed.
  std::chrono::microseconds duration{0};
};

// What on_start sees, before the router runs. The Smithy operation is not
// yet known pre-dispatch, so start observations are labeled by method and
// target only.
struct RequestStart {
  std::string method;
  std::string target;
};

// Middleware reporting every request to callbacks — the structured-logging
// and metrics hook. on_complete runs after the response is built (count =
// callbacks, latency = duration); the optional on_start runs before dispatch
// so an in-flight gauge can increment (pair it with on_complete's decrement —
// the two always pair, even when dispatch throws: the completion then
// reports status 500 with an empty operation before the exception continues
// to the transport's containment). Throwing callbacks are logged and
// swallowed. A null on_complete aborts at composition time (ADR-0009; a null
// sink would otherwise fail silently); on_start and now may be null. Callbacks run on the
// transport's request thread; keep them cheap or hand off. now is injectable for deterministic
// tests (null means steady_clock).
Middleware Observe(std::function<void(const RequestObservation&)> on_complete,
                   std::function<void(const RequestStart&)> on_start = nullptr,
                   std::function<std::chrono::steady_clock::time_point()> now = nullptr);

// 401 unless the request carries "authorization: Bearer <token>" (scheme
// matched case-insensitively per RFC 6750) and validator(token) returns
// true — the server-side counterpart of @httpBearerAuth.
Middleware RequireBearerAuth(std::function<bool(const std::string&)> validator);

// 401 unless header_name carries the key — prefixed "<scheme> " when scheme
// is non-empty — and validator(key) returns true; the server-side
// counterpart of @httpApiKeyAuth(in: "header").
Middleware RequireApiKeyHeader(std::string header_name, std::string scheme,
                               std::function<bool(const std::string&)> validator);

}  // namespace smithy::server

#endif  // SMITHY_SERVER_MIDDLEWARE_H_
