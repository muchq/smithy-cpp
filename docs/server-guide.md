# Server guide: implementing a generated service

Phase 4 generates a server counterpart for every service: `server.h`/`src/server.cc` with a
`<Service>Handler` interface and a `<Service>Server`, exposed as the module's `:server` Bazel
target.

## Implementing a handler

Handler implementations must be **thread-safe**: the production transport
(`BeastServerTransport`, ADR-0006) dispatches requests on a thread pool, so any mix of
operations can run concurrently against the one handler instance you pass to the server.
Guard shared state (the quickstart's in-memory handler shows the minimal mutex pattern).

```cpp
#include "example/weather/server.h"

class MyHandler final : public example::weather::WeatherHandler {
 public:
  smithy::Outcome<GetCityOutput> GetCity(const GetCityInput& input,
                                         const smithy::server::RequestContext& context) override {
    if (/* not found */) {
      smithy::Error error = smithy::Error::Modeled("NoSuchResource", "no city: " + input.cityId);
      error.set_detail(NoSuchResource{.resourceType = "City"});  // serializes the typed body
      return error;
    }
    std::clog << "GetCity from " << context.request->peer_address << "\n";  // or leave it unnamed
    return GetCityOutput{...};
  }
  // ... one method per operation
};
```

- **Request metadata**: the second parameter carries what the typed input doesn't model
  (issue #46). `context.request` is the raw `smithy::http::HttpRequest`: read unmodeled
  headers (`context.request->headers.Get("x-tenant")`), the transport-stamped client
  address (`context.request->peer_address`, `"ip:port"`, empty on the in-memory Loopback),
  or the request's `traceparent` — always present and parseable behind a transport, since
  the ingress mints a root context when the client sent none (ADR-0011); parse it with
  `smithy::http::ParseTraceparent` and `GenerateSpanId` (`smithy/http/trace_context.h`) to
  open child spans. `context.labels`
  and `context.query_params` hold the decoded routing captures. Handlers that need none of
  it leave the parameter unnamed. Behind a reverse proxy, derive the real client with
  `smithy::http::ClientAddress` over a `TrustedProxies` set (`smithy/http/forwarded.h`,
  ADR-0012) instead of reading `x-forwarded-for` yourself — the raw header is
  client-authored.

- **Modeled errors**: return `smithy::Error::Modeled("<ErrorShapeName>", message)`. The server
  maps the code to the shape's `@httpError` status (else 400/`@error("server")` → 500) and the
  protocol's error identity — simpleRestJson sets the neutral `X-Error-Type` header and serializes the
  detail as the body; rpcv2Cbor writes the fully qualified shape id as `__type` in the CBOR
  body; jsonRpc2 answers a JSON-RPC error object whose `code` is the `@httpError` status and
  whose `data` carries the members plus `__type`. Attach the typed structure with `set_detail()` to serialize its members (simpleRestJson
  `@httpHeader`-bound error members become response headers); the generic message is added
  only when the detail carries no message member of its own.
- **Validation/serialization errors** (including malformed request input the framework catches
  before your handler runs) map to 400; any other failure is a non-leaking 500
  `InternalFailure` carrying the request's trace id as `x-correlation-id` (ADR-0011) — a
  returned error correlates exactly like a thrown one.
- **A handler that throws** (rather than returning an `Error`) is contained by the transport
  layer: the exception is converted into a 500 whose `x-correlation-id` header is the
  request's trace id (ADR-0011), and the same id plus the exception's `what()` is written to
  `std::clog`. One throwing request fails alone — it never unwinds into the transport's I/O
  thread and terminates the process. Prefer returning `smithy::Error` for expected failures;
  the catch-all is a safety net, not a control path.

## Running a server

The server is transport-agnostic: `Handler()` returns a `smithy::http::RequestHandler` that
plugs into any `HttpServerTransport`:

```cpp
example::weather::WeatherServer server(std::make_shared<MyHandler>());

smithy::http::BeastServerTransport transport(options);  // production (ADR-0006)
transport.Start(server.Handler());
// or smithy::http::Loopback for in-process tests. (SocketHttpServer is test-only: one
// connection at a time, loopback only — its Start() says so on std::clog.)
```

Cross-cutting behavior (auth checks, request logging, metrics) wraps `server.Handler()` as
user-supplied middleware — `smithy::server::Chain` composes it outside the generated router,
and `smithy::server::Observe` is the built-in logging/metrics hook. See
[production-guide.md](production-guide.md).

Routing (method + URI pattern from `@http`, greedy labels, 404/405 with `Allow`),
request-binding deserialization (labels, query incl. `@httpQueryParams`, headers, JSON/CBOR
bodies), and response serialization (status, headers, body) are all generated; rpcv2Cbor
services check the `smithy-protocol` header and dispatch on the fixed
`/service/{Service}/operation/{Operation}` form. jsonRpc2 services register a single `POST /`
route and dispatch on the request envelope's `method` member, answering every well-formed
JSON-RPC call — success or error — as an HTTP 200 envelope (envelope-level failures use the
reserved JSON-RPC codes: -32700 parse, -32600 invalid request, -32601 method not found,
-32602 invalid params).

## Serving event streams

Event-stream operations (ADR-0016) grow the handler a streaming method — input first, the
typed session in the middle, context last — that blocks for the session's lifetime:
`Send`/`Receive` until done, then return `Unit` for a clean close, or an `Error`, which ends
the stream with one typed exception message before the close (a `Modeled` error with
`set_detail()` surfaces on the client exactly like a unary modeled error) — unless the
stream already terminated: propagating a failed `Receive()` closes without a message,
which that peer already observed. The `stream&` is valid only until the handler returns,
so join any helper thread still using it before returning — or hand such threads an
*owning handle* instead: `stream.Share()` returns an `EventStreamHandle<Out>` (issue
#112, ADR-0017), a cheap-copy value safe to hold beyond the return — copies are how a
session fans out, and all of them share one view of it. A handle sends and closes from
any thread while the session lives; once the handler has returned it fails softly with
`Error::Transport` — exactly what a closed stream reports — so nothing dangles and
nothing new can go wrong. The generated
server exposes `StreamRouter()`, a `smithy::server::WebSocketRouter` with every streaming
route registered; mount it on the transport in two lines — the upgrade path deliberately
bypasses the HTTP middleware chain (ADR-0015), so unary dispatch beside it is untouched:

```cpp
smithy::http::BeastServerTransport::Options options;
options.websocket_gate = server.StreamRouter()->Gate();  // 404/405 refusals pre-upgrade
options.on_websocket = server.StreamRouter()->Serve();   // blocking per-session dispatch
smithy::http::BeastServerTransport transport(options);
transport.Start(server.Handler());                       // unary routes, same port
```

A handler that must not park until the client speaks — one with periodic work, or a
watchdog of its own — bounds the wait instead: `stream.Receive(std::chrono::seconds(1))`
returns `Error::Timeout` (code `TimeoutError`) when the deadline passes with nothing to
report, on a session that is untouched and still usable, so the loop does its other work
and comes back. A clean close is still `nullopt` and a broken wire still a
`TransportError`; only the timeout spares the stream:

```cpp
while (true) {
  auto event = stream.Receive(std::chrono::seconds(1));
  if (!event.ok()) {
    if (event.error().code() != "TimeoutError") return smithy::Unit{};  // wire gone
    if (!PublishHeartbeat(stream)) return smithy::Unit{};
    continue;                                                           // quiet, not dead
  }
  if (!event->has_value()) return smithy::Unit{};                       // clean close
  /* handle **event */
}
```

Whatever method the operation models, upgrades are always GET — a WebSocket upgrade is a
GET on the wire, and the generated routes register accordingly: on the modeled URI for
the binding protocols, on the shared `/` endpoint for jsonRpc2 (ADR-0023), whose opening
envelope carries the routing instead.

Application admission policy (auth, rate limits) composes by wrapping `Gate()`: run your
refusal first, then defer to the router's (`smithy/server/websocket_router.h` shows the
pattern). Constraint validation also guards streaming inputs, with one wrinkle: the labels,
query, and headers arrive on the upgrade request, which the transport accepts *before* the
route parses them — so a validation failure surfaces as a successful dial whose first
`Receive()` is a terminal `SerializationException`, not as a refused upgrade.

Multi-client fan-out — "N connected players, the server pushes state to all of them" —
is `smithy::server::SessionRegistry<Out>` (issue #112, ADR-0017), a thread-safe map of
owning handles with a bounded outbound queue and a writer thread per session, so a
broadcast never stalls a room behind the slowest client's TCP window:

```cpp
smithy::server::SessionRegistry<RoomEvents> registry;
registry.Add(player_id, stream.Share());              // handler entry
registry.SendTo(player_id, event);                    // queued, non-blocking
registry.Broadcast(ids, [&](const auto& id) { return RedactFor(id); });  // per-recipient
registry.Remove(player_id);                           // handler exit (a late Remove is
                                                      // a soft bug, never a dangle)
```

When a session's queue is full the event is dropped and the slow-consumer policy runs:
the default disconnects the client (its handler observes the close and unwinds);
`Options::on_slow_consumer` keeps the policy with your application instead. Per-recipient
construction exists because broadcast-identical-bytes is the wrong primitive for
per-viewer state — the callback runs once per recipient, outside all registry locks.

Reconnects get a grace window on the same registry (ADR-0020): set
`Options::grace_period`, call `Detach(id)` on abrupt loss instead of `Remove`,
and `Resume(id, handle)` swaps a reconnecting connection into the parked entry
(`ResumeOrAdd` is the blessed admission call wrapping both, ADR-0022)
— `Options::on_expired` runs the deferred cleanup exactly once when nobody
comes back. The full handshake (resume ticket → gate → `Resume` → snapshot
replay) and the client redial shape live in the
[production guide's reconnect section](production-guide.md#reconnect-and-resume).

### Serving without a thread per session (ADR-0019)

Every session served through `on_websocket` parks one handler-pool thread for its
lifetime, so `handler_threads` caps concurrent streams. The completion-driven seam removes
that: set `Options::on_websocket_session` instead (exactly one of the two), receive the
session as a `std::shared_ptr<WebSocket>`, launch a
`smithy::eventstream::Detached` coroutine over
`smithy::eventstream::AsyncEventStream<Out, In>`, and return — the session lives until a
`Close`, the idle timeout, or `Stop()`:

```cpp
smithy::eventstream::Detached Serve(Hub& hub, std::string id,
                                    std::shared_ptr<smithy::http::WebSocket> socket) {
  smithy::eventstream::AsyncEventStream<Out, In> stream(std::move(socket), Encode, Decode);
  hub.registry().Add(id, stream.Share());              // the same handle, unchanged
  while (true) {
    auto event = co_await stream.Receive();            // parks no thread
    if (!event.ok() || !event->has_value()) break;
    hub.registry().Broadcast(...);                     // pushes go through the registry
  }
  hub.registry().Remove(id);
}  // stream destroyed here: closes the session and revokes its handles

options.on_websocket_session = [&hub](const smithy::http::HttpRequest& request,
                                      std::shared_ptr<smithy::http::WebSocket> socket) {
  Serve(hub, IdFor(request), std::move(socket));  // a Detached coroutine; returns immediately
};
```

Resumption runs on the transport's completion context (a Beast io thread): never block
there — blocking work belongs on your own threads, reached through `Share()`. The
coroutines reference the hub from io threads long after the launch callback returned, so
declare hub state (the registry included) before the transport and `Drain` before either
dies. Pair this with `SessionRegistry Options::async_delivery = true` and fan-out sheds
its writer threads too: each session's queue drains through
`EventStreamHandle::SendAsync` completion chains, same FIFO/policy/drain contracts, zero
registry threads (sessions on sockets without async support fall back to a writer thread
automatically). One interplay to know: the chain and a direct send (`co_await
stream.Send`, a raw handle send) share the socket's one send slot, and on the first
collision that session falls back to writer-thread delivery — nothing lost, but that
session thereafter costs the one thread async mode exists to avoid. So steady-state
pushes to a registered session belong in the registry, and direct sends in its serve
loop are for request/reply moments.

Multi-route servers mount the shared seam through `WebSocketRouter` exactly like the
borrowed one — `AddSession` routes with the same pattern grammar, then:

```cpp
options.websocket_gate = router.Gate();
options.on_websocket_session = router.ServeSession();
```

(one router serves one seam; `Add` and `AddSession` refuse to mix — a process that wants
both seams serves them from two transports, since a transport itself mounts at most one
dispatcher).

### Generated async streaming handlers (ADR-0021)

Everything above is the hand-mount shape — yours when the generator does not cover a
transport or a wire. For a generated service, the generator now emits the whole thing:
implement `<Service>AsyncHandler` — each streaming operation is a coroutine returning
`smithy::eventstream::StreamTask` over the operation's `<Op>AsyncServerStream&`, unary
operations keep their blocking signatures — construct `<Service>Server` with it, and
mount the session seam:

```cpp
class MyHandler final : public example::chat::ChatAsyncHandler {
  smithy::eventstream::StreamTask Converse(example::chat::ConverseInput input,
                                           example::chat::ConverseAsyncServerStream& stream) override {
    while (true) {
      auto event = co_await stream.Receive();
      if (!event.ok() || !event->has_value()) co_return smithy::Unit{};
      // ... co_await stream.Send(reply), registry fan-out via stream.Share() ...
    }
  }
  // ... Watch, ListRooms ...
};

example::chat::ChatServer server(std::make_shared<MyHandler>());
options.websocket_gate = server.StreamRouter()->Gate();
options.on_websocket_session = server.StreamRouter()->ServeSession();
```

Route matching, input parsing, envelope codecs, and refusal framing are all generated.
`co_return` an error and the generated wrapper ends the stream with the typed exception
message — exactly the blocking contract — and a coroutine that throws surfaces as the
never-leak `InternalFailure` instead of terminating. `input` arrives by value (the
coroutine's own copy; the upgrade request is gone by the first resumption), and there is
no `RequestContext` parameter: everything modeled rides the input — model needed headers
as input members and enforce identity/origin at the gate. Two unmodeled things the
blocking handler could read from `context.request` are genuinely out of reach on this
surface today: the peer address and unmodeled trace headers. A small owned launch-info
parameter can close that gap additively when a consumer needs it; until then those
belong in the gate. One server instance serves one seam — the constructor picks it; a
service that wants some streaming operations blocking and some async needs two server
instances on two transports, each implementing its full handler, so look for a per-route
knob no further. Execution contexts: code before the handler's first `co_await` runs on
the launching handler thread (the examples' blocking `ResumeOrAdd` admission is fine
there); every later resumption is a transport completion context — never block those,
and reach blocking work through `stream.Share()`. Shutdown reads the same as the
hand-mount: `registry.Drain(grace)` closes every session, each parked
`co_await stream.Receive()` completes with the close, the coroutine cleans up and
returns — and the Detached lifetime rules above (state outlives the transport; more
than one io thread when sessions mix with handle traffic) apply to generated handlers
verbatim. The thread-free chat hub is the working reference
([examples/chat/async_hub_server_main.cc](../examples/chat/async_hub_server_main.cc),
driven as real shell-commanded processes by `async_hub_cli_test.sh`), and the same
consumer script passes out of tree against both seams
(`examples/bazel-consumer/chat_async_reconnect_server_main.cc`).

Note `Stop()`'s semantics from ADR-0015: it *aborts* live stream sessions rather than
draining them, so a graceful rollout drains application-side first — now one line:
`registry.Drain(grace)` closes every session (each blocked handler wakes, returns, and
removes itself) and waits until the registry empties, after which `Stop()` has nothing
left to abort. The working reference is the chat hub
([examples/chat/hub_handler.h](../examples/chat/hub_handler.h)): rooms, per-viewer
redaction, watchers and talkers on one registry, and the SIGTERM → `Drain()` → `Stop()`
lifecycle, exercised end to end both in memory (`hub_e2e_test.cc`) and as real processes
driven by shell commands (`hub_cli_test.sh`).

Handlers are testable without any transport (or Boost): `InMemoryWebSocketPair` plus an
injected dialer runs the full generated client↔server path in memory. The chat example's
e2e fixture is ~15 lines:

```cpp
auto server = std::make_shared<ChatServer>(std::make_shared<MyHandler>());
smithy::ClientConfig config;
config.websocket_dialer = [&](const smithy::http::WebSocketDialRequest& request)
    -> smithy::Outcome<std::shared_ptr<smithy::http::WebSocket>> {
  auto [near, far] = smithy::http::InMemoryWebSocketPair::Create();
  smithy::http::HttpRequest upgrade;                 // what a transport would deliver
  upgrade.method = "GET";
  upgrade.target = request.target;
  upgrade.headers = request.headers;
  serve_threads.emplace_back([serve = server->StreamRouter()->Serve(), upgrade,
                              session = far] { serve(upgrade, *session); });
  return near;
};
auto client = ChatClient::Create(std::move(config));  // client->Converse(...) streams in memory
```

(Join the serve threads in teardown after `Close()`ing the far ends.) The full-duplex chat
example ([examples/chat/](../examples/chat/)) is the working reference:
a generated client and server streaming both directions over real WebSockets, plus this
in-memory wiring for Boost-free tests.

### Browser clients

A browser cannot speak the binary event-stream framing without a hand-written JS codec —
so for `simpleRestJson` services there is a negotiated JSON-text wire (ADR-0018,
issue #113). Two additions to the mount above make a service browser-ready:

```cpp
options.websocket_accept_json_frames = true;                     // negotiate the JSON wire
options.websocket_gate = [origin = smithy::server::RequireOrigin({"https://muchq.com"}),
                          router = server.StreamRouter()->Gate()](
                             const smithy::http::HttpRequest& request)
    -> std::optional<smithy::http::HttpResponse> {
  if (auto refusal = origin(request)) return refusal;            // hijacking defense first
  return router(request);                                        // then 404/405 routing
};
```

A page then needs zero codec — the subprotocol offer selects text frames carrying
`{"event": "<member>", "payload": {...}}` (`"exception"` in place of `"event"` for the
terminal error arm), same closed-union semantics, one event per message:

```js
const ws = new WebSocket("wss://example.com/rooms/lobby/converse?ticket=" + ticket,
                         "smithy.eventstream.v1+json");
ws.onmessage = (m) => { const { event, exception, payload } = JSON.parse(m.data); ... };
ws.send(JSON.stringify({ event: "message", payload: { text: "hi" } }));
```

Native clients are untouched: they offer nothing, get a headerless 101, and keep the
binary wire — the handler and everything above the socket speak `eventstream::Message`
either way and never learn which wire a session negotiated. Fail-closed transposes: on a
JSON session, *binary* frames and envelopes with unknown members fail the session exactly
as text frames fail a binary one. Only enable the flag for `simpleRestJson` services —
an rpcv2Cbor event cannot ride a text frame, and a negotiated session refuses it on the
first `Send`.

`RequireOrigin` compares scheme + host + port exactly (default ports resolved) and admits
requests with *no* Origin header — non-browser clients don't send one, and the attack it
stops (a hostile page driving a victim's browser) cannot omit it. It is
cross-site-WebSocket-hijacking defense, not authentication: browsers cannot set upgrade
headers, so `@httpHeader`-bound members and the header auth traits never reach a
browser-dialed upgrade — model browser-facing auth as an `@httpQuery` member and see the
[production guide](production-guide.md#browser-clients) for the blessed ticket pattern
and its caveats. The working reference is
[examples/chat/chat_browser_e2e_test.cc](../examples/chat/chat_browser_e2e_test.cc): a
browser-fidelity peer conversing in JSON text beside a binary generated client, the
origin gate refusing a foreign page, and the modeled `Kicked` error arriving as an
`"exception"` envelope.

### jsonRpc2 streams (ADR-0023)

`jsonRpc2` streams speak JSON-RPC itself — text envelopes end to end on the protocol's
shared endpoint, no smithy-specific framing, no subprotocol. The generated server
registers one `GET /` stream route per seam and dispatches on the **opening request
envelope's `method`** (exactly how the unary `POST /` routes); the opening call's
`params` carry the operation's initial-request members, events travel as notifications
in both directions echoing the opening id inside `params`, and the stream ends with a
response envelope for the opening id — `result` on a clean handler return, the unary
error object for a modeled error, the reserved codes (-32700/-32600/-32601/-32602) for
envelope-level failures — then the close. Handlers, `StreamRouter()`, both serve seams,
and everything above the socket are unchanged; the mount needs one flag beyond the
two-liner:

```cpp
options.websocket_gate = server.StreamRouter()->Gate();
options.on_websocket_session = server.StreamRouter()->ServeSession();  // or on_websocket
options.websocket_raw_text_frames = true;  // the JSON-RPC text wire (ADR-0023)
```

A page needs even less than the negotiated wire above — no subprotocol token:

```js
const ws = new WebSocket("wss://example.com/?ticket=" + ticket);
ws.send(JSON.stringify({ jsonrpc: "2.0", method: "Accumulate",
                         params: { start: 10 }, id: 1 }));
ws.onmessage = (m) => { const { method, params, result, error } = JSON.parse(m.data); ... };
ws.send(JSON.stringify({ jsonrpc: "2.0", method: "add",
                         params: { id: 1, payload: { value: 5 } } }));
```

The raw-text flag claims the whole listener (it is refused beside
`websocket_accept_json_frames`), so serve jsonRpc2 streams from their own transport. A
vanilla JSON-RPC 2.0 client that ignores notifications still observes one well-formed
call → response pair. The wire is pinned by the authored suite in
[protocol-tests/jsonrpc2/stream_conformance_test.cc](../protocol-tests/jsonrpc2/stream_conformance_test.cc)
(normative, per the ADR-0002 precedent); the worked example is the calculator's
`Accumulate` session ([examples/jsonrpc2](../examples/jsonrpc2)), over the in-memory
pair and real Beast WebSockets, both seams, TLS included.

## Redirects (3xx)

A redirect is a modeled operation like any other: bind `Location` with `@httpHeader` and let
the status come from either the `@http` trait or an `@httpResponseCode` member.

```smithy
/// Fixed status: it rides the @http trait. @suppress is required — Smithy's own
/// HttpResponseCodeSemantics validator fails the model with "Expected an `http`
/// code in the 2xx range, but found 302", which stops model assembly.
@readonly
@suppress(["HttpResponseCodeSemantics"])
@http(method: "GET", uri: "/r/{slug}", code: 302)
operation Resolve {
    input := {
        @required
        @httpLabel
        slug: String
    }
    output := {
        @required
        @httpHeader("Location")
        location: String
    }
}

/// Status chosen per request — 301 for a retired slug, 302 for a live one.
/// @httpResponseCode carries it, so the modeled code stays 200 and no
/// suppression is needed.
@readonly
@http(method: "GET", uri: "/d/{slug}")
operation ResolveDynamic {
    input := {
        @required
        @httpLabel
        slug: String
    }
    output := {
        @required
        @httpResponseCode
        status: Integer

        @required
        @httpHeader("Location")
        location: String
    }
}
```

Prefer the fixed form when the status never varies: it needs the suppression, but the status is
then part of the contract rather than something the handler can get wrong. Reach for
`@httpResponseCode` when the handler genuinely chooses (permanent vs temporary).

Generated clients treat 3xx as success, so a redirect arrives as a typed output carrying
`location` — not as an error. Bodies follow RFC 9110: 1xx, 204, and 304 responses are sent with
no body and no `Content-Type`, and every other status keeps the protocol's rule that a server
always sends a JSON body (at minimum `{}`). A 3xx *may* carry a body, so redirects get the
`{}` — harmless to browsers, which read `Location`.

`examples/bazel-consumer/redirect_e2e_test.cc` drives both spellings end to end, including
raw-socket assertions on the bytes.

## Generated smoke tests

Every generated module ships `tests/smoke_test.cc` (target `:smoke_test` in the module's
`tests/` package): the generated client calls the generated server over the loopback transport
— every operation round-trips a minimal valid value and one test proves modeled-error mapping.
It passes out of the box and is the natural place to start testing a real handler. Streaming
operations are skipped by the generated smoke/integration suites (a unary round trip has no
meaning for a session; their handler overrides are close-immediately stubs) — drive them the
way the chat example's e2e tests do.

## Constraint validation

Inputs are validated against the model's constraint traits after parsing and before your
handler runs: `@required` (top-level body/query/header members), `@length` (strings count
Unicode code points), `@range`, `@pattern`, `@uniqueItems`, and enum membership, recursively
through structures, unions, lists, and maps. Failures never reach the handler — the server
responds with the standard 400 `ValidationException` wire shape (`message` summary plus a
`fieldList` of per-member `{path, message}` entries, JSON-pointer paths like `/list/0`), with
the exact message formats the official validation conformance suite pins. `@internal` enum
members stay accepted on the wire but are omitted from the advertised value set.

`@pattern` evaluates on a linear-time NFA engine (`smithy/core/regex.h`), so no pattern/input
combination can backtrack catastrophically — the classic ReDoS pattern `^([0-9]+)+$` validates
request-sized inputs in microseconds instead of hanging the dispatch thread. The engine covers
the ECMA-262 subset Smithy patterns use; backreferences and lookaround (inherently
backtracking constructs) are rejected at generation time with an error naming the shape and
the fix.

## Conformance

Generated servers pass the official server-mode `httpRequestTests`/`httpResponseTests` suites
(across the alloy simpleRestJson and rpcv2Cbor suites) plus the `httpMalformedRequestTests` parser-strictness suite, all with a documented must-shrink exclusion list. Wire
requests parse into the exact modeled inputs — including `@httpPayload` (blob/string/enum raw
bodies, structure/union/document JSON bodies) and `@httpPrefixHeaders` — malformed ones are
rejected with the exact error identity (strict booleans, bounds-checked integers, strict
timestamps, single-member unions, `SerializationException`/`UnsupportedMediaTypeException`/
`NotAcceptableException` headers), and handler outputs/errors serialize to the exact wire
responses — `@httpResponseCode`, bodies suppressed on the statuses RFC 9110 forbids one on
(1xx, 204, 304), Content-Type (415) and
Accept (406) enforcement (blob payloads without `@mediaType` accept anything), and
all-query-params `@httpQueryParams` maps. Ambiguous route tables fail at generation time.
Routes for `@requestCompression` operations transparently gunzip request bodies arriving with
`Content-Encoding: gzip` (decompression is size-capped against bombs; malformed gzip is a 400
serialization error) — see [production-guide.md](production-guide.md).

## Not yet generated (Phase 5+)

Nested `@required` absences as `fieldList` entries and a server-strict serde variant (clients
must skip null dense-map values and accept UTC-offset timestamps in responses; servers share
that serde today), and `@streaming` blob payloads (see
[Current limitations](../README.md#current-limitations)). Event-stream operations generate
streaming handlers and a `StreamRouter()` (ADR-0016).
