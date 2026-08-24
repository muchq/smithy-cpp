#ifndef SMITHY_HTTP_BEAST_TRANSPORT_H_
#define SMITHY_HTTP_BEAST_TRANSPORT_H_

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "smithy/http/transport.h"
#include "smithy/http/websocket.h"

namespace smithy {
// smithy/client/config.h — forward-declared so this header stays includable
// without the client headers. The library dependency is deliberate:
// FromConfig is the ClientConfig→transport bridge, and it lives here because
// only this side can construct a Beast client while :client stays Boost-free.
struct ClientConfig;
}  // namespace smithy

namespace smithy::http {

// Production HTTP/1.1 server transport on Boost.Beast/asio (ADR-0006):
// concurrent connections on an asio thread pool (bounded by
// Options::max_connections), handlers on their own executor
// (Options::handler_threads) so blocking handlers don't starve the wire,
// keep-alive, per-connection timeouts, graceful shutdown, and optional TLS
// termination (ADR-0007).
// This is what generated services should run on; WebSocket upgrades
// (Phase 8) extend this transport.
//
//   smithy::http::BeastServerTransport server({.port = 8080});
//   server.Start(service.Handler());
//   ...
//   server.Stop();
class BeastServerTransport : public HttpServerTransport {
 public:
  // A request the transport rejected itself — the over-limit 413/431 answers
  // written before a handler chain exists, which Observe middleware therefore
  // never sees (issue #46). method/target may be empty when the request never
  // parsed that far (a 431 can fire mid-headers).
  //
  // The `= {}` on the strings is not redundant with their default constructor
  // (issue #193). Clang's -Wmissing-designated-field-initializers, on under
  // -Wextra since Clang 19, fires on a designated initializer that omits a
  // field lacking an NSDMI — so without these a caller describing exactly the
  // case above, `{.status = 431}`, would not compile under this repo's
  // --config=werror, and would have to spell out the empties to say what the
  // omission already said.
  struct RejectedRequest {
    int status = 0;
    std::string peer_address = {};
    std::string method = {};
    std::string target = {};
  };

  // A connection the transport terminated without delivering a response
  // (ADR-0013) — failures the wire sees and Observe middleware doesn't:
  // most happen before any handler chain ran; the write-phase kDropped
  // happens after, so that request appears in BOTH the Observe stream and
  // here (the composition, not a double-count — the event says its
  // response never arrived). Silence means healthy: clean keep-alive
  // closes (with or without TLS close_notify), idle timeouts with nothing
  // received, probe-shaped handshake non-starts, and Stop()-time
  // cancellations are deliberately not reported.
  struct ConnectionEvent {
    enum class Kind {
      // TLS was configured and a handshake went WRONG: plaintext to the
      // TLS port, a version/cipher/ALPN mismatch. A flood of these is the
      // "something is sending plaintext here" alarm. Connections that just
      // went away — TCP health probes and scanners that connect and leave
      // or idle into the deadline — are silent: that noise scales with
      // infrastructure, not incidents.
      kTlsHandshakeFailure,
      // Bytes arrived that never parsed into a request.
      kFramingError,
      // A request that had begun (at least one octet parsed) stalled past
      // request_timeout_seconds: the slowloris shape.
      kReadTimeout,
      // The peer vanished mid-request or mid-response.
      kDropped,
      // A request asked for a WebSocket upgrade, the gate admitted it, and
      // the upgrade handshake then failed (ADR-0015). Once a session is
      // up, wire failures surface through Send/Receive to the serve
      // callback instead — the application is the observer there.
      kUpgradeFailure,
    };
    Kind kind = Kind::kDropped;
    // "ip:port"; may be empty when the socket can no longer report it.
    std::string peer_address = {};
    // The transport's own error text (error_code::message()).
    std::string detail = {};
    // Time spent in the failing phase (handshake, read, or write).
    std::chrono::microseconds elapsed{0};
  };

  struct Options {
    // "127.0.0.1" keeps test servers private; use "0.0.0.0" to serve externally.
    std::string address = "127.0.0.1";
    int port = 0;  // 0 binds an ephemeral port; read port() after Start.
    int threads = 4;
    // Handlers run on this dedicated pool, so a blocked handler (DB call,
    // downstream RPC, lock) cannot starve the io threads that accept
    // connections and read/write the wire (issue #46) — size it for your
    // handlers' blocking profile. 0 runs handlers inline on the io pool,
    // saving the executor hop when every handler is CPU-cheap and
    // non-blocking.
    int handler_threads = 16;
    // Concurrent-connection cap: at the limit the server stops accepting and
    // new connections wait in the kernel's listen backlog until a session
    // closes, so a connection flood cannot exhaust fds/memory (issue #46).
    // Idle keep-alive sessions still expire on request_timeout_seconds, so
    // they cannot pin the cap forever. 0 means unlimited.
    std::size_t max_connections = 1024;
    int request_timeout_seconds = 30;
    // Over-limit requests are answered (413 for the body, 431 for headers)
    // with Connection: close and a bounded lingering close, not silently
    // aborted (issue #94).
    std::size_t max_body_bytes = std::size_t{64} * 1024 * 1024;
    std::size_t max_header_bytes = std::size_t{8} * 1024;
    // Stop() drains: no new connections or keep-alive reads, and in-flight
    // requests get this long to finish before the pool is torn down. Stop()
    // itself is bounded — a handler that never returns is abandoned (its
    // thread and the transport state deliberately leak, with a std::clog
    // trace) a short grace after this deadline instead of wedging Stop()
    // forever, so budget supervisor kill timeouts at roughly this plus 2
    // seconds (issue #46).
    int drain_timeout_seconds = 10;
    // Observation hook for the transport's own rejections (one call per
    // RejectedRequest, before the rejection response is written). Runs on an
    // io thread, concurrently across connections — keep it cheap and
    // thread-safe; a throwing callback is contained and logged. Wire it to
    // the same sink as smithy::server::Observe so over-limit abuse is
    // visible in the same metrics.
    std::function<void(const RejectedRequest&)> on_rejected{};
    // Observation hook for connections the transport terminated without a
    // response (one call per ConnectionEvent; ADR-0013). Same contract as
    // on_rejected: io thread, concurrent across connections, cheap and
    // thread-safe, throwing callbacks contained and logged. Wire it to the
    // same sink so handshake failures, framing garbage, stalls, and drops
    // are visible in the same metrics as served requests.
    std::function<void(const ConnectionEvent&)> on_connection_event{};
    // WebSocket upgrade (ADR-0015). Unset, requests that ask for an
    // upgrade remain ordinary HTTP requests for the handler chain to
    // answer (426, 404 — its call). Set, an upgrade request is offered to
    // websocket_gate after being fully read (the decision sees the whole
    // request: target, headers, auth material) — return an HttpResponse
    // to refuse with that answer before any 101 exists, nullopt to
    // accept — and the accepted session runs in on_websocket on the
    // handler pool (it blocks by design, so Start refuses this with
    // handler_threads == 0). on_websocket's return ends the session with
    // a close handshake — the WebSocket& is valid only until then, so
    // join any helper thread still using it before returning. An unset
    // gate accepts every upgrade: admission policy is the application's,
    // composable from the same middleware pieces the HTTP chain uses.
    std::function<std::optional<HttpResponse>(const HttpRequest&)> websocket_gate{};
    std::function<void(const HttpRequest&, WebSocket&)> on_websocket{};
    // The shared-session sibling of on_websocket (ADR-0019): receives the
    // accepted session as an owner and may return immediately — the
    // session lives until a Close (any holder or the peer), the idle
    // timeout, or Stop()'s abort, so serving it costs no parked handler
    // thread (the completion-driven receive and the coroutine adapter in
    // smithy/eventstream/async_event_stream.h are the intended loop).
    // At most one of on_websocket / on_websocket_session may be set
    // (neither is an HTTP-only server); the gate and the wire mode — the
    // JSON-frames negotiation or the raw-text flag below — apply to
    // both. Runs contained on the handler pool, and
    // should return quickly — it is a launch point, not a serve loop.
    std::function<void(const HttpRequest&, std::shared_ptr<WebSocket>)> on_websocket_session{};
    // Negotiated JSON-text event-stream frames (ADR-0018): set, a client
    // that offers the `smithy.eventstream.v1+json` subprotocol on the
    // upgrade gets the token echoed in the 101 and a session whose
    // messages travel as text frames carrying the JSON envelope
    // ({"event": "<member>", "payload": {...}}) — the wire a browser
    // speaks with JSON.parse alone. Everyone else keeps the binary wire,
    // headerless 101 included: native clients never negotiate, and with
    // this unset the transport is byte-identical to one predating the
    // mode. The mode is a wire detail — on_websocket, the routers, and
    // generated code still speak eventstream::Message — but enable it
    // only when the services served here carry JSON event payloads
    // (simpleRestJson): a non-JSON event on a negotiated session fails
    // its first Send instead of riding a text frame.
    bool websocket_accept_json_frames = false;
    // The raw-text wire (ADR-0023): every accepted upgrade carries
    // headerless messages as verbatim text frames, one JSON-RPC 2.0
    // envelope per frame — what a generated jsonRpc2 streaming server
    // mounts. Unnegotiated (a browser connects with plain
    // `new WebSocket(url)`; the wire IS the protocol), so it claims the
    // whole listener: Start refuses it together with
    // websocket_accept_json_frames, and a binary frame fails the session
    // the way text does on the default wire. Enable it only on a server
    // whose upgrades all speak jsonRpc2 streams.
    bool websocket_raw_text_frames = false;
    // How long a silent upgraded connection stays up (keep-alive pings
    // run underneath — a healthy-but-quiet stream survives, a vanished
    // peer is detected). The HTTP request_timeout_seconds governs the
    // wire only up through the 101.
    int websocket_idle_timeout_seconds = 300;
    // TLS termination: set both (PEM text, not file paths) to serve https.
    // Posture is fixed, not configurable (issue #46): TLS 1.2 minimum,
    // ECDHE+AEAD cipher suites for 1.2 (every 1.3 suite qualifies), and ALPN
    // that answers http/1.1 — a client offering ALPN without http/1.1 (e.g.
    // h2-only) is refused at the handshake. mTLS is tracked with #90.
    std::string tls_certificate_chain_pem{};
    std::string tls_private_key_pem{};
  };

  BeastServerTransport() : BeastServerTransport(Options{}) {}
  explicit BeastServerTransport(Options options);
  ~BeastServerTransport() override;

  Outcome<Unit> Start(RequestHandler handler) override;
  void Stop() override;

  int port() const;

 private:
  struct State;  // Hides boost headers from this public header.

  void Shutdown() noexcept;
  // Start()'s body; Start() wraps it so no exception (a std::thread ctor
  // failure, a bad_alloc) crosses the Outcome boundary (ADR-0003).
  Outcome<Unit> StartContained(RequestHandler handler);

  Options options_;
  std::shared_ptr<State> state_;
  std::vector<std::thread> threads_;
};

// Production HTTP/1.1 client transport on Boost.Beast/asio (ADR-0007):
// keep-alive connection reuse behind a small idle pool, per-request
// timeouts, and TLS via asio SSL (BoringSSL) with certificate and hostname
// verification on by default. Thread-safe: concurrent Send() calls use
// distinct connections.
//
//   smithy::ClientConfig config;
//   config.endpoint = "https://api.example.com";
//   config.tls.ca_pem = corp_ca_pem;               // when not publicly trusted
//   auto transport = smithy::http::BeastHttpClient::FromConfig(config);
//   if (!transport) { /* bad endpoint */ }
//   config.http_client = *transport;               // the wire
//   auto client = MyServiceClient::Create(std::move(config));
class BeastHttpClient : public HttpClient {
 public:
  // Direct construction for tests and custom wiring. Production
  // configuration should flow through FromConfig so every knob lives on the
  // one ClientConfig (issue #49).
  struct Options {
    std::string host{};
    int port = 80;
    bool tls = false;
    // Verification knobs when `tls` is true — the same struct ClientConfig
    // carries, so FromConfig copies it wholesale and the two can't drift.
    TlsOptions tls_options{};
    int request_timeout_ms = 30000;
    // Idle keep-alive connections retained for reuse.
    std::size_t max_idle_connections = 4;
  };

  explicit BeastHttpClient(Options options);
  ~BeastHttpClient() override;

  // One-stop construction from the ClientConfig the generated client will
  // use: endpoint (scheme/host/port), tls.verify_peer/tls.ca_pem,
  // request_timeout_ms, and max_idle_connections all come from the config,
  // so nothing is configured twice. Fails on an unparsable or non-http(s)
  // endpoint. Any endpoint path prefix stays the generated client's job.
  static Outcome<std::shared_ptr<BeastHttpClient>> FromConfig(const ClientConfig& config);

  Outcome<HttpResponse> Send(const HttpRequest& request) override;

 private:
  struct State;  // Hides boost headers from this public header.

  // Send()'s body; Send() wraps it so no exception (a bad_alloc from the sync
  // drive) crosses the Outcome boundary (ADR-0003).
  Outcome<HttpResponse> SendContained(const HttpRequest& request);

  std::shared_ptr<State> state_;
};

}  // namespace smithy::http

#endif  // SMITHY_HTTP_BEAST_TRANSPORT_H_
