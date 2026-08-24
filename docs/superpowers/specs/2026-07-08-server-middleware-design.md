# Server middleware additions: Guard, HealthEndpoint, Observe on_start

**Date:** 2026-07-08
**Status:** Approved
**Motivation:** First production pilot (migrating MoonBase's `portrait` service off
meerkat) needs three capabilities meerkat provides today: rate limiting, a health
endpoint, and request metrics including an in-flight gauge. This spec closes those
gaps in `smithy/server/middleware.h` without adding any dependency to the runtime —
in particular, no opentelemetry-cpp (its protobuf/gRPC tree violates the runtime's
dep-light rule; see production-guide.md "Observability").

## Design stance

Same split resilience4go uses: the framework owns the composition point and the
HTTP contract; the application owns the policy. The rate-limiting *algorithm*,
the metrics *backend*, and the *decision* of what to admit all stay app-side deps.
smithy-cpp ships only generic, dependency-free middleware.

## 1. `Guard` — generic admission middleware

```cpp
Middleware Guard(std::function<bool(const http::HttpRequest&)> admit,
                 std::function<http::HttpResponse(const http::HttpRequest&)> reject);

// Convenience reject factory; matches meerkat's refusal shape.
std::function<http::HttpResponse(const http::HttpRequest&)> TooManyRequests(
    std::optional<std::chrono::seconds> retry_after = std::nullopt);
```

- `admit(request)` returning true passes the request to `next`; false returns
  `reject(request)` without dispatching to the router.
- `TooManyRequests()` returns status 429, `content-type: application/json`, body
  `{"error":"Too many requests"}`, plus a `Retry-After` header when `retry_after`
  is set.
- Both callbacks are required (non-null); passing a null `std::function` is
  documented as undefined (consistent with `Chain`'s existing contract).
- Rate limiting is one instantiation: the app injects its limiter (e.g. MoonBase's
  `futility::rate_limiter::SlidingWindowRateLimiter` keyed on `X-Forwarded-For`)
  inside `admit`. Other instantiations: IP allowlists, maintenance mode.
- No peer-address plumbing: deployments behind a reverse proxy key on
  `X-Forwarded-For` from `request.headers`. Exposing the socket peer address on
  `HttpRequest` is out of scope (nothing in the pilot needs it).

## 2. `HealthEndpoint` — static liveness

```cpp
Middleware HealthEndpoint(std::string path = "/health");
```

- Short-circuits `GET` or `HEAD <path>` with 200, `content-type:
  application/json`, body `{"status":"healthy"}` (body omitted for HEAD, so
  transports never emit a mismatched content-length — added in PR review).
  Match is exact on the path portion of `target` (query string ignored).

  > **Superseded (#192):** the HEAD carve-out above was backwards. Omitting the
  > body in the handler does not avoid a mismatched content-length, it *is* one
  > — the transport sizes the response from the body it is handed, so `HEAD
  > /livez` answered `Content-Length: 0` where the GET answered 20. Framing is
  > the transport's: it now withholds the octets and keeps the length
  > (RFC 9110 §9.3.2), and the handler returns the same body either way.

- Requests to the path with any other method pass through to `next` (the
  router 404/405s them, or serves them if the model defines such a route).
  Any other target passes through untouched.
- Static body only — no timestamp (meerkat includes one; nothing consumes it).
  Readiness checks (dependency probes, 503 on degraded) are deliberately
  deferred; the signature can grow an optional checks parameter later without
  breaking callers.
- Addendum (issue #49): the reserved growth path landed as
  `HealthEndpoint(std::string path = "/health", std::vector<ReadinessCheck>
  checks = {})` — with checks the endpoint is a readiness probe (200 when every
  check passes, else 503 `{"status":"unhealthy","failing":[<names>]}`); probes
  run per request, and a throwing probe counts as failing. Liveness and
  readiness compose as two instances (`/livez` bare, `/readyz` with checks).

## 3. `Observe` grows an optional `on_start`

```cpp
struct RequestStart {
  std::string method;
  std::string target;
};

Middleware Observe(std::function<void(const RequestObservation&)> on_complete,
                   std::function<void(const RequestStart&)> on_start = nullptr,
                   std::function<std::chrono::steady_clock::time_point()> now = nullptr);
```

- `on_start` fires before dispatch; `on_complete` fires after the response is
  built, exactly as today. Existing single-callback call sites compile unchanged.
- `RequestStart` carries only `method` and `target`: the Smithy operation is not
  known until the router runs, so pre-dispatch observations cannot be labeled by
  operation (same limitation as meerkat's `RecordRequestStart`).
- Start/complete are always paired: if `next` throws, `on_complete` still runs —
  reporting `status = 500` and the measured duration, `operation` empty — and the
  exception then propagates unchanged to the transport's containment
  (server_dispatch.h). An in-flight gauge driven by the pair can therefore never
  leak.
- Callbacks run on the transport's request thread; keep them cheap or hand off
  (existing guidance, unchanged).

## Composition order (documented default)

```cpp
transport.Start(smithy::server::Chain(
    {Guard(admit, TooManyRequests()),   // outermost: shed load first
     Observe(on_complete, on_start),    // observes everything admitted, incl. health
     HealthEndpoint()},                 // liveness, visible in metrics
    server.Handler()));
```

Guard outermost sheds abusive traffic before it costs anything; health probes are
admitted, rate-limited, and counted in request metrics (operation label empty).
Order is the application's choice — this is the documented recommendation, not a
constraint.

## Out of scope

- The MoonBase-side adapter (`Observe` callbacks → `futility::otel::MetricsRecorder`,
  gauge `http_server_requests_active`, counter `http_server_requests`, histogram
  `http_server_request_duration`) and portrait's `Main.cc` wiring — that is the
  portrait migration plan, in MoonBase.
- An optional `//runtime:otel` adapter target (PLAN.md defers post-0.1.0).
- Readiness checks, peer-address exposure, response-size observations.

## Error handling

- `Guard`'s `reject` response is returned as-is; the middleware never throws.
- `HealthEndpoint` allocates nothing per request beyond the response struct.
- `Observe` exception pairing as above; throwing `on_start`/`on_complete`
  callbacks are logged and swallowed (the existing `Observe` containment
  behavior — a misbehaving metrics backend must never discard a response).
  A null `on_complete` throws `std::invalid_argument` at composition time
  (added in PR review): the containment above would otherwise swallow the
  `bad_function_call` on every request, so a null sink would fail silently —
  unlike `Guard`'s nulls, which fail loudly via transport containment and
  stay a documented precondition.

## Testing

- Unit tests alongside the existing middleware tests in `runtime/tests`:
  - Guard: admit passes through untouched; reject short-circuits (router never
    called); `TooManyRequests` shape with and without `Retry-After`.
  - HealthEndpoint: GET path → 200 body; HEAD path → 200 empty body; GET
    path?query → 200; POST path → pass through; other targets → pass through.
  - Observe: `on_start` receives method/target before dispatch; pairing holds
    when `next` throws; existing one-callback signature still compiles (no
    source change to existing tests); injectable clock still drives `duration`.
- Integration: extend the `examples/bazel-consumer` integration test to compose
  all three around a generated server, driven by the generated client (keeps the
  quickstart's CI-cannot-rot property).

## Documentation

- `production-guide.md` "Server middleware": add the three primitives and the
  composition-order example above, with the app-side limiter/OTel-adapter sketch.
- `CHANGELOG.md` entry under Unreleased.
