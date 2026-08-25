# Changelog

All notable changes to smithy-cpp are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/); versions follow the
policy in [docs/versioning.md](docs/versioning.md).

## [Unreleased]

### Fixed

- **Numeric wire values no longer truncate into generated narrow types**
  (#109). Three holes in the otherwise-uniform range-check posture: an
  `intEnum` member in a document body cast the raw wire int64 straight into
  its `int32`-backed `enum class`, so 2^32+2 silently aliased onto a valid
  enumerator (byte / short / integer members already rejected out-of-range
  values, and intEnum *text* bindings were already bounded — the body path
  now shares the same check); a `float` member cast the parsed double
  unchecked on both the body and text-binding paths, so a finite wire value
  beyond float range (`1e300`) was undefined behavior per [conv.double] —
  the new `smithy::FloatFromDouble` rejects it while NaN/±Infinity still
  pass; and
  the jsonRpc2 client truncated `error.code` to `int` *before* its 100–599
  range test, classifying 2^32+404 as HTTP 404 (with 5xx codes wrongly
  marked retryable) — codes are now classified on the full int64.
- **Modeled redirects: a 3xx `@httpResponseCode` is a success, not an error**
  (#184). Generated clients tested `status < 200 || status > 299` when the
  output bound `@httpResponseCode`, so every modeled redirect came back as a
  `GenericError` with its `Location` binding discarded — while the
  static-`@http(code:)` branch of the same generator accepted one, because it
  compares against the modeled status. The window is now "not an error status",
  which makes the two spellings agree and removes the `CustomCodeOutput`
  conformance exclusion.
- **Bodiless statuses no longer carry content** (#184). Only 204 was
  special-cased, so a 304 (reachable through `@httpResponseCode`) went out with
  `content-type: application/json` and a `{}` body — a response RFC 9110
  forbids content on, and one that misleads every cache in the path. 1xx, 205
  (§15.3.6) and 304 now join 204, and the rule covers `@httpPayload` responses
  as well as document bodies: the payload branch returned before the old check
  was reached, so a payload-bearing operation shipped its payload on a 304 (and
  on a modeled 204, which predates this change). With `@httpResponseCode` the
  check is emitted at runtime as `helpers::StatusAllowsContent`, since the
  status is not known at generation time. 3xx is deliberately not included: a
  redirect *may* carry content, and alloy's `CustomCodeOutput` case pins `{}` at
  status 399.

### Added

- **Servers validate intEnum membership** (#109). String enums already
  failed request validation outside the modeled value set; intEnum members
  were accepted silently. They now produce the same suite-exact
  `ValidationException` message
  (`Member must satisfy enum value set: [1, 2]`), with `@internal` members
  staying wire-valid but unadvertised — string-enum policy throughout.
  Clients deliberately keep
  unknown-but-in-range values for forward compatibility, and the asymmetry is
  pinned by `examples/roundtrip/rest/numeric_bounds_wire_test.cc`.
- **Redirects are documented and covered** — a "Redirects (3xx)" section in
  [docs/server-guide.md](docs/server-guide.md) covering both spellings and the
  `@suppress(["HttpResponseCodeSemantics"])` a static non-2xx `@http` code
  needs (Smithy's own validator stops model assembly without it), plus
  `examples/bazel-consumer/redirect_e2e_test.cc`: both spellings end to end
  over loopback and a real socket, with raw-socket assertions on the bytes a
  browser actually reads.

## [0.2.0] - 2026-08-02

Event-stream hardening: a terminal transition with both an async send and an
async receive parked no longer deadlocks the completing thread, and the
transport-neutral half of the ADR-0019 contract now runs as one shared suite
against every `WebSocket` implementation, in-repo and out. Additive for the
public runtime headers — existing implementors are unaffected.

### Added

- **`WebSocket::TerminalWaiters`** — the parked ADR-0019 completions a
  session's terminal transition takes, plus the one safe order to run them
  in. Its `Fire` is rvalue-only and completes the send before the receive,
  so the rule below cannot be restated wrongly (or forgotten) by the next
  implementation; both in-repo transports now take and fire through it.
  Additive: existing `WebSocket` implementors are unaffected.
- **A shared WebSocket contract suite**
  (`smithy/testing/websocket_contract_test.h`) — the transport-neutral half
  of ADR-0019 as a type-parameterized gtest suite each transport
  instantiates with a small driver. Both transports carried the identical
  terminal-ordering bug precisely because their suites mirrored each other
  by hand; anything an implementation must do regardless of its wire now
  lives in one body that runs against all of them. Published as the
  `websocket_contract_test_support` target, so an out-of-tree `WebSocket`
  can be held to the same contract; the consumer module implements one and
  does exactly that. Four implementations run it today — both transports,
  the `JsonRpcStreamSocket` decorator, and the consumer's.

### Fixed

- **Event streams: a terminal transition no longer deadlocks the completing
  thread** (#173). When a session ended with both an async receive and an
  async send parked — a peer close or reset arriving while a registry
  fan-out write was in flight — the transports fired the receive completion
  first. That resumed the session loop inline, and `~AsyncEventStream`'s
  revocation drain then waited for a pin held by the send completion the
  same transition had already taken, sitting one frame below on that very
  thread: unreachable by `Close()` and by every other io thread, so the
  thread parked forever (with the Beast default of four io threads, four
  such events take the process out). Both transports now fire the send
  waiter first — it releases its pin and returns — and the receive last.
  Affects `BeastServerTransport`/`BeastWebSocketClient` sessions and
  `InMemoryWebSocketPair`.

## [0.1.0] - 2026-07-30

The first release: a vendor-neutral Smithy → C++ code generator, the runtime
it targets, and Bazel-native consumption — with every generated surface
conformance-tested and integration-tested in CI. Consumers pin the `v0.1.0`
tag via `git_override` / `archive_override`; Bazel Central Registry and Maven
Central publishing remain deferred (see [docs/versioning.md](docs/versioning.md)).

### Protocols

- **`alloy#simpleRestJson`** (REST/JSON): the full HTTP binding surface —
  labels (incl. greedy), query, headers, prefix headers, payloads
  (JSON-encoded string payloads per alloy's conformance model),
  `@httpResponseCode`, content negotiation — with neutral `X-Error-Type`
  error identity and status-code fallback. Green against alloy's official
  conformance suite (documented, shrinking exclusion list).
- **`smithy.protocols#rpcv2Cbor`** (RPC/CBOR): green against the official
  Smithy conformance suite, including the recursion and defaults cases.
- **`smithy.cpp.protocols#jsonRpc2`** (RPC/JSON): JSON-RPC 2.0 over a single
  POST endpoint, defined in-repo with an authored conformance suite; interop
  pinned against hand-rolled JSON-RPC peers. Event streams speak JSON-RPC
  itself (ADR-0023): text envelopes on the shared endpoint — no
  smithy-specific framing — with their own authored stream conformance
  suite.

### Generator

- Types (structs, unions over `std::variant`, enums with unknown-value
  preservation, intEnums), Document-pivot serde, clients, servers, generated
  BUILD files, smoke tests, and seeded random integration suites per fixture.
- `@default` population (including `@input` client-skip/server-fill semantics
  and `@required @default` evolution leniency), boxed recursion via
  `smithy::Boxed<T>`, `@required` response headers, `@idempotencyToken`
  auto-fill, `@paginated` paginators, gzip `@requestCompression`,
  constraint validation with suite-exact `ValidationException` output.
- Consumer Bazel rules (`smithy_cpp_{types,client,server}_library`) run the
  generator hermetically inside the build graph; out-of-tree consumer module
  tested in CI on Linux/macOS.
- Generated handler methods take the request metadata alongside the typed
  input: `Op(const OpInput&, const smithy::server::RequestContext&)`, where
  the context carries the raw request (unmodeled headers, the inbound
  `traceparent`, the transport-stamped peer address) plus the decoded routing
  captures (issue #46). **Breaking** for commit-pinning consumers: existing
  handler implementations add the parameter (unnamed when unused).

### Runtime

- Client TLS verification gains its two missing negative tests. Both pin
  properties that were previously only ever asserted to be *configured*, never
  observed to bite, so a silent weakening — in our code or underneath us in a
  BoringSSL bump — would not have failed a single test. (1) Hostname
  verification: a server presenting a certificate the client explicitly trusts
  as its own root, but whose SAN covers another host, must be refused; the
  existing untrusted-CA test cannot reach this, since it fails at chain
  validation instead. A second self-signed test identity
  (`kMismatchedNameCertificatePem`, `CN=other.example.com`) makes the name
  check the only thing left that can reject the handshake. (2) The client's
  TLS 1.2 floor: a hand-built listener capped at TLS 1.1 must be unreachable
  even with a fully trusted certificate — the mirror of the server-side floor
  test the suite already had — with a TLS 1.2 control proving the refusal is
  the floor and not a broken fixture. Both were verified by mutation: removing
  the hostname check, and removing the client floor, each fails exactly its
  own test.
- Exception containment and the `-fno-exceptions` gate (issue #109, ADR-0003):
  no exception may cross a smithy `Outcome` boundary or unwind a transport io
  thread. Two changes reconcile that contract with ADR-0009's fail-fast
  posture. (1) Composition-time misuses that threw `std::invalid_argument` no
  longer throw. Programming errors — a null rate-limit policy, a null
  health-check probe or a JSON-corrupting check name, a null observe sink —
  `Fatal`-abort with the same message (ADR-0009); they are not conditions a
  caller can handle. A malformed proxy-trust CIDR is instead recoverable
  config (like a bad bind address), so `TrustedProxies` gains a
  `Parse() -> Outcome<TrustedProxies>` factory returning `Error::Validation`
  (the throwing constructor is removed). Either way the `throw` is gone, so
  the paths compile without exceptions. (2) Wire-facing callbacks in the
  `-fno-exceptions`-clean runtime (request handlers, readiness probes, metrics
  sinks) run inside a new `smithy::internal::Contain`
  (`smithy/core/exception_guard.h`), which compiles to a direct call under
  `-fno-exceptions`. The Beast transport — which cannot build `-fno-exceptions`,
  so it always has exceptions — carries its own containment: each background
  `io_context::run()` is wrapped in a catch-and-re-enter backstop (a stray throw
  drains remaining work instead of terminating the process), its handler-pool
  posts and WebSocket completion callbacks are contained at the call, and its
  `Send`/`Start`/`FromConfig` Outcome boundaries are guarded against
  resource-construction failures (a `std::bad_alloc`, a `std::thread` ctor
  failing under load). A dedicated CI leg (`bazel build --config=noexcept`,
  `make noexcept`) now builds the dependency-light runtime with exceptions
  disabled — the ADR's enforcement gate — with `//runtime:http_beast` and its
  Boost-dependent subtree out of scope.
- Request-line injection defense (issue #109): the client transports now
  reject a space, control byte (CR/LF included), or DEL in `HttpRequest`'s
  `method` and `target` before dialing — the request-line companion to the
  header defense below. Both clients spliced those fields straight into
  `"METHOD SP TARGET SP HTTP/1.1"` (the socket client by concatenation,
  Beast via `target()`/`method_string()`, neither of which rejects CR/LF),
  so a raw CR/LF in a hand-built target smuggled a second request — verified
  live: the Beast client had been *sending and echoing* such a request 200.
  A method is a token and a target is percent-encoded origin-form, so a
  conformant value is unaffected; colon and other legal target characters
  still pass (unlike a header name). `ValidRequestLineField` (in `:http`)
  gates both `Send` paths — and the WebSocket dial, whose upgrade-GET target
  reaches the wire through Beast's equally-unvalidating handshake `target()`
  — with `Error::Validation`, before any bytes reach the wire.
- Outbound header-injection defense (issue #109): every transport write path
  now rejects control bytes in header names and values before they reach the
  wire, closing an HTTP response/request-splitting hole. A handler or
  middleware echoing untrusted data into a header (a `Location`, a request-id
  echo) could previously smuggle `CR`/`LF` and forge extra headers or a
  second message — Beast's `fields::insert` and the socket transports'
  string concatenation both passed the bytes through verbatim, even though
  the same code already strips the framing headers for the sibling
  smuggling vector. `ValidHeaderName`/`ValidHeaderValue`/`FindUnsafeHeader`
  (in `:http`, RFC 9110-aligned: names are control/space/colon/DEL-free
  tokens, values additionally permit HTAB and obs-text) gate all five
  paths — Beast server response, Beast client request, Beast WebSocket
  upgrade, and the socket client/server. Server responses with an unsafe
  header become a plain synthesized 500 (nothing echoed); client sends and
  WebSocket dials fail with `Error::Validation` naming the header, before
  anything is written. The permissive `Headers` container is unchanged
  (inbound parsing stores into it too); enforcement lives at the transports'
  write authority.
- `Timestamp::Format(kEpochSeconds)` renders negative fractional instants
  correctly (issue #109): floor-division formatted −500 ms as `-1.5`, which
  the (correct) parser read back as −1500 ms — every pre-1970 instant with
  a nonzero millisecond part was mis-rendered and round-trip-broken on the
  epoch-seconds string paths (HTTP label/query/header bindings; JSON and
  CBOR bodies were unaffected, being numeric). Sign and magnitude now
  format separately, and `Format(kEpochSeconds)` is now total over the
  unchecked int64 domain — the civil decomposition (whose day arithmetic
  overflows near the int64 edge, a pre-existing UB for absurd unchecked
  instants) runs only in the arms that need it, pinned at the extremes
  under UBSan. The `kHttpDate` weekday/month tables become `const char*`
  so their `%s` use no longer leans on literal-backed `string_view` NUL
  termination. The differential suite gains an epoch-seconds sweep (both
  signs, fractional milliseconds — the domain gap that let the original
  bug through), and `//fuzz:timestamp_fuzz` fuzzes all three wire-facing
  parsers with reformat/reparse fixed-point checks.
- Gzip feeds zlib in bounded slices (issue #109): `avail_in` is 32-bit, and
  the old single feed truncated the length silently — a 4 GiB + N byte
  `@requestCompression` body compressed to a *valid* gzip stream of its
  first N bytes, corrupting large uploads with no error anywhere (4 GiB
  exactly compressed to the empty stream). Both directions now re-feed
  input in bounded slices with no size cliff; the decompress side's
  trailing-garbage check now judges the whole input rather than its
  truncated 32-bit view. Teardown is RAII on every path, and `ZLIB_CONST`
  replaces the `const_cast`s. The slice bound is test-parameterized
  (`gzip_test_peer.h`, with the bound's validity enforced fatally at the
  seam), so the re-feed loop is pinned by kilobyte fixtures instead of
  4 GiB ones — and fuzzed: `//fuzz:gzip_fuzz` holds slice-fed and whole-fed
  gzip to identical verdicts and bytes on arbitrary inputs, alongside the
  compress→decompress round trip.
- `smithy::Document` pivot, JSON (nlohmann-backed) and hand-rolled CBOR
  codecs (RFC 8949 vectors + fuzzers), `Outcome`/`Error` model, retries with
  full-jitter exponential backoff, client interceptors, server middleware,
  W3C trace-context helpers, `@httpBearerAuth`/`@httpApiKeyAuth` wiring.
- Transports: in-memory loopback and dependency-free socket client/server
  for tests and simple deployments; **Boost.Beast production transports both
  directions** — `BeastServerTransport` (thread pool, keep-alive, timeouts,
  size limits with the 413/431 rejections observable via
  `Options::on_rejected`, graceful drain, TLS termination) and `BeastHttpClient`
  (keep-alive connection pool, per-request timeouts, TLS via BoringSSL with
  certificate + hostname verification on by default).
- Connections the transport terminates without a response are observable
  (ADR-0013): `BeastServerTransport::Options::on_connection_event` reports
  TLS handshake failures (handshakes that went wrong, not probe non-starts),
  framing garbage, stalled reads (the slowloris shape), and mid-stream
  drops, each with the peer, the error text, and phase-elapsed time — while
  clean closes (with or without TLS close_notify), idle reaping, and
  shutdown stay deliberately silent. Each wire phase now gets its own
  `request_timeout_seconds` budget, so a handler outrunning the read
  deadline's residue no longer has its response cancelled.
- Server trace identity minted at transport ingress (ADR-0011): a valid
  inbound `traceparent` continues verbatim; an absent or malformed one is
  replaced with a fresh root context, so `Observe`'s `trace_parent` always
  parses and any 5xx leaving the handler chain — returned or thrown —
  carries the request's trace id as `x-correlation-id` (a handler-set id
  wins).
- Proxy-aware client identity (ADR-0012): `smithy::http::ClientAddress`
  derives the real client behind a reverse proxy from `peer_address` and
  `x-forwarded-for` — anchored at the L4 peer, walking rightmost-untrusted
  against a `TrustedProxies` CIDR set, so a spoofed entry from outside the
  trust boundary never wins; the no-proxy topology is the explicit
  `TrustedProxies::None()`, never a default constructor (issue #104).
  `smithy::server::PerClientRateLimit` ships the derivation-into-admission
  wiring (the pluggable `allow(client)` policy sees only the derived key;
  underivable requests are admitted), and `DeriveClient` reports each
  address's derivation `Source` so a drifted trust boundary shows up on a
  dashboard instead of as a silent one-bucket collapse. The production
  guide teaches the composed middleware plus the `TRUSTED_PROXY_CIDRS`
  plumbing convention.
- Server middleware additions for production serving: `Guard` admission
  control (allowlists, maintenance mode — policy stays an application
  dependency) with a `TooManyRequests` reject factory,
  `HealthEndpoint` static liveness, and an optional `Observe` `on_start`
  callback for in-flight gauges with guaranteed start/complete pairing.
  **Breaking:** `Observe(callback, now)` call sites become
  `Observe(callback, nullptr, now)`.
- The browser wire for event streams (ADR-0018, issue #113): a client
  that offers the `smithy.eventstream.v1+json` WebSocket subprotocol on a
  server with `Options::websocket_accept_json_frames` set gets text
  frames carrying a JSON envelope — `{"event": "<member>",
  "payload": {...}}`, `"exception"` for the error arm — so a page speaks
  a generated simpleRestJson stream with `JSON.parse` alone: no codec, no
  build step. Native clients keep the binary wire (no offer, headerless
  101, byte-identical to before); the translation is transport-internal
  (`//runtime:eventstream_json`, `EncodeJsonFrame`/`DecodeJsonFrame`), so
  `EventStream`, the routers, `SessionRegistry`, and generated code are
  untouched; and the fail-closed posture transposes — binary frames and
  unknown envelope members fail a JSON session the way text fails a
  binary one. `BeastWebSocketClient::Options::offer_json_frames` offers
  the mode client-side for parity and tests (silent binary fallback when
  not accepted; a server selecting an unoffered subprotocol fails the
  dial). `smithy::server::RequireOrigin({...})` is the composable
  Origin-allowlist gate browser-facing endpoints need (scheme + host +
  port exact; absent Origin admitted — hijacking defense, not auth), and
  the production guide now names the blessed browser auth pattern
  (short-lived single-use tickets in an `@httpQuery` member, validated by
  the gate before the 101) with its caveats said out loud.
- Bounded receives on event streams (issue #128): `WebSocket::Receive` and
  `EventStream::Receive` grow a `std::chrono::milliseconds` overload, so a
  consumer waiting for a message the peer never sends gets a verdict
  instead of a hang — the case that previously burned a CI job's whole
  timeout with no assertion output. The deadline is a fourth outcome,
  `Error::Timeout` (code `TimeoutError`), distinct from the peer's clean
  close (`nullopt`) and from a failed session, and it is the one failure
  that spares the stream: unlike `Close()` — until now the only way to
  unblock a parked receive — the session stays usable, so the caller can
  assert, send, or wait again. Both in-repo transports honor it (`wait` →
  `wait_for` on the wait they already had; the one-outstanding-receive slot
  releases exactly as a completed receive releases it), and the delegating
  sockets and typed streams pass it straight through. The overload is
  **pure virtual** rather than defaulted: nothing the base class can reach
  bounds a wait (wrapping the blocking `Receive()` in a thread or the async
  twin leaves a parked receive holding its slot and eventually eating a
  message with no caller left to take it), so a default could only have
  ignored the deadline and blocked forever — the exact failure this
  overload exists to prevent, delivered to the one caller who asked for a
  bound. **Breaking** for commit-pinning consumers that implement
  `smithy::http::WebSocket` themselves (a hand-rolled test fake, an adapter
  over another WebSocket library): add the overload — honor the deadline
  and return `Error::Timeout`, or delegate to what you wrap. Consumers
  using the runtime's own sockets (`InMemoryWebSocketPair`, the Beast
  transports, an injected `websocket_dialer` returning either) are
  unaffected.
- Completion-driven event streams (ADR-0019) — the async adapter ADR-0014
  through ADR-0017 name as future work, runtime slice: `WebSocket` grows
  `ReceiveAsync`/`SendAsync`/`SupportsAsync` (one outstanding per class,
  completions on the transport's completion context; native on the Beast
  sessions and the in-memory pair, polite refusals by default so custom
  sockets keep working). `BeastServerTransport::Options::on_websocket_session`
  is the shared-session sibling of `on_websocket`: the callback owns the
  session and returns immediately, so a stream no longer parks a
  handler-pool thread — `handler_threads` returns to sizing unary work.
  `smithy::eventstream::AsyncEventStream<Tx, Rx>` + `Detached` put
  `co_await` where the blocking facade parks a thread (same terminal
  semantics as `EventStream`, same `Share()` handles), and
  `SessionRegistry Options::async_delivery` drains each session's queue
  through `EventStreamHandle::SendAsync` completion chains instead of
  writer threads (per-session fallback for blocking-only sockets). The
  thread-free chat hub (`examples/chat/async_hub_*`) serves the same
  generated wire through the new seam, driven as real shell-commanded
  processes. Generated handler/client surfaces stayed blocking in that
  slice — the handler half of the coroutine lift landed as ADR-0021
  (above); the generated client stays blocking.
- Streaming routes on the shared seam (issue #118): `WebSocketRouter` grows
  `AddSession`/`ServeSession()`, the `on_websocket_session` parallel of
  `Add`/`Serve()` — same pattern grammar, precedence, conflict rules, and
  seam-agnostic `Gate()`, with the winning route receiving the session as
  an owner (a launch point, not a serve loop). One router serves one seam:
  the transport mounts at most one dispatcher, so `Add` and `AddSession`
  refuse to mix rather than deaden routes silently. The thread-free chat
  hub now mounts its Converse route through the router instead of a
  hand-rolled target parser.
- jsonRpc2 event streams (ADR-0023, issue #123): the one refusing protocol
  now streams, on the JSON-RPC-native wire — one WebSocket upgrade on the
  protocol's shared `/` endpoint, every frame a text JSON-RPC 2.0 envelope.
  The opening request envelope selects the operation and carries the
  initial-request members in `params` (the first protocol with body-bound
  initial members, realizing ADR-0016's reserved seam); events are
  notifications in both directions echoing the opening id inside `params`
  (the `eth_subscribe` shape); the stream ends with a response envelope for
  the opening id — `result` on clean completion, the unary error-object
  convention unchanged for modeled errors, the reserved codes (-32700,
  -32600, -32601, -32602) for envelope-level failures — then the close.
  Mid-stream envelope violations (unparseable text, request or response
  envelopes after the opening call, foreign-id echoes) are policed by the
  wrapper per role: the server answers the reserved-code terminal for the
  opening id before its close, both ends fail closed. A
  vanilla JSON-RPC client that ignores notifications sees one well-formed
  call/response pair; a browser consumes the whole session with
  `JSON.parse` alone (`new WebSocket(url)`, no subprotocol, no codec). The
  translation lives above the transport (`//runtime:eventstream_jsonrpc`'s
  `JsonRpcStreamSocket` — so the in-memory pair carries the actual wire
  text) over a new raw-text transport mode
  (`Options::websocket_raw_text_frames`, `WebSocketDialRequest::
  raw_text_frames`); both serve seams dispatch the opening envelope behind
  one generated `/` stream route, and the terminal envelopes reuse the
  unary emitters — one error identity. Pinned by an authored stream
  conformance suite (`protocol-tests/jsonrpc2`), the calculator's
  `Accumulate` example over pair and real Beast (TLS included), the
  compile gauntlet's constrained-member validation refusal, and a consumer
  CLI suite driving the generated tally service as real processes — the
  generated client for the modeled flows, a raw stdin-to-text-frames peer
  for the refusal and policing edges the well-behaved client cannot
  produce.
- Registry admission primitives (ADR-0022, issue #122):
  `SessionRegistry::ResumeOrAdd(id, mint, deadline)` is the reconnect
  admission recipe as one call — Resume first, fresh Add second, retried
  to the deadline because a reconnect can beat the old wire's failure
  notice — returning the three-way `Admission` every consumer branches
  on (`kResumed` → snapshot replay, `kAdded` → join announce, `kRefused`
  → collision answer). It blocks by contract: call it pre-first-suspend
  on the launching thread, never from a completion context. Its refusal
  is now actionable: `Close(id)` kicks the id's live session — the
  handler observes the close and runs its normal exit path, leaving the
  id admittable (freed after a Remove exit, parked-resumable after a
  Detach exit) — the missing move for silent partitions; policy stays
  with the application. The
  three example admission loops are deleted in favor of the call.
- Generated async streaming handlers (ADR-0021): a streaming service now
  also emits `<Service>AsyncHandler` — each streaming operation a coroutine
  returning the new `smithy::eventstream::StreamTask` over
  `<Op>AsyncServerStream&`, unary operations unchanged — and a second
  `<Service>Server` constructor that registers every streaming route on the
  shared-session seam (`AddSession`/`ServeSession`), so a fully generated
  handler serves N sessions with zero parked threads. The generated launch
  wrapper restores the blocking contract end to end: parse/validation
  refusals answer before any coroutine exists, a `co_return`ed error ends
  the stream with the typed exception message (framed via `SendAsync`,
  completion-context-safe), and a throwing coroutine surfaces as the
  never-leak `InternalFailure` instead of terminating. The chat hub now
  runs on this surface (its hand-written mount deleted; the CLI suite
  passes unchanged), and the consumer workspace drives the same reconnect
  script against blocking and async generated servers alike.
- Reconnect grace (ADR-0020, issue #116): `SessionRegistry` grows
  `Options::{grace_period, on_expired, queue_while_detached}` and
  `Detach(id)`/`Resume(id, handle)` — an abrupt loss parks the session
  (zero per-session threads; one lazy expiry thread per registry), a
  reconnect within grace performs the identity-keyed atomic swap and
  replays a snapshot, and expiry runs its cleanup exactly once, mutually
  exclusive with a successful resume. Events to a detached id drop by
  default (snapshot replay is authoritative); opt-in retention keeps a
  bounded tail. `Drain` and the destructor expire detached sessions
  immediately. The production guide blesses the whole loop — resume
  ticket → gate → `Resume` → snapshot — plus the native redial shape; the
  chat hubs demonstrate the registry half (abrupt kill, snapshot resume,
  deferred departure) as real shell-driven processes.
- Event-stream session handles and fan-out (ADR-0017, issue #112):
  `stream.Share()` mints an `EventStreamHandle<Out>` — an owning cheap-copy
  value handle safe to hold beyond the handler's borrow (copies are how a
  session fans out), sending and closing from any thread while the session
  lives and failing softly with `Error::Transport` (never a dangle) once
  it is gone.
  `smithy::server::SessionRegistry<Out>` builds the multi-client hub on
  top: a thread-safe map of handles with a bounded outbound queue and
  writer thread per session, so `SendTo`/`Broadcast` never block on a slow
  client's wire; per-recipient event construction
  (`Broadcast(ids, make)`) for per-viewer redaction; a slow-consumer
  policy (disconnect by default, `Options::on_slow_consumer` to override);
  and `Drain(grace)` — close every session and wait for handlers to unwind
  — as the graceful step before the transport's abort-flavored `Stop()`.
  The consumer reference is the chat hub (`examples/chat/`): rooms,
  redaction, watchers and talkers on one registry, SIGTERM → drain → clean
  exit, tested in memory and as real processes driven by shell commands.
  **Breaking:** `EventStream` is now move-only (copying was never
  meaningful; handles are how a session fans out).
- Phase 8 slice 3, generated event streams (ADR-0016): `@streaming` union
  operations generate real streaming code for `simpleRestJson` and
  `rpcv2Cbor` (`jsonRpc2` refused with a diagnostic until its native wire
  landed — ADR-0023 above). Clients gain
  `Outcome<EventStream<In, Out>> Op(input)` — the upgrade GET resolves
  `@http` bindings exactly like a unary request, dialing derives from the
  one `ClientConfig` endpoint (an `https` endpoint dials `wss`), and
  `ClientConfig::websocket_dialer` injects a custom dialer the way
  `http_client` injects the unary transport (with
  `smithy::http::InMemoryWebSocketPair`, that is how streams test without
  Boost). Handlers grow
  `Outcome<Unit> Op(input, EventStream<Out, In>&, context)`; generated
  servers expose `StreamRouter()`, a `smithy::server::WebSocketRouter`
  sharing the unary `Router`'s matching, mounted on the transport via
  `websocket_gate`/`on_websocket` in two lines. One event travels per
  binary WebSocket message in the slice-1 framing with an authored,
  vendor-neutral envelope (`:message-type`/`:event-type`/
  `:exception-type`); a handler error becomes one typed exception message
  then a close, surfacing client-side exactly like a unary modeled error
  (kind, code, typed detail). Scope edges are generation-time diagnostics
  (`@eventHeader`/`@eventPayload`, body-bound initial-request members,
  initial-response members), generated smoke/integration suites skip
  streaming operations, and BUILD deps grow only for streaming services.
  PLAN §Phase 8's exit criterion lands as `examples/chat/`: the generated
  chat client and server run full duplex over real WebSockets (TLS
  included) in CI, with an in-memory twin and an out-of-tree consumer
  acceptance test beside it.
- Phase 8 slice 2, WebSocket transports (ADR-0015): `BeastServerTransport`
  upgrades WebSocket requests in place — `Options::websocket_gate` refuses
  with a plain HTTP answer before any 101 exists (auth sees the whole
  request), `Options::on_websocket` serves the accepted session on the
  handler pool — and `BeastWebSocketClient::Dial` is the client end
  (ADR-0007 TLS posture, SNI, hostname verification, one handshake
  budget). Both ends speak `smithy::http::WebSocket`: blocking full-duplex
  `Send`/`Receive`/`Close` carrying one event-stream frame per binary
  WebSocket message, with real backpressure both directions, keep-alive
  pings under an idle timeout, and protocol violations (text messages,
  non-frame payloads) failing the session. Failed upgrades surface as the
  new `ConnectionEvent::Kind::kUpgradeFailure`; `Stop()` aborts live
  sessions so blocked serve callbacks wake. Usable directly ahead of the
  generated streaming API (slice 3).
- Phase 8 groundwork, wire-format-first (ADR-0014): `//runtime:eventstream`
  is the event-stream message framing the binding streaming protocols
  (simpleRestJson, rpcv2Cbor) are defined against — CRC-guarded prelude, ten typed header wire types (headers
  build from plain values and the timestamp is the runtime's own
  `smithy::Timestamp`), opaque `Blob` payloads, an incremental strict
  fail-closed decoder, and symmetric bounds (Encode refuses whatever
  Decode would reject). `Message::FindHeader`/`FindString` cover the
  dispatch-on-`:event-type` lookup every consumer does, and messages
  debug-render. Fuzzed, and pinned with byte-exact vectors from an
  independent implementation.
- Fuzz harnesses (JSON, CBOR, URI, server dispatch, regex) and a Google
  Benchmark suite (serde, codecs, per-protocol request round trips, real-TCP
  transport round trips incl. Beast and Beast TLS) run in CI.
- CBOR decoder rejects additional-information 31 on integers and tags
  (RFC 8949 §3.3 not-well-formed encodings previously decoded as 0 / -1 /
  an ignored tag), found by the hostile corpus below.

### Removed

- **Windows support** (ADR-0008, issue #58). Linux and macOS are the
  supported platforms, both sanitizer-covered in CI. The socket transport is
  POSIX-only (no winsock/WSAStartup fabric), the Bazel rules carry no
  Windows `select()`s or `build:windows` config, and the Windows CI jobs are
  gone. Re-adding Windows would be a port driven by a concrete consumer, not
  a revert.

### Testing & CI (issue #48)

- **Compile-the-output harness** (`codegen/compile-tests/`): the generator
  runs inside the Bazel graph on a hostile gauntlet model — C++ keyword
  member names, quote/backslash/newline enum values, raw-string delimiter
  attacks, int64-extreme bounds/defaults, recursion, keyword union variants —
  and CI compiles the result for every protocol, client and server mode both.
  Issue #43's whole bug class now fails CI instead of a consumer's build.
- Curated hostile CBOR corpus (`cbor_hostile_test.cc`): systematic
  truncations, reserved encodings, indefinite-length abuse, depth bombs,
  boundary integers/halves, and an every-strict-prefix-rejects property, as
  the CBOR counterpart of the vendored JSONTestSuite bank.
- Direct unit tests for `core/uuid.cc` (format, version/variant bits,
  uniqueness, thread-local streams) and `client/observability.cc`
  (attempt observations, trace-context propagation).
- The regex ReDoS bound is a deterministic step-count assertion
  (`Search(text, &steps)` instrumentation) instead of a wall-clock limit.
- `make verify` / `make verify-full`: one-command local verification
  mirroring the CI jobs one-to-one.
- **HTTP/1.1 parser extracted and fuzzed** (`smithy/http/http1.h`): the
  socket transports' hand-rolled message reader is now a pure,
  callback-fed function with a libFuzzer harness (`//fuzz:http1_fuzz`, in
  the CI smoke loop) and a platform-independent hostile bank
  (`http1_hostile_test.cc`) covering smuggling framing, hostile
  content-lengths, truncation-everywhere, and header floods. Hardening
  found while banking: an empty or `+`-signed Content-Length previously
  parsed as a valid length; both now reject (digits-only per RFC 9110).
- **Malformed-server coverage evened out**: hand-written suites pin how the
  generated simpleRestJson and rpcv2Cbor servers reject hostile requests
  (unparseable bodies, protocol-precondition violations, wrong
  content-type/method/route) the way jsonrpc2's generated suite always did —
  including the previously-unasserted simpleRestJson `@pattern`-violation
  wire message.
- **Union x protocol conformance cells filled** (`protocol-tests/unions/`):
  the cbor and jsonRpc2 union cells — previously reliant on coin-flip random
  integration tests that only prove serde self-consistency — now pin the
  wire subdocument for every union variant deterministically in all four
  directions (client encode/decode, server decode/echo), plus the reject
  cells (empty, multi-member, unknown-member, null-member) and the `__type`
  discriminator tolerance.
- **Union member-type gauntlet** (`protocol-tests/unions/` + issue #56):
  a generated-in-graph UnionGauntlet service extends the union cells to
  blob, timestamp, list, map, enum, intEnum, and recursive-struct members,
  each pinned in four directions per protocol, with a hand-derived
  byte-exact request vector (RFC 8949 deterministic encoding / compact
  sorted JSON) and the error-shape cell: a modeled error whose union member
  rides next to its `__type` discriminator, both wire-inspected and
  round-tripped into the typed error detail. The existing reject cells now
  pin their diagnoses (exactly-one-member, unknown-member, non-map), not
  just the rejection; the `__type` tolerance is documented in
  docs/generated-types.md as the serde contract.
- **Sanitizers run on macOS too**: the asan+ubsan CI job is now a
  linux/clang + macos/apple-clang matrix, covering the transport layer's
  Apple-specific paths (SO_NOSIGPIPE, libc++) — every supported platform is
  now sanitizer-covered.
- **Golden self-ratification closed** (`GoldenProtocolTestAuditTest`): the
  byte-identical regeneration check validates the goldens against the same
  generator that produced them, so a generator bug that dropped or rewrote
  conformance vectors could ratify itself. A new audit enumerates the test
  cases from the upstream suite definitions (the alloy and Smithy
  conformance jars, and the authored jsonRpc2 model) using only the
  upstream smithy-model API and asserts every case is either in the
  committed golden test sources or in the must-shrink exclusion list — plus
  per-case wire facts (method/status), no phantom tests, and no exclusion
  naming a nonexistent upstream case. Together with the generator's
  stale-exclusion guard, the seam is now watched from both sides.
- **Generator-class unit tests**: direct Java suites for the previously
  untested generator internals — CppLiterals (the issue-#43 escaping
  chokepoint: octal escapes, int64-min idiom, float literal typing),
  CppReservedWords (keyword vs macro boundary), ProtocolSupport bounds,
  CppSettings validation, MemberDefaults (the `@default`/`@input`/`@required`
  semantics matrix), RecursionIndex (boxing decisions and refused cycles),
  plus emitted-source suites for SerdeGenerator (required-member errors,
  dense-null rejection, union exactly-one arithmetic, `@timestampFormat`),
  ValidationGenerator (suite-exact messages, compilable bounds, one-time
  pattern compilation, code-point vs element length), and
  BuildFileGenerator (target set per mode, runtime target wiring,
  emitBuildFile). 43 new tests; generator unit coverage was 2 of 31
  classes before this.
- **Code-coverage tooling**: a `coverage` CI job runs
  `bazel coverage --combined_report=lcov` over the runtime, prints the
  per-module summary, and uploads the rendered HTML report as an artifact;
  `make coverage` runs the same locally. Measurement only — no gate yet.

[0.2.0]: https://github.com/muchq/smithy-cpp/releases/tag/v0.2.0
[0.1.0]: https://github.com/muchq/smithy-cpp/releases/tag/v0.1.0
