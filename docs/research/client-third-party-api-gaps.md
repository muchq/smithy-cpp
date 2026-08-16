# Research: options for third-party HTTPS client gaps (#189)

**Status:** Research note (2026-08-16). No decision — input for ADRs / follow-up
issues. Maps options for
[issue #189](https://github.com/muchq/smithy-cpp/issues/189), sourced from
[MoonBase#1390](https://github.com/muchq/MoonBase/issues/1390).

Evaluated against current `master`. Prior art that constrains the transport
side: ADR-0005 (dep-light transports), ADR-0007 (Beast + BoringSSL TLS),
`docs/research/libwebsockets-transport.md` (lws deferred).

## TL;DR

| # | Gap | Recommended option | Effort shape |
|---|---|---|---|
| 1 | Boost-free TLS | **A.** `//runtime:http_tls` — socket + BoringSSL (already a direct dep) | New transport target; keep Beast for server/WS |
| 2 | Proxy + CA add | Env + explicit proxy on that transport; **`ca_pem` append mode** | CONNECT is the hard part; CA fix is small |
| 3 | Response decompress | Client-layer gunzip via existing `GzipDecompress` + `accept_encoding` | Plumbing only; no new deps |
| 4 | `Retry-After` | Floor under delay with separate `retry_after_cap` | Local `retry.cc` change |
| 5 | First-class 304 | Document `@httpResponseCode` now; optional `http_status` on `Error` later | Mostly model/docs; small Error API |
| 6 | Streaming bodies | Defer for unary JSON; sink/spill later for blobs | Large if full codegen streaming |
| 7 | Overall deadline | `ClientConfig::overall_timeout_ms` in `SendWithRetries` | Small config + retry loop |

**Suggested sequencing:** `3 → 4 → 7 → 2d(ca)` (quick wins) then `1A → 2a/2b`
(headline HTTPS path). Do not block on curl, lws, Beast CONNECT, or full body
streaming. MoonBase can take Beast now; **proxy remains the sandbox smoke-test
blocker** regardless of TLS transport.

---

## Current anchors

| Piece | State today |
|---|---|
| Transports | `SocketHttpClient` (plaintext, `:http`), `BeastHttpClient` (TLS + pool, `:http_beast`), `Loopback` |
| `Create()` | https without `config.http_client` → Validation — never auto-builds Beast |
| TLS | asio-SSL over BoringSSL inside Beast only; `TlsOptions::{verify_peer, ca_pem}` |
| `ca_pem` | **Replace** system roots (`add_certificate_authority` XOR `set_default_verify_paths` in `SetupClientTlsContext`) |
| Proxy | Absent on the client (ADR-0007 explicitly out of scope for 0.1.0) |
| Retry | Full-jitter only; `RetryableStatus` = 429/500/502/503/504; no `Retry-After` read |
| Bodies | `using Body = std::string`; Beast client `parser.body_limit(boost::none)` |
| Compression | Request-side gzip (`@requestCompression`); `GzipDecompress` unused on responses |
| Timeouts | `request_timeout_ms` per attempt only; no overall wall-clock budget |
| 304 | Fixed `@http(code: 200)` → error path; `@httpResponseCode` treats `200 ≤ status < 400` as success |
| `Error` | No HTTP status field — empty-body 304 surfaces as `UnknownError` / `"HTTP 304"` |

`transport.h` still comments “adapters for libcurl etc. later”; PLAN’s curl
client story was superseded by ADR-0005/0007 (Beast production client).

---

## 1. Boost-free TLS transport

### Options

| Option | Client-path deps | Buys | Cost / risks |
|---|---|---|---|
| **A. `//runtime:http_tls`** — extend the socket stack with BoringSSL | `@boringssl` only (already direct; no Boost) | Two-library client footprint + HTTPS; fits ADR-0005; `Create()` can auto-pick on https | Own TLS I/O, timeouts, SNI, hostname verify; pooling optional later; no WS (Beast stays for streams/servers) |
| **B. `//runtime:http_curl`** — libcurl `HttpClient` | BCR `curl` (default BoringSSL; module graph still lists openssl/mbedtls) | Proxy env + CONNECT, response decompress, CA/bundle knobs largely free; optional HTTP/2 | Heavier than A; `ssl_lib` build-flag shape ADR-0007 rejected for asio; still additive via injection |
| **C. Keep Beast; document inject** | ~53 Boost modules when HTTPS | Zero new code; some consumers already link Beast for servers | Client-only binaries pay Boost for TLS; does not fix sandbox proxy |
| **D. libwebsockets** | See existing research note | H2/H3, built-in proxy | BCR TLS backend flag + event-loop inversion — **no change now** |

### Recommendation

**A** as the headline fix for client-only HTTPS. Keep Beast as the production
default for servers and WebSockets. Consider **B** later if reinventing CONNECT
and response decode is not worth it. Do **not** make `Create()` silently link
Beast (breaks the dep-light rule).

Once A (or B) exists, auto-wire order:

1. Prefer injected `http_client`.
2. Else https → `TlsSocketHttpClient::FromConfig` (or curl equivalent).
3. Else http → `SocketHttpClient`.

`FromConfig` should honor the same `TlsOptions` (and future proxy knobs) as
Beast so configuration does not fork.

Rough implementation sketch for **A:** wrap the existing `http1` read/write
path over `SSL_*` BIOs or a thin `SslSocket` helper; reuse request-line/header
validation from `SocketHttpClient`; share certificate setup with a
Boost-free twin of `SetupClientTlsContext` (raw BoringSSL, not asio-SSL).

---

## 2. Proxy support and CA add-vs-replace

### Proxy options

| Option | Where | Notes |
|---|---|---|
| **2a. Env-based** | `HTTPS_PROXY` / `HTTP_PROXY` / `NO_PROXY` in new TLS transport(s) | Curl: nearly free. Socket+BoringSSL: CONNECT tunnel then TLS; http proxy uses absolute-form request URI |
| **2b. Explicit config** | `ClientConfig::proxy` / `ProxyOptions` | Testable; non-env deployments; can still default from env |
| **2c. Beast CONNECT** | `beast_transport.cc` | Large; ADR-0007 out of scope; only helps Beast consumers |

### CA options

| Option | Behavior |
|---|---|
| **2d. Append by default** | Always `set_default_verify_paths`, then `add_certificate_authority` when `ca_pem` set |
| **2e. Explicit mode** | `ca_pem_mode = kReplace \| kAppend` (default `kAppend`); tests that use self-signed-as-root keep `kReplace` |

Today’s replace semantics are load-bearing for unit tests that pass the
self-signed leaf as `ca_pem` (`runtime/testing/.../tls_test_identity.h`).

### Recommendation

Ship **2e** (or **2d** + test updates) with whichever TLS path — small and
unblocks “public roots + proxy MITM CA”. Prefer **2a+2b on the Boost-free
transport** (and on curl if chosen). Do not block the headline TLS work on
Beast CONNECT. Item 2 is the sandbox live-API blocker regardless of (1).

---

## 3. Response decompression

| Option | Notes |
|---|---|
| **3a. Client middleware after transport** | Inspect `Content-Encoding`; `GzipDecompress`; strip encoding; reuse 64 MiB bomb cap |
| **3b. Transport-native** | Curl does this; DIY transports should still converge on one policy in `:client` |
| **3c. `ClientConfig::accept_encoding`** | Default off or `"gzip"`; set `Accept-Encoding` when unset on the request |

### Recommendation

**3a + 3c in `//runtime:client`** — works for Socket, Beast, Loopback, and a
future `http_tls` without new dependencies. Start gzip-only; leave br/zstd for
later. Make the output cap configurable (align with server request decompress).

---

## 4. Honor `Retry-After`

Hook point: `SendWithRetries` in `runtime/src/client/retry.cc` already has the
response when deciding to sleep.

| Option | Behavior |
|---|---|
| **4a. Floor, then `max_backoff`** | `max(RetryDelay, ParseRetryAfter)` then `min(..., max_backoff)` — a `Retry-After: 30` is crushed by the 20 s default |
| **4b. Floor with separate cap** | Honor server up to `RetryPolicy::retry_after_cap` (e.g. minutes) so polite servers win |
| **4c. Exact wait when header present** | No jitter — predictable, worse synchronized retries |

Parse delta-seconds and HTTP-date (IMF-fixdate; runtime already has timestamp
helpers). Invalid/missing → current jitter path. Apply on retryable statuses
(at least 429/503).

### Recommendation

**4b**, optionally `floor + jitter * slack` above the server value. Sleep must
also respect item 7’s remaining deadline.

---

## 5. First-class 304 / conditional GET

Smithy’s `@error` is only `client`/`server`, so 304 cannot be a modeled error.
The codegen escape hatch already exists: operations with `@httpResponseCode`
treat any non-error status (`200 ≤ status < 400`) as success — see the
redirect / `If-None-Match` example under `examples/bazel-consumer/`.

| Option | Shape | Pros / cons |
|---|---|---|
| **5a. Document the `@httpResponseCode` pattern** | Model-only | Zero runtime; every API must opt in |
| **5b. Optional `http_status` on `Error` / `ParsedError`** | `error.http_status() == 304` | Stops string-matching `"HTTP 304"`; still error-shaped |
| **5c. Soft sentinel** | Distinct `code` / kind for not-modified | Clearer than GenericError; still not success |
| **5d. Codegen: 304 → empty success** | Opt-in flag/trait | Ergonomic for caches; clashes with `@required` output members |
| **5e. Helpers only** | `IsNotModified(error)` + header helpers | Weak typing; no model change |

### Recommendation

**5a** for APIs we control (e.g. a chess.com Smithy model). **5b** when
touching the Error surface so all paths can branch cleanly. Defer **5d** until
there is a real Smithy-level story; do not invent a private trait lightly.

---

## 6. Streaming / sink-based response bodies

Unary responses are fully buffered (`Body = std::string`). Event-stream
`@streaming` is a different wire (WebSocket) and does not help large unary
JSON/blob downloads.

| Option | Scope |
|---|---|
| **6a. Stay buffered; document limits** | Status quo — OK until multi-MB concurrency hurts |
| **6b. Transport `BodySink` callback** | Bounds read memory; JSON decode still wants a full document |
| **6c. Codegen blob/`@httpPayload` streaming** | Large design, adjacent to event-stream complexity |
| **6d. Spill-to-file helper** | Practical for workers; consumer-side |

### Recommendation

**6a** for the initial third-party JSON client. Revisit **6b/6d** when a
concrete multi-MB concurrency budget appears. Do not block 1–4 on **6c**.

---

## 7. Overall deadline

| Option | Notes |
|---|---|
| **7a. `ClientConfig::overall_timeout_ms`** | `SendWithRetries` tracks start; skip/sleep clamped to remaining; `Error::Timeout` on expiry |
| **7b. Absolute `deadline` time_point** | Better for shared budgets across several calls |
| **7c. Rely on `max_attempts` only** | Already exists; does not bound wall clock |

### Recommendation

**7a** first. Clamp per-attempt `request_timeout_ms` to remaining budget when
forwarding to the transport if practical. Interact with **4b**: never sleep
past the overall deadline. `Error::Timeout` already distinguishes this class
from a broken wire.

---

## Sequencing

```text
Quick wins (no new transport):   3 → 4 → 7 → 2e (ca mode)
Headline third-party HTTPS:      1A (+ Create auto-wire) → 2a/2b on that transport
Model-side 304:                  5a now; 5b when Error grows status
Defer:                           1B/1D, Beast CONNECT, 6c, HTTP/2
```

## Product choices for a follow-up ADR

1. Is **`http_tls` (A)** the blessed client-only HTTPS path, with Beast remaining
   the server/WS default?
2. **`ca_pem`:** append-by-default with explicit replace mode for tests?
3. **`Retry-After` vs `max_backoff`:** separate cap (4b) or hard clamp (4a)?
4. Should **`Create()` auto-construct** the Boost-free TLS transport on https,
   or keep explicit inject forever?

## Sources (in-repo)

- Issue [#189](https://github.com/muchq/smithy-cpp/issues/189)
- ADR-0005, ADR-0007; `docs/research/libwebsockets-transport.md`
- `runtime/include/smithy/http/transport.h`, `client/config.h`, `client/retry.h`
- `runtime/src/http/beast_transport.cc` (`SetupClientTlsContext`), `socket_transport.cc`
- `runtime/src/client/retry.cc`; `compression/gzip.h`
- `codegen/.../ClientGenerator.java` (`Create()` https rejection)
- BCR [`curl`](https://registry.bazel.build/modules/curl) (option B dependency shape)
