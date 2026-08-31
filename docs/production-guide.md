# Production guide

How to configure generated smithy-cpp clients and servers for production use:
timeouts, retries, and request compression. Every knob lives on
`smithy::ClientConfig` (`smithy/client/config.h`), so the guidance below
applies to every generated client the same way.

```cpp
#include "myservice/client.h"
#include "smithy/client/config.h"

smithy::ClientConfig config;
config.endpoint = "http://api.example.com:8080";
config.request_timeout_ms = 5000;
config.retry.max_attempts = 5;
auto client = myservice::MyServiceClient::Create(std::move(config));
```

## Timeouts

`config.request_timeout_ms` (default 30000) bounds each HTTP attempt —
connect plus request plus response — on the built-in socket transport. A
timed-out attempt fails with a retryable transport error, so it feeds the
retry loop below. A `BeastHttpClient` built via `FromConfig` (see
[Production client transport](#production-client-transport)) honors the
same value, so the timeout is configured once. If you inject a transport
you constructed yourself, that transport owns timeout enforcement; the
built-in behavior is the reference.

Pick a timeout from your service's latency tail (a small multiple of p99),
not a comfortable-sounding round number: with retries enabled the worst-case
caller wait is roughly `max_attempts × timeout` plus backoff sleeps.

## Retries

Every generated client sends through `smithy::SendWithRetries`
(`smithy/client/retry.h`). Two failure classes are retried:

- **Transport errors flagged retryable** — connection refused/reset,
  timeouts.
- **Transient HTTP statuses** — 429, 500, 502, 503, 504 (the set every
  Smithy SDK treats as transient). Other statuses, including 400/403/404 and
  modeled errors, are returned immediately.

Backoff is **full-jitter exponential**: retry *n* sleeps
`uniform(0, min(max_backoff, initial_backoff × 2^(n-1)))`. Jitter
desynchronizes clients after a shared failure, so a recovering server is not
hit by a synchronized thundering herd.

```cpp
config.retry.max_attempts = 3;                              // total tries; 1 disables retries
config.retry.initial_backoff = std::chrono::milliseconds(100);
config.retry.max_backoff = std::chrono::milliseconds(20000);
```

Guidance:

- **Interactive paths:** keep `max_attempts` low (2–3) and cap
  `max_backoff` near your latency budget; a user-facing call gains nothing
  from a 20-second sleep.
- **Batch/background paths:** raise `max_attempts` and let `max_backoff`
  breathe; throttling (429) resolves on its own if you back off.
- **Idempotency:** retries resend the same serialized request.
  `@idempotencyToken` members are generated once per call and reused across
  attempts, so the server can deduplicate. For non-idempotent operations
  without a token, weigh whether a retried timeout can double-apply.
- **Tests:** wire-exact tests set `config.retry.max_attempts = 1` (generated
  suites already do). To test retry behavior deterministically, inject
  `config.retry.sleep` and `config.retry.jitter`.

## Request compression

Operations modeled with `@requestCompression(encodings: ["gzip"])` gzip
their request body when it reaches
`config.request_min_compression_size_bytes` (default 10240, the Smithy
default; 0 compresses everything). The client appends `gzip` to any existing
`Content-Encoding` header value. Nothing is configured per call — model the
trait and the generated client and server both handle it:

- **Client:** compresses via `smithy::GzipCompress` (`//runtime:compression`,
  zlib) after serialization, before send.
- **Server:** generated routes for `@requestCompression` operations
  transparently gunzip requests arriving with `Content-Encoding: gzip`
  (or `..., gzip`) and reject malformed gzip bodies with a 400
  serialization error. Decompression is capped (64 MB) to stop
  decompression-bomb inputs.

Compression trades CPU for bytes: leave the 10 KiB threshold alone unless
you have measured small-payload wins; compressing tiny bodies usually
inflates them.

## Auth

Services modeled with `@httpBearerAuth` or `@httpApiKeyAuth` get credential
wiring generated into their clients — set the provider on the config and
every request carries it (providers are called per request, so rotation
just works):

```cpp
config.bearer_token = [] { return LoadToken(); };   // @httpBearerAuth
config.api_key = [] { return LoadApiKey(); };       // @httpApiKeyAuth
```

Bearer tokens ride as `authorization: Bearer <token>`; API keys go where
the model binds them — a named header (with the trait's scheme prefix, if
any) or a query parameter. A null provider leaves requests anonymous.

Server-side, the matching guards ship as middleware
(`smithy/server/middleware.h`):

```cpp
transport.Start(smithy::server::Chain(
    {smithy::server::RequireBearerAuth([](const std::string& token) {
      return TokenIsValid(token);  // 401 otherwise
    })},
    server.Handler()));
// Or: smithy::server::RequireApiKeyHeader("x-api-key", /*scheme=*/"", validator)
```

Vendor-specific signing schemes (e.g. SigV4) are out of scope by design;
implement them as an `Interceptor` (below).

## Pagination

Operations modeled with `@paginated` (top-level string tokens) get a
generated paginator: `client.PaginateListCities(input)` returns a
`ListCitiesPaginator` that is a single-pass range (issue #49) — iteration
yields one `smithy::Outcome<Page>&` per page, and a failed call is yielded
exactly once before the range ends by itself, so the loop needs no manual
token or nullopt protocol:

```cpp
for (auto& page : client.PaginateListCities({.pageSize = 100})) {
  if (!page.ok()) return page.error();   // pagination stops on first error
  for (const auto& city : page->items) Process(city);
}
```

The paginator owns a copy of the client and input, so it outlives both; the
range is single-pass (call `begin()` once — range-for does). The pull API
remains for manual control: `Next()` yields one page at a time,
`std::nullopt` once the service stops returning a next token, or the first
failed call's error. An empty-string token is treated as end-of-pagination
(defensive: it can never loop forever on a server echoing empty tokens).

## Client interceptors

`config.interceptors` (`smithy/client/interceptor.h`) hooks user code around
every HTTP attempt a generated client makes — auth headers, tracing ids,
request/response logging — without touching generated code:

```cpp
class BearerAuth final : public smithy::Interceptor {
 public:
  void ModifyBeforeTransmit(smithy::http::HttpRequest& request, int attempt) override {
    request.headers.Set("authorization", "Bearer " + LoadToken());
  }
  void ReadAfterTransmit(const smithy::http::HttpRequest& request,
                         const smithy::Outcome<smithy::http::HttpResponse>& outcome,
                         int attempt) override {
    LogAttempt(request.target, attempt, outcome.ok() ? outcome->status : -1);
  }
};

config.interceptors.push_back(std::make_shared<BearerAuth>());
```

Interceptors run in registration order, around each attempt (retries included
— `attempt` is 1-based). `ModifyBeforeTransmit` mutates a fresh copy of the
request per attempt, so edits never accumulate across retries or leak into
the caller's view. Hooks must not throw.

## Server middleware

Generated servers expose their router as a plain
`smithy::http::RequestHandler`, so cross-cutting server behavior — auth
checks, request logging, metrics — composes as middleware outside the
generated code (`smithy/server/middleware.h`), with any transport:

```cpp
WeatherServer server(handler);

// Policy stays an application dependency (your rate limiter, your metrics
// backend); the middleware owns only the composition point.
auto limiter = std::make_shared<MyRateLimiter>(/* window, budget */);
auto db = std::make_shared<MyDbPool>(/* ... */);

// The deployment's proxy trust boundary (ADR-0012): x-forwarded-for
// entries count only when appended by these networks. Directly reachable
// (no proxy)? Say so: TrustedProxies::None() — the header is then ignored
// and every request keys as its TCP peer. Parse() rejects a malformed CIDR
// with an Error::Validation, so fail startup rather than deploy a boundary
// that silently widens or narrows.
auto trusted_result = smithy::http::TrustedProxies::Parse({"10.0.0.0/8"});
if (!trusted_result) { /* report trusted_result.error(); refuse to start */ }
const smithy::http::TrustedProxies trusted = *std::move(trusted_result);

transport.Start(smithy::server::Chain(
    {// Outermost: shed abusive traffic before it costs anything. The
     // framework derives the client behind the trust boundary and keys
     // admission on it — never the raw header, which any client can write
     // (smithy/http/forwarded.h has the derivation contract).
     smithy::server::PerClientRateLimit(
         [limiter](const std::string& client) { return limiter->Allow(client); },
         trusted, std::chrono::seconds(30)),
     // Observe everything admitted — health probes included, reporting
     // `operation` as their own path so a dashboard can filter them out.
     // Hand it the SAME trust boundary as the limiter: without it the
     // observation reports the peer while the limiter keyed on the
     // forwarded client, and the log cannot answer a question about the
     // limiter's own decision.
     smithy::server::Observe(
         [](const smithy::server::RequestObservation& o) {
           // One access-log record: o.method, o.target, o.operation,
           // o.status, o.duration, o.trace_parent, o.request_bytes,
           // o.response_bytes, o.handler_threw (a contained crash, not a
           // deliberate 500), and o.client — the derived client with its
           // .source provenance, which is the bucket the limiter keyed on.
           // Also gauge -1; count 1; latency o.duration.
         },
         [](const smithy::server::RequestStart& s) {
           // gauge +1 (labeled by s.method/s.target; the operation is not
           // known until the router runs).
         },
         nullptr, trusted),
     // Liveness: GET or HEAD /livez -> 200 {"status":"healthy"}. A HEAD
     // gets that body's Content-Length and none of its octets, framed by
     // the transport; everything else passes through to the router.
     smithy::server::HealthEndpoint("/livez"),
     // Readiness: the same endpoint with checks. Every probe runs on every
     // request (no caching — a cached 200 would hide a dependency outage);
     // any failure answers 503 {"status":"unhealthy","failing":["db"]}.
     // A throwing probe counts as failing, never unwinds into the transport.
     smithy::server::HealthEndpoint(
         "/readyz", {{"db", [db] { return db->Alive(); }}})},
    server.Handler()));
```

The first middleware in the chain is outermost: it sees the request first and
can short-circuit before anything below it runs (so the limiter's rejections
never reach `Observe` — track rejection rates in the limiter itself, or
compose `Observe` *outside* the limiter, which logs the 429s with the client
they were rejected for at the cost of observing traffic you refused). Because
admission keys on the derived client address, health probes budget as their
real source (the node or balancer address the transport saw) rather than
sharing one spoofable key with abusive traffic; if even that source's own
budget matters, compose the `HealthEndpoint` instances outside the limiter.
Requests with no derivable client at all (the in-memory `Loopback` has no
peer) are admitted without consulting your policy, so hand-driven tests
never rate-limit each other through one shared empty key. A trusted peer
that sent no header keys as the tier's own address — correct, and the
reason the dashboard signal below matters when a proxy stops appending the
header.

Readiness probes run on the transport's request thread, once per probe
request — keep them cheap (a pool's cached connectivity flag, not a fresh
dial) and thread-safe. `Guard` is the generic admission primitive
underneath — IP allowlists, maintenance mode — admit/reject callbacks in,
one decision point out; `PerClientRateLimit` is `Guard` with the ADR-0012
derivation wired in by the framework (the ADR records why hand-wiring it is
the hazard).

`Observe`'s callbacks run on the transport's request thread (keep them
cheap or hand off) and always pair: when dispatch throws, `on_complete`
reports a 500 completion before the exception reaches the transport's
containment, so an in-flight gauge can never leak. Throwing callbacks are
logged and swallowed.

**Watch the trust boundary itself.** A drifted trust set (the proxy's
address changed; the CIDR didn't) fails silently: the spoof defense ignores
the header on every request and all traffic collapses onto the proxy's one
key. The fingerprint is visible in `smithy::http::DeriveClient` — the
richer form of `ClientAddress` that also reports *how* the address was
derived. `Observe` reports it directly — `o.client.source`, derived against
the `TrustedProxies` you passed it — so counting it needs no extra
middleware. On the dashboard: behind a proxy, ~100% `kUntrustedHeaderIgnored`
means the trust set no longer matches the topology, and ~100% `kTrustedTier`
means the proxy is not appending `x-forwarded-for`.

**Plumbing the trust set.** The boundary is deployment config; the
convention is a `TRUSTED_PROXY_CIDRS` environment variable holding a
comma-separated CIDR list, parsed once at startup. Unset must mean a
*deliberate* direct-connect topology — and only unset: a set-but-empty
value is a parse error like any other malformed entry, so a template that
renders an empty string fails startup instead of silently collapsing
proxied traffic onto one key (the issue-#104 accident, config edition):

```cpp
const char* cidrs = std::getenv("TRUSTED_PROXY_CIDRS");
smithy::http::TrustedProxies trusted = smithy::http::TrustedProxies::None();
if (cidrs != nullptr) {
  // the comma-list splitter from smithy/http/headers.h
  auto parsed = smithy::http::TrustedProxies::Parse(smithy::http::SplitHeaderListValues(cidrs));
  if (!parsed) { /* log parsed.error(); refuse to start */ }
  trusted = *std::move(parsed);
}
```

## Serving lifecycle

The pattern for a long-running server is SIGTERM/SIGINT → `Start`/block/`Stop`, with the drain
([Server hardening](#server-hardening) has the contract) doing the graceful half. This is
`main()` from [`examples/simplerestjson/serve_main.cc`](../examples/simplerestjson/serve_main.cc)
verbatim — compiled, lifecycle-tested in CI, and runnable as
`bazel run //examples/simplerestjson:bookstore_server`:

```cpp
int main(int argc, char** argv) {
  sigset_t shutdown_signals;
  sigemptyset(&shutdown_signals);
  sigaddset(&shutdown_signals, SIGINT);
  sigaddset(&shutdown_signals, SIGTERM);
  // Before Start(): threads the transport creates inherit this mask, so the
  // shutdown signals reach only the sigwait() below.
  pthread_sigmask(SIG_BLOCK, &shutdown_signals, nullptr);

  BookstoreServer server(std::make_shared<InMemoryBookstore>());
  smithy::http::BeastServerTransport transport({
      .address = "0.0.0.0",
      .port = argc > 1 ? std::atoi(argv[1]) : 8080,  // 0 binds an ephemeral port
      .drain_timeout_seconds = 10,
  });
  smithy::Outcome<smithy::Unit> started = transport.Start(server.Handler());
  if (!started.ok()) {
    std::fprintf(stderr, "bookstore: start failed: %s\n", started.error().message().c_str());
    return 1;
  }
  std::fprintf(stderr, "bookstore: serving on :%d (SIGTERM or Ctrl-C drains and exits)\n",
               transport.port());

  int signal_number = 0;
  sigwait(&shutdown_signals, &signal_number);  // serve until SIGTERM/SIGINT
  std::fprintf(stderr, "bookstore: signal %d, draining\n", signal_number);
  transport.Stop();  // in-flight requests get drain_timeout_seconds to finish
  return 0;
}
```

Under Kubernetes: SIGTERM is exactly what the kubelet sends, so size
`terminationGracePeriodSeconds` above `drain_timeout_seconds`, and compose the `/livez` +
`/readyz` probes from the middleware chain above in front of the handler — readiness flips
traffic away while the drain finishes.

## Observability

The runtime's observability story is deliberately SDK-free: enriched hooks
on both sides plus W3C Trace Context helpers, so any backend — including
OpenTelemetry — plugs in without the core taking a telemetry dependency.

**Server:** `Observe` (above) reports, per request: `method`, `target`,
`operation` (the Smithy operation that handled it, stamped by the generated
router; the endpoint's own path for `HealthEndpoint` and `MetricsEndpoint`;
empty for 404/405/400 dispatch failures), `status`, `duration`, and
`trace_parent` — the request's W3C `traceparent` header, which always parses:
a valid inbound one continues verbatim, and the transport ingress mints a
fresh root when the client sent none or sent garbage (ADR-0011). The same
trace id is the `x-correlation-id` on the contained 500 when a handler
throws. An optional `on_start` callback fires before dispatch (method and
target only), enabling in-flight gauges; start/complete always pair, even
when the handler throws.

It also reports `request_bytes` and `response_bytes`, a `handler_threw` flag,
and `client` — the ADR-0012 derived client (address plus provenance), **not**
the raw `x-forwarded-for`, which a direct client can forge. That is the
identity `PerClientRateLimit` keys on, so it is the one that answers "whose
bucket did that 429 come from"; pass `Observe` the same `TrustedProxies` you
give the limiter or the two will disagree. Unset means
`TrustedProxies::None()` — the deliberate direct-connect statement, under
which the peer is the client and the header is ignored wholly. Watch the
distribution of `client.source`: every request reporting `kDirectPeer` with
one address means you are behind a proxy and did not say so.

`handler_threw` separates "we crashed" from "the handler deliberately
answered 500" — both report status 500 with no operation, and an access log
that cannot tell them apart sends whoever reads a 5xx spike looking for the
wrong thing. A thrown request has no response, so its `response_bytes` is 0;
`handler_threw` is what makes that an absence rather than an empty body. The
exception text stays on the transport's containment log, which carries the
same trace id.

**Client:** two ready-made interceptors in
`smithy/client/observability.h`:

```cpp
// Metrics/logging: one callback per HTTP attempt (retries visible).
config.interceptors.push_back(smithy::ObserveAttempts(
    [](const smithy::AttemptObservation& a) {
      // a.method, a.target, a.attempt, a.status (-1 = transport error),
      // a.error_message
    }));

// Distributed tracing: sets a W3C traceparent header on every attempt that
// lacks one. Pass a callback returning your application's active trace
// context to join an existing trace; omit it to start fresh roots.
config.interceptors.push_back(smithy::PropagateTraceContext());
```

`smithy/http/trace_context.h` has the underlying helpers —
`ParseTraceparent`, `FormatTraceparent`, `GenerateTraceContext`,
`GenerateSpanId` — for building richer integrations (e.g. a server
middleware that opens a span from `RequestObservation::trace_parent`).

**Prometheus:** the one bundled backend, because the text exposition format
needs no client library — it is a few lines of text over HTTP, so it costs
zero dependencies. Two middleware compose around the generated handler:

```cpp
auto metrics = std::make_shared<smithy::server::MetricsRegistry>(
    smithy::server::MetricsOptions{.enabled = true, .service_name = "todo-service"});
transport.Start(smithy::server::Chain({smithy::server::MetricsEndpoint(metrics),
                                       smithy::server::RecordMetrics(metrics)},
                                      server.Handler()));
```

**It is off unless you say otherwise.** `MetricsOptions::enabled` defaults to
false, and a disabled registry is not a registry that records into a void: the
two middleware compose to the *identity*, so nothing wraps the request path —
no timing, no lock, not even an extra call frame — and `/metrics` reaches the
router like any other unmodeled path and 404s. (A disabled endpoint answering
an empty 200 would read to Prometheus as a live target reporting no series,
which is exactly what a service whose metrics have gone silent looks like.)
Handles from a disabled registry are inert rather than unusable, so
application code never branches on the flag; only their *arguments* still
cost anything, so guard a hot call site whose labels are themselves expensive
with `metrics->enabled()`.

Registration is not conditional on the flag. An invalid metric name, a type
collision, or a bad bucket ladder aborts at startup either way (ADR-0009), so
switching metrics on in production is never the first time those checks run.

`RecordMetrics` is `Observe` wired to the registry, so request timing has one
implementation and the scraped numbers cannot drift from the logged ones. The
order above is deliberate: the endpoint sits *outside* the recorder, so
scrapes answer without being counted as served traffic — swap them and every
scrape inflates your own request rate, at whatever interval Prometheus polls.

Five families are exposed on `/metrics` (path configurable), labeled by
`service_name`, `http_method` and `route`:

| Family | Type | Notes |
| --- | --- | --- |
| `http_server_requests_total` | counter | every completed request |
| `http_server_requests_success_total` | counter | status < 400 |
| `http_server_requests_failure_total` | counter | status >= 400 |
| `http_server_requests_active_gauge` | gauge | no `route` label; see below |
| `http_server_request_duration_microseconds` | histogram | `_bucket`/`_sum`/`_count` |

plus `metrics_observations_dropped_total`, the registry's own health.

This is not a vocabulary of our own invention, and it is deliberately **not
configurable**. It is [MoonBase](https://github.com/muchq/MoonBase)'s shared
HTTP serving contract — spoken identically by its Java (yodel), Rust
(server_pal) and C++ (futility/otel, behind aura) emitters, and pinned across
them by `//domains/platform/libs/otel_contract`: names, descriptions, label
sets, route sentinels and bucket boundaries alike. Those services are who
scrapes this, and their dashboards (prom_proxy) query exactly these names with
exactly these labels. A knob here would be a way for one service to drift off
that contract, and the drift is silent — the panel renders empty, which looks
like a quiet service rather than a misconfigured one. If a second fleet ever
needs a different dialect, that is the point to design one.

`service_name` is required whenever metrics are enabled, and an empty one
aborts at construction: every dashboard query selects on it, so a service
reporting the empty string is scraped, stored, and invisible.

Three consequences of the contract worth knowing:

- **Status is not a label.** The outcome rides on the success and failure
  counters, which are two views of the same tally the total sums — so the
  three can never disagree, and no series is multiplied by the codes a
  service happens to return.
- **The active gauge carries no route.** It moves at request start, before
  dispatch, where nothing bounded is known about the path. Every rail leaves
  the route off it for that reason, and prom_proxy's negative
  `route!="/health"` matcher passes a series without the label through
  untouched — which is what makes the same filter safe on it.
- **Durations are microseconds**, on the ladder the rails pin equal.
  `histogram_quantile` reads `le` off bucket counts, so a service on a
  different ladder charts a quantile computed against different bins than
  everything beside it.

The label set is bounded by construction, because cardinality is what
actually kills a metrics endpoint. `target` is deliberately *not* a label —
it carries path parameters and query strings, so one series per distinct URL
is one series per request id; `route` is the bounded stand-in the router
stamps from the model, and a request that reached no operation reports the
`unmatched` sentinel rather than the empty string (`route!="/health"` matches
the empty string, so unrouted traffic would silently join the serving
figures). `HealthEndpoint` and `MetricsEndpoint` answer paths the model does
not define, so they stamp that path as their route (`route="/health"`):
probes are usually a service's highest-volume route, and left unlabeled they
would bury the 404 rate in the sentinel they share with it, and mix their own
latency into the same duration histogram. The path is fixed at composition,
so it is one series per composed endpoint — compose the probes inside
`RecordMetrics` if you want them counted, outside it if you do not.
`http_method` arrives from the wire, so anything outside the nine RFC 9110
verbs collapses to `CUSTOM` rather than minting a series per invented verb,
and a request rejected before its method parsed reports `(unparsed)`. Past
`max_series` combinations the registry stops minting and counts what it
refused in `metrics_observations_dropped_total` — alert on that being
non-zero rather than discovering the cap as an OOM.

Two things composition still has to get right for the MoonBase dashboards:
compose `HealthEndpoint()` on its default `/health` path and *inside*
`RecordMetrics`, because prom_proxy subtracts `route!="/health"` from every
serving number and charts that route on its own tile — a service that never
reports it reads as having no probe rather than as a healthy one. And point
Prometheus at the service directly: this produces the collector's output
shape without the collector.

Your own metrics share the same scrape — one Prometheus target covers the
service, rather than the built-in families sitting behind one endpoint and
your domain numbers behind another. Mint a family once and keep the handle:

```cpp
auto orders = metrics->NewCounter("orders_processed_total", "Orders processed.");
auto latency = metrics->NewHistogram("order_pipeline_seconds", "Pipeline time.");
auto depth = metrics->NewGauge("queue_depth", "Pending jobs.");

orders.Increment({{"region", "us-east"}});
latency.Observe(elapsed.count());
depth.Set(pending);
```

Declare the series whose labels are known at startup — `orders.Declare({{"region",
"us-east"}})`, or `depth.Declare()` for an unlabeled one. A series nobody has
touched is simply absent from the scrape, and a counter whose first exported
sample is its first event's value hides that event for good: `increase()` and
`rate()` measure the change *between* samples, so with nothing earlier the
first one shows no increase at all and the panel reads zero — worse than a
missing tile, because it looks like an answer. Declaring is idempotent and
never disturbs a series that already has events. A declared histogram is
genuinely empty rather than an observation of zero, so `rate(_sum)/rate(_count)`
stays unbiased. Labels carrying request data have no series to declare (and
are the cardinality problem above); bound them to a known kind and declare
that instead.

Handles are cheap to copy and address the same family, so a handler can hold
them as members. The registry keeps owning the parts that are easy to get
wrong: label values are escaped, labels are sorted so `{a,b}` and `{b,a}` are
one series rather than two, and the same per-family cap applies — a label
taken from unbounded data (a user id) costs that family its series budget and
is attributed on
`metrics_observations_dropped_total{metric="..."}` instead of taking
the process down. A metric name that isn't a valid Prometheus name, or that
collides with an existing family under a different type, aborts at
registration: both produce a scrape Prometheus rejects in full, and nothing
in-process would notice.

One more hook is worth wiring, because middleware cannot reach it. The
transport answers over-limit requests (413/431) itself, while the parser is
still reading and before any handler chain exists — so `RecordMetrics` never
sees them and an over-limit flood would be invisible in the counters:

```cpp
options.on_rejected = smithy::server::RecordRejections(metrics);
```

These count as requests (`route="unmatched"`, with `413`/`431` as the
signature) but file no latency and never move the in-flight gauge: a request
refused at parse time has no service latency to report, and recording it as a
zero observation would drag `rate(_sum)/rate(_count)` down — flattering the
latency panel during exactly the flood it should be exposing. Because they
are counted and not timed, a route that only ever saw rejections appears in
the request counters with no histogram series at all. A method that never
parsed (a 431 can fire mid-headers) is labeled `(unparsed)` rather than
`CUSTOM`, since "never parsed" and "client invented a verb" are different
diagnoses.

The endpoint is unauthenticated: it is middleware, so gate it the way you
gate anything else — compose `Guard` or `RequireBearerAuth` outside it, or
bind the scrape listener somewhere the internet cannot reach.

**OpenTelemetry:** not bundled, by design — opentelemetry-cpp's dependency
tree (protobuf, gRPC for OTLP) would violate the runtime's dep-light rule.
The hooks above map 1:1 onto OTel spans and metrics; an optional
`//runtime:otel` adapter is planned post-0.1.0 once the hook shapes have
survived production use (see PLAN.md).

## Production client transport

`SocketHttpClient` (the `config.endpoint` default) is a test/reference
transport (ADR-0006), kept as the zero-dependency fallback for plain-http
endpoints: plaintext, connection-per-request. Production clients should
inject `BeastHttpClient` (ADR-0007): keep-alive connection pooling,
per-request timeouts, and TLS with certificate and hostname verification on
by default.

Every knob lives on the one `ClientConfig` (issue #49):
`config.tls.ca_pem` / `config.tls.verify_peer` for trust,
`config.max_idle_connections` for pooling, and the same
`config.request_timeout_ms` the rest of this guide tunes.
`BeastHttpClient::FromConfig` reads them all — endpoint, TLS, timeout, and
pool size come from the config, so nothing is configured twice:

```cpp
smithy::ClientConfig config;
config.endpoint = "https://api.example.com";   // identity + path prefix
config.tls.ca_pem = corp_ca_pem;               // only when not publicly trusted
auto transport = smithy::http::BeastHttpClient::FromConfig(config);
if (!transport) { /* bad URL */ }
config.http_client = *transport;               // the wire
auto client = MyServiceClient::Create(std::move(config));
```

`ca_pem` (PEM text, not a file path) replaces the system trust roots for
private CAs; `config.tls.verify_peer = false` exists as an escape hatch for
local experiments and must never reach production. The lower-level
`BeastHttpClient::Options` constructor remains for tests and custom wiring
(loopback ports, deliberately broken TLS). `//runtime:http_beast` is
self-contained — it carries the asio SSL implementation and the BoringSSL
dependency itself, so no extra build flags are needed.

## Event streams

A streaming operation (ADR-0016) needs no extra client configuration: the
WebSocket dial derives host, port, and TLS from the same `config.endpoint`
and `config.tls` the unary transport uses (an `https` endpoint dials `wss`).
The call returns the typed session; drive it with the canonical loop —
`Receive()`'s nullopt is the peer's clean close, and a received exception is
terminal, surfacing exactly like a unary modeled error:

```cpp
auto stream = client.Converse(input);            // upgrade GET on the @http URI
if (!stream) { /* dial/refusal error */ }
while (true) {
  auto event = stream->Receive();
  if (!event.ok()) { /* modeled exception or wire failure */ break; }
  if (!event->has_value()) break;                // server closed cleanly
  /* dispatch on (**event).is_...() */
  (void)stream->Send(/* your next event */);
}
stream->Close();                                 // idempotent; also the cancel path
```

An operation that models no client-to-server events returns a receive-only
stream: its `Send` does not compile (the `NoEvents` direction), so drive it
with `Receive`/`Close` only.

`Receive()` blocks until *something* happens — a message, the peer's close,
or a failure. When waiting forever is the wrong answer (a test asserting an
event, a caller with other work to do), pass a deadline:

```cpp
auto event = stream->Receive(std::chrono::seconds(2));
if (!event.ok() && event.error().code() == "TimeoutError") {
  // Nothing arrived in time. The session is untouched: assert, log, retry
  // the wait, send, or close — the caller decides.
}
```

The timeout is a fourth outcome, distinct from the clean close (`nullopt`)
and from a broken session (`TransportError`), and it is the only failure that
leaves the stream usable — which is what separates it from `Close()`, the
other way to end a wait (that one ends the session for good, and reports the
peer's own close). The bound is always real: the overload is pure virtual on
`smithy::http::WebSocket`, so every session — the two shipped transports, the
delegating wrappers, and any socket you implement yourself — answers the
deadline or does not compile. There is no default that quietly blocks
forever. The same overload exists one layer down, on `WebSocket` itself, for
code holding a raw session; a hand-rolled socket (a test fake, an adapter
over another WebSocket library) owes its callers a wait that actually ends
and an `Error::Timeout` when it does.

The completion-driven half has the same deadline (#130):
`co_await stream.Receive(std::chrono::seconds(2))` on an `AsyncEventStream`
resolves with the same four outcomes and the same rule — a timeout releases
the receive slot and leaves the session usable, and an event the wire
delivers after the deadline waits for the next await. One layer down it is
`WebSocket::ReceiveAsync(timeout, callback)`, a refusing default rather than
a pure virtual (the async family is opt-in), with the shared contract suite
holding every `SupportsAsync()` implementation to it.

Not every `ClientConfig` knob reaches a streaming dial — the upgrade GET is
not a unary request:

| ClientConfig knob | Event-stream dial |
|---|---|
| `endpoint`, `tls` | **Applies.** Host, port, and wss-vs-ws derive from the one endpoint. |
| `bearer_token` / `api_key` (the modeled auth traits) | **Applies.** Attached to the upgrade request like any unary request. |
| `user_agent` | **Applies.** Rides the upgrade request's headers. |
| `websocket_dialer` | **Applies.** Replaces the default Beast dial (the test seam). |
| `http_client` | **Not used.** Streams never touch the unary transport. |
| `interceptors` | **Not applied.** The upgrade bypasses the interceptor chain. |
| `retry` | **Not applied.** A failed or refused dial surfaces once; refusals are terminal. |
| `request_timeout_ms`, `max_idle_connections` | **Not applied.** Dial phases run under `WebSocketDialRequest::handshake_timeout_ms` (default 30 s); a live session idles under its `idle_timeout_seconds` (default 300, keep-alive pings underneath). |

The interceptor row is the trap worth naming: auth implemented as an
interceptor that stamps `authorization` onto unary requests never runs for
streams, and the dial goes out anonymous. Use the modeled auth traits
(`config.bearer_token` / `config.api_key`), or wrap the dialer to add
headers to the upgrade request:

```cpp
config.websocket_dialer = [](smithy::http::WebSocketDialRequest request) {
  request.headers.Set("authorization", "Bearer " + FetchToken());
  return smithy::http::BeastWebSocketClient::Dialer()(request);
};
```

The two dial-timeout knobs above live on `WebSocketDialRequest` with
production defaults; a wrapping dialer like the one shown can tighten them
per deployment the same way.

Server-side, mount the generated `StreamRouter()` on the transport
(`websocket_gate` = `Gate()`, `on_websocket` = `Serve()`; compose your own
admission refusals around `Gate()` — see
[server-guide.md](server-guide.md#serving-event-streams)). The hardening
notes above apply verbatim: upgraded sessions idle under
`websocket_idle_timeout_seconds`, and `Stop()` aborts live streams rather
than draining them (ADR-0015), so end streams application-side first when a
rollout needs grace. Every live stream also pins one handler-pool
thread for its whole lifetime (the serve callback blocks by design), so
size `handler_threads` at expected concurrent sessions plus unary
headroom — sixteen idle streams on the default pool starve everything
else.

## Browser clients

Browsers get their own wire and their own auth path (ADR-0018, issue
#113), because the `WebSocket` API constrains both: a page cannot produce
binary event-stream frames without a hand-written codec, and it cannot set
headers on the upgrade request at all.

**The wire** depends on the protocol. On `simpleRestJson` it is the
negotiated JSON-text mode: set
`BeastServerTransport::Options::websocket_accept_json_frames` and a page
that passes the subprotocol to the constructor speaks the stream with
`JSON.parse` alone — text frames carrying
`{"event": "<member>", "payload": {...}}`, `"exception"` in place
of `"event"` for the terminal error arm. Native clients are untouched
(no offer, binary wire, byte-identical 101). On `jsonRpc2` (ADR-0023)
there is nothing to negotiate: set
`Options::websocket_raw_text_frames`, and `new WebSocket(url)` with no
subprotocol at all speaks plain JSON-RPC 2.0 — a request envelope to
open, notifications both ways, a response envelope to end. The
[server guide](server-guide.md#browser-clients) has the two-line mount and
the JS loop for both.

**The blessed auth pattern is a short-lived, single-use ticket in an
`@httpQuery`-bound initial-request member.** The browser `WebSocket`
constructor cannot attach headers, so `@httpHeader`-bound members and the
`bearer_token`/`api_key` dial traits silently never reach a browser-dialed
upgrade — do not model browser-facing streaming auth as headers. Model it
as query:

```smithy
operation Converse {
    input := {
        @required @httpLabel room: String
        @required @httpQuery("ticket") ticket: String   // browser-reachable
    }
    ...
}
```

Mint the ticket with an authenticated *unary* operation over HTTPS (the
page can send `Authorization` headers on `fetch`), give it a lifetime of
seconds and one use, and validate it in a gate composed ahead of the
router's — admission control stays ahead of the 101, exactly like header
auth for native clients. On `jsonRpc2` the ticket cannot be a *modeled*
member at all — the protocol's contract is "no HTTP bindings apply", so
there is no `@httpQuery` to bind it to; put it in an **unmodeled** query
parameter on the upgrade URL (`new WebSocket(url + "?ticket=...")`) and
validate it in the same gate, which sees the raw upgrade request either
way. The exposure caveat below applies identically. The caveat, out loud: **query strings land in
access logs** — the transport's own, and every proxy's on the path (a
Caddy or nginx in front logs the full target of the upgrade GET). A
short-lived single-use ticket bounds that exposure to a token that is
worthless by the time it is written; a long-lived credential in a query
string is an incident, not a pattern — never put `bearer_token`-grade
secrets there. Cookies are the workable alternative when the page and the
service share a site (same-site topology; cross-origin needs deliberate
`SameSite=None; Secure` and pairs with the origin gate below against
cross-site WebSocket hijacking). First-message auth — an `authenticate`
event as the first stream message — is deliberately *not* blessed: it
moves auth past the gate, so admission control can no longer refuse before
the upgrade exists and every unauthenticated dial costs a live session and
a handler-pool thread. And do not smuggle tokens through
`Sec-WebSocket-Protocol`: that header is a negotiation channel (ADR-0018
now actively uses it), it is echoed into the 101, and proxies log it like
any other header — a token there is neither modeled, nor validated, nor
private.

**Browser-facing endpoints need an Origin allowlist.**
`smithy::server::RequireOrigin({"https://muchq.com"})` returns a
`websocket_gate` that refuses (403) upgrades whose `Origin` is present and
not listed — scheme + host + port exact — and admits requests with no
Origin header at all (non-browser clients don't send one; the attack this
stops cannot omit it). It is hijacking defense, not auth: compose it ahead
of the ticket gate and the router's. The end-to-end reference for all
three pieces — JSON wire, origin gate, and a native client beside them —
is [examples/chat/chat_browser_e2e_test.cc](../examples/chat/chat_browser_e2e_test.cc).

## Reconnect and resume

**Reconnect is a resume ticket plus `SessionRegistry::Resume` plus a
snapshot (ADR-0020)** — for every streaming client, not just browsers
(the ticket mechanics are the [browser auth pattern](#browser-clients)
above, which native clients may use too). Flaky mobile networks, page
reloads, and laptop lids make reconnect table stakes for session apps,
and the whole loop is made of pieces this guide already taught. Server
side, enable grace on the registry and split the handler's exits:

```cpp
smithy::server::SessionRegistry<RoomEvents>::Options options;
options.grace_period = std::chrono::seconds{300};
options.on_expired = [&](const std::string& id) {
  // The deferred cleanup: collect the game, tell the room. Runs exactly
  // once, off the handler threads (the registry's expiry thread, or the
  // Drain caller); mutually exclusive with a successful Resume.
};
...
// The handler's exit, split the ADR-0020 way:
if (left_deliberately) {
  registry.Remove(id);    // immediate — cancels any grace, never expires
} else if (!registry.Detach(id)) {
  registry.Remove(id);    // grace disabled or entry gone: the immediate path
}
```

The reconnect handshake is the ticket pattern again, aimed at resumption:
mint a **resume ticket** with the same authenticated unary (bound to the
session id this time), carry it on the reconnect upgrade's `@httpQuery`
member, validate it in the gate before any 101 exists. The handler then
tries `Resume(id, stream.Share())` — the identity-keyed atomic swap; it
succeeds only on a detached session within grace, exactly once, mutually
exclusive with `on_expired` — and on success sends the **current-state
snapshot as its first events** before normal traffic. On failure it falls
back to the fresh-join path (`Add`), because the session expired or never
existed. A reconnect can beat the old wire's failure notice, so admission
must retry briefly before refusing the id as a live duplicate — and that
whole dance is one registry call (ADR-0022) — the blessed admission call
every example makes:

```cpp
using Registry = smithy::server::SessionRegistry<RoomEvents>;

const auto admission = registry.ResumeOrAdd(
    id, [&stream] { return stream.Share(); }, std::chrono::seconds(1));
switch (admission) {
  case Registry::Admission::kResumed:  /* snapshot replay */ break;
  case Registry::Admission::kAdded:    /* announce the join */ break;
  case Registry::Admission::kRefused:  /* the id is live elsewhere */ break;
}
```

`mint` runs exactly once per attempt — the one fresh `Share()` serves the
Resume try and the Add try — with attempts every ~50ms, so a one-second
deadline (the old hand-rolled recipe's 20 × 50ms) comfortably covers the
failure-notice race. The call blocks up to the deadline — legal because
admission runs before the handler's first suspension, on the launching
thread. A `kRefused` you
*know* is wrong — a half-dead session whose wire never sent a FIN — has a
convergent answer now: `registry.Close(id)` kicks the old session — its
handler observes the close and runs the normal exit path, so the id is
admittable on the next dial (freed outright after a Remove exit;
parked-resumable after a Detach exit, where the redial resumes with the
old identity and gets the snapshot). Kicking stays the application's call;
`ResumeOrAdd` never does it on its own.

Say the posture out loud in your protocol docs, because it shapes client
code: **recovery is snapshot replay, not message replay.** ADR-0016's
"in-flight state is lost" stays true across reconnects — events sent
while detached are dropped by default; `Options::queue_while_detached`
retains a bounded tail if snapshots are expensive (bounded means bounded:
a full retained queue drops the overflow outright, with no slow-consumer
policy run), and applications needing stronger delivery own sequence
numbers at the protocol level.

Client side, redial is application logic at both ends — a browser writes
it in page JS regardless — and the worked native shape is a loop, not a
knob: back off with jitter and a cap; re-mint the resume ticket (the old
one is spent — single-use); re-dial with it; on success, treat the first
events as the snapshot and rebuild local state from scratch before
resuming normal handling; on a refusal, fall back to the fresh-join
handshake and tell the user their seat expired. `Drain` expires detached
sessions immediately, so a deploying server is never waiting out ghosts —
clients should treat a close during redial as "try the other host", not
"give up". Two end-to-end references drive the loop — abrupt kill, resume
with roster snapshot, grace expiry announcing the departure — as real
processes:
[examples/chat/async_hub_cli_test.sh](../examples/chat/async_hub_cli_test.sh)
on the generated-async-handler hub (ADR-0021), and
[examples/bazel-consumer/chat_reconnect_cli_test.sh](../examples/bazel-consumer/chat_reconnect_cli_test.sh)
on a fully generated server and clients through the module boundary.

## Server hardening

The production server transport (`BeastServerTransport`, ADR-0006) enforces
per-connection timeouts (`request_timeout_seconds`) and body- and header-size
limits (`max_body_bytes`, `max_header_bytes`) — over-limit requests are
answered with `413 Content Too Large` / `431 Request Header Fields Too Large`
and `Connection: close`, followed by a bounded lingering close (a few seconds
/ 256 KiB of drain) so the status stays readable; a client that streams past
the budget without reading may still see a reset, which is inherent to the
recipe. These rejections are written by the transport itself, before a
handler chain exists, so `Observe` middleware never sees them — set
`Options::on_rejected` to observe them (status, peer address, and whatever
the parser got to), wired to the same sink as your `Observe` callbacks.

The connections that die without any response are observable the same way
(ADR-0013): set `Options::on_connection_event` for TLS handshake failures
(a flood on the TLS port means something is sending plaintext there — a
misrouting load balancer), framing garbage, stalled requests
(`kReadTimeout`, the slowloris shape), and peers vanishing mid-request or
mid-response (`kDropped`), each with the peer, the transport's error text,
and time spent in the failing phase. Silence means healthy: clean
keep-alive closes, idle timeouts with nothing received, and shutdown
cancellations are deliberately not reported, so the signal does not scale
with healthy traffic. Wire it to the same sink as `on_rejected` and
`Observe`: with both hooks installed, every connection the transport
terminates is either accounted for or deliberately, documented-ly healthy.

Serving WebSocket event streams (ADR-0015) adds one kind and two
behaviors to know about: `kUpgradeFailure` reports upgrade handshakes
that failed after the gate admitted them (once a session is up, wire
failures surface to your serve callback through `Send`/`Receive` — the
application is the observer there); an upgraded connection's silence is
governed by `websocket_idle_timeout_seconds` (default 300, keep-alive
pings underneath) instead of `request_timeout_seconds`; and `Stop()`
*aborts* live stream sessions rather than draining them — an in-flight
stream gets no grace period, so end streams application-side first if
that matters to your rollout.

Concurrent connections are capped by `max_connections` (default 1024; 0
disables the cap): at the cap the server pauses accepting and new
connections wait in the kernel's listen backlog until a session closes, so
a connection flood cannot exhaust file descriptors or memory. Idle
keep-alive sessions still expire on `request_timeout_seconds`, so they
cannot pin the cap.

Handlers execute on their own pool (`handler_threads`, default 16; 0 runs
them inline on the io threads): a handler that blocks on a database or
downstream call cannot starve the `threads` io threads that accept
connections and read and write the wire, so already-computed responses keep
flowing even while every handler is blocked. Size `handler_threads` for
your handlers' blocking profile; handler implementations must be
thread-safe either way, as concurrent requests dispatch concurrently.

Responses are framed by the transport alone: any `content-length`,
`transfer-encoding`, or `connection` header set by a handler is dropped
rather than emitted beside the transport's own (a duplicate or conflicting
framing pair is the classic request-smuggling vector); both server
transports enforce this.

The transport terminates TLS when
`tls_certificate_chain_pem` + `tls_private_key_pem` are set (ADR-0007). The
TLS posture is fixed rather than configurable: TLS 1.2 minimum, ECDHE+AEAD
cipher suites for 1.2 (every 1.3 suite qualifies), and ALPN answering
`http/1.1` — a client that offers ALPN without `http/1.1` (say, h2-only) is
refused at the handshake rather than silently served a protocol it did not
agree to; clients that send no ALPN are unaffected. The client transport
enforces the same TLS 1.2 floor. Client-certificate (mTLS) verification is
tracked with the auth work (#90).

The transport drains on `Stop()`: new
connections and keep-alive reads cease immediately, while requests already
read off the wire get up to `drain_timeout_seconds` (default 10) to finish
writing their responses before the thread pool is torn down. `Stop()` is
itself bounded: a handler that never returns cannot wedge shutdown — past
the drain deadline plus a short grace (2 seconds; worst-case `Stop()` is
about `drain_timeout_seconds + 2s`, which is the number to budget in
`TimeoutStopSec`/`terminationGracePeriodSeconds`), the stuck worker is
abandoned with a `std::clog` trace (its thread and the transport's internal
state deliberately leak, since a thread cannot be killed safely) and
`Stop()` returns; if the handler ever does return, the abandoned reaper
finishes the cleanup in the background. See
[server-guide.md](server-guide.md).
