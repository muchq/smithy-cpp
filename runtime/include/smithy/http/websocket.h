#ifndef SMITHY_HTTP_WEBSOCKET_H_
#define SMITHY_HTTP_WEBSOCKET_H_

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "smithy/eventstream/frame.h"
#include "smithy/http/headers.h"
#include "smithy/http/transport.h"

namespace smithy::http {

// A blocking, full-duplex event-stream session over one WebSocket
// connection (ADR-0015). What travels is eventstream::Message — exactly one
// frame per binary WebSocket message; text messages and malformed frames
// fail the session. (Two sibling wire modes flip the encoding without
// touching this facade: a session negotiated to ADR-0018's JSON-text mode
// carries one JSON envelope per text message, and a raw-text session
// (ADR-0023, jsonRpc2 streams) carries headerless Messages as verbatim
// text frames — in both, binary messages fail, the transport translates,
// and every mode carries the same Messages.) Both ends of the
// wire speak this type: the server side arrives in
// BeastServerTransport::Options::on_websocket (borrowed) or
// on_websocket_session (shared, ADR-0019), the client side from
// BeastWebSocketClient::Dial.
//
// Full duplex means one receiving thread and one sending thread may block
// concurrently (mirroring WebSocket's own one-read + one-write model);
// concurrent Send calls are serialized, not rejected, while concurrent
// Receive calls contend for messages in unspecified order (not detected) —
// keep one receiver. Backpressure is
// real on both sides: Send returns when its frame is on the wire, and a
// receiver that stops calling Receive pauses the wire after a small
// internal buffer fills. A paused reader also pauses the keep-alive
// machinery, so come back for your messages within the idle timeout —
// a receiver that never returns is eventually indistinguishable from a
// vanished peer, and the session ends on the idle deadline.
//
// Canonical serve/consume loop:
//
//   while (true) {
//     auto message = socket.Receive();
//     if (!message.ok()) break;              // wire failed: log and stop
//     if (!message->has_value()) break;      // peer closed cleanly: done
//     ... handle **message, socket.Send(reply) ...
//   }
//
class WebSocket {
 public:
  virtual ~WebSocket() = default;

  // Blocks for the next message. nullopt is the peer's clean close — the
  // stream's natural end, not an error, and the reverse of
  // eventstream::DecodeMessage's nullopt: it never means "try again"
  // (another Receive just returns nullopt again, immediately). Errors are
  // permanent for the session: a broken wire, a protocol violation (text
  // message, a binary message that is not exactly one event-stream
  // frame), or Stop()/Close.
  virtual Outcome<std::optional<eventstream::Message>> Receive() = 0;

  // The same receive under a deadline, for a caller that must not wait
  // forever for a message the peer may never send (a test asserting an
  // event, a poll loop with other work to do). Four outcomes instead of
  // three: the message, the peer's clean close (nullopt), the session's
  // permanent error — and Error::Timeout, whose code ("TimeoutError")
  // is what separates "nothing yet" from both of those.
  //
  // A timeout leaves the session untouched and usable — that is the whole
  // point of the deadline over Close(), which unblocks a receive by ending
  // the stream: receive again, send, close, whatever the caller decides
  // now that it stopped waiting. The one-outstanding-receive rule (below)
  // is unaffected: a timed-out receive releases its slot exactly as a
  // completed one does. A non-positive timeout polls — it takes a message
  // that is already in hand or a terminal state, and otherwise times out
  // without blocking. The deadline bounds THIS call only; the session's
  // own idle timeout still governs a quiet wire.
  //
  // Pure virtual, unlike the ReceiveAsync defaults below, because there is
  // no honest default to write: nothing this class can reach bounds a wait.
  // Wrapping the blocking Receive() in a thread or an async twin leaves the
  // parked receive holding its slot and eventually eating a message with
  // no caller left to take it (and cancelling it means Close(), which ends
  // the session — the very thing the deadline avoids), and stashing that
  // message for the next call needs per-session state this interface does
  // not have. So the wait is bounded where the waiting actually happens,
  // by the implementation, or not offered at all. Delegating sockets pass
  // the deadline straight through to what they wrap.
  virtual Outcome<std::optional<eventstream::Message>> Receive(
      std::chrono::milliseconds timeout) = 0;

  // Blocks until the message's frame is on the wire (natural backpressure —
  // nothing queues unboundedly). Fails with Error::Validation for a message
  // the codec refuses to encode (the session is untouched and stays
  // usable), Error::Transport once the session is closed or broken —
  // including a Close() from another thread while this call is blocked.
  virtual Outcome<Unit> Send(const eventstream::Message& message) = 0;

  // Initiates the close handshake; idempotent, non-blocking, and safe
  // from any thread — this is how another thread unblocks a blocked
  // Receive (which then returns nullopt, or an error if the close fails)
  // OR a blocked Send (which fails with a transport error; its frame may
  // be mid-wire, so the close aborts the connection rather than finishing
  // the write, and the peer then observes an error instead of a clean
  // end). The peer's acknowledging close surfaces through Receive the
  // same way. To end a session, Close(); never use destruction as
  // cross-thread cancellation. Close also completes any async operation
  // (below) still parked in the session, the way it unblocks the blocking
  // calls — but only those still parked: a terminal transition that already
  // took a parked completion owns it from then on, and Close can no longer
  // reach it. That distinction is load-bearing for implementors; see
  // TerminalWaiters.
  virtual void Close() = 0;

  // --- Completion-driven twins (ADR-0019) ------------------------------
  //
  // Same outcomes as the blocking calls, delivered to a callback that
  // fires exactly once, on an unspecified thread — the transport's
  // completion context (a Beast io thread; for the in-memory pair,
  // whichever peer thread completed the operation) — or inline on the
  // caller's thread for immediate refusals. At most ONE receive-class and
  // ONE send-class operation may be outstanding per session across the
  // blocking and async APIs together: a second async call completes inline
  // with Error::Validation, while blocking callers keep their
  // serialize-by-waiting behavior. Callback code runs on the wire's
  // threads: never block there.
  //
  // A session that ends while both classes are parked must complete the
  // SEND before the RECEIVE (#173). Completions fire inline on the
  // ending thread, so a receive completion can run an entire session
  // teardown underneath the transition: it resumes the awaiting coroutine,
  // the loop exits, and ~AsyncEventStream drains the ADR-0017 revocation
  // pins. A parked EventStreamHandle::SendAsync holds one of those pins
  // until its completion runs — and once the transition has taken that
  // completion out of the session, Close() can no longer fire it and
  // neither can the write path, so the only thing that can release the pin
  // is the completion the transition is still holding, one frame below the
  // drain, on the very thread that is blocked. No other thread can help:
  // nothing was queued to an executor. A send completion releases and
  // returns; only a receive can end a session, so receives go last.
  // TerminalWaiters below is the enforcement — take into it, fire with it,
  // and the order is not yours to get wrong.
  //
  // The base-class defaults keep every existing implementor compiling:
  // they refuse with Error::Validation and report SupportsAsync() false,
  // so layers above (SessionRegistry's async delivery, the coroutine
  // adapter) can fall back honestly. Overriding them is accepting the
  // contracts above; both in-repo transports do.

  using ReceiveCallback = std::function<void(Outcome<std::optional<eventstream::Message>>)>;
  using SendCallback = std::function<void(Outcome<Unit>)>;

  // The parked completions one session's terminal transition took, and the
  // one safe order to run them in. Implementations move their parked slots
  // in under their own lock (std::exchange, never a bare std::move:
  // libc++'s small-buffer std::function move leaves the source engaged, and
  // an empty slot is what marks the class idle), then release the lock and
  // Fire.
  //
  // Fire is rvalue-only and runs the send first — that ordering is the
  // whole point of the type, so the callbacks are private and Fire is the
  // only way to reach them: there is nothing to fire out of order with.
  // `invoke` receives (label, callback, outcome) and performs the call,
  // which is where a transport that must contain a throwing application
  // callback (ADR-0003) wraps it; a transport with nothing to contain just
  // calls through. Both outcomes are built by the caller because their
  // wording is the session's, not this type's.
  class TerminalWaiters {
   public:
    TerminalWaiters() = default;
    TerminalWaiters(ReceiveCallback receive, SendCallback send)
        : receive_(std::move(receive)), send_(std::move(send)) {}

    template <typename Invoke>
    void Fire(Outcome<Unit> sent, Outcome<std::optional<eventstream::Message>> received,
              Invoke&& invoke) && {
      if (send_) {
        invoke("websocket send", send_, std::move(sent));
      }
      if (receive_) {
        invoke("websocket receive", receive_, std::move(received));
      }
    }

   private:
    ReceiveCallback receive_;
    SendCallback send_;
  };

  // The by-value callbacks are the overriders' contract (they park and
  // later move them); the defaults only refuse.
  // NOLINTBEGIN(performance-unnecessary-value-param)
  virtual void ReceiveAsync(ReceiveCallback callback) {
    callback(Error::Validation(
        "websocket: this implementation has no async operations (SupportsAsync() is the "
        "check; the blocking Receive/Send still work)"));
  }

  // The blocking deadline's completion-driven twin (#130): the callback
  // fires exactly once with the same four outcomes as Receive(timeout) —
  // the message, the peer's clean close (nullopt), the session's permanent
  // error, or Error::Timeout ("TimeoutError") when the budget runs out
  // with nothing to report. Exactly like the blocking overload, a timeout
  // is not terminal: the session is untouched and usable, and the receive
  // slot is released exactly as a completed receive releases it — the next
  // receive (either API) may park again, and a message the wire delivers
  // after the deadline waits in the session for it. A non-positive timeout
  // polls: it completes inline with what is already in hand, or with the
  // timeout. The one-outstanding-receive rule is unchanged.
  //
  // The timeout must race the completion and settle exactly once — the
  // implementation owns that race the same way it owns the parked slot
  // (there is nowhere honest to bound the wait from outside; the blocking
  // overload's doc block explains why). The refusing default keeps every
  // existing implementor compiling; the contract suite holds anything that
  // reports SupportsAsync() to overriding this too.
  virtual void ReceiveAsync(std::chrono::milliseconds timeout, ReceiveCallback callback) {
    (void)timeout;
    callback(Error::Validation(
        "websocket: this implementation has no deadline-bounded async receive (the untimed "
        "ReceiveAsync and the blocking Receive(timeout) may still work)"));
  }

  virtual void SendAsync(const eventstream::Message& message, SendCallback callback) {
    (void)message;
    callback(Error::Validation(
        "websocket: this implementation has no async operations (SupportsAsync() is the "
        "check; the blocking Receive/Send still work)"));
  }
  // NOLINTEND(performance-unnecessary-value-param)

  virtual bool SupportsAsync() const { return false; }
};

// One streaming dial as generated clients describe it (ADR-0016): where to
// connect (host, port, TLS — derived from the client's http(s) endpoint, so
// nothing is configured twice), the upgrade GET's target with its bound
// labels and query, and the headers that ride the upgrade request.
// BeastWebSocketClient::Dialer() consumes it by building Dial's Options;
// custom dialers (ClientConfig::websocket_dialer — how tests run streams
// without Beast) receive it verbatim.
struct WebSocketDialRequest {
  std::string host;
  // 0 means the scheme default: 443 with tls, 80 without.
  int port = 0;
  bool tls = false;
  // Verification knobs when `tls` is true (ClientConfig::tls).
  TlsOptions tls_options;
  // The request target of the upgrade GET (the streaming endpoint).
  std::string target = "/";
  // Extra headers on the upgrade request — bearer tokens, api keys.
  Headers headers;
  // The raw-text wire (ADR-0023): headerless messages ride as verbatim
  // text frames, one JSON-RPC envelope each. Set by generated jsonRpc2
  // streaming clients on every dial — the wire IS the protocol, so there
  // is no negotiation. Custom dialers that carry Message values without a
  // wire (the in-memory pair) ignore it.
  bool raw_text_frames = false;
  // The per-phase budget for the connect, TLS, and upgrade handshakes.
  int handshake_timeout_ms = 30000;
  // After the upgrade: how long a silent connection stays up. Keep-alive
  // pings run underneath, so a healthy-but-quiet stream survives and a
  // vanished peer is detected without any application ping protocol.
  int idle_timeout_seconds = 300;
};

// The dialer a generated streaming client calls: one WebSocketDialRequest
// in, one connected session out (ADR-0016). ClientConfig::websocket_dialer
// carries an injected one; BeastWebSocketClient::Dialer() is the default.
using WebSocketDialer =
    std::function<Outcome<std::shared_ptr<WebSocket>>(const WebSocketDialRequest&)>;

// Dials a WebSocket connection carrying event-stream messages (ADR-0015):
// resolve, connect, TLS (ADR-0007 posture: TLS 1.2 floor, certificate and
// hostname verification on by default, SNI), then the WebSocket upgrade
// handshake — each phase under the handshake_timeout_ms budget (name
// resolution uses the system resolver's own timeout). The returned
// session owns one background io thread for the connection's lifetime.
// Give each thread its own copy of the returned shared_ptr and destroy
// the handle only after every Send/Receive has returned — Close() is the
// cancellation path, destruction is not.
//
//   auto socket = smithy::http::BeastWebSocketClient::Dial({
//       .host = "stream.example.com", .port = 443, .tls = true,
//       .target = "/events",
//   });
//   if (!socket.ok()) { ... }
//   (*socket)->Send(...); (*socket)->Receive();
class BeastWebSocketClient {
 public:
  struct Options {
    std::string host{};
    // 0 means the scheme default: 443 with tls, 80 without.
    int port = 0;
    bool tls = false;
    // Verification knobs when `tls` is true — the same struct ClientConfig
    // carries (beast_transport.h precedent), so wiring cannot drift.
    TlsOptions tls_options{};
    // The request target of the upgrade GET (the streaming endpoint).
    std::string target = "/";
    // Extra headers on the upgrade request — bearer tokens, api keys: the
    // server's websocket_gate sees these before any upgrade completes.
    Headers headers{};
    // Offer the negotiated JSON-text frame mode (ADR-0018) on the dial:
    // an echoed subprotocol selects text framing, no echo falls back to
    // the binary wire silently — both modes carry the same messages, so
    // the difference is invisible above the session. For parity, tooling,
    // and the negotiation tests; browsers are the JSON wire's audience,
    // and native clients should keep the default. A server that answers
    // with a subprotocol never offered fails the dial.
    bool offer_json_frames = false;
    // The raw-text wire (ADR-0023): headerless messages as verbatim text
    // frames, one JSON-RPC envelope each — what a generated jsonRpc2
    // streaming client dials with. Unnegotiated (the wire IS the
    // protocol), and refused together with offer_json_frames.
    bool raw_text_frames = false;
    // The per-phase budget for the connect, TLS, and upgrade handshakes.
    int handshake_timeout_ms = 30000;
    // After the upgrade: how long a silent connection stays up. Keep-alive
    // pings run underneath, so a healthy-but-quiet stream survives and a
    // vanished peer is detected without any application ping protocol.
    int idle_timeout_seconds = 300;
  };

  static Outcome<std::shared_ptr<WebSocket>> Dial(Options options);

  // Dial in WebSocketDialer form — what a generated streaming client uses
  // when ClientConfig::websocket_dialer is unset (ADR-0016). Declared here,
  // implemented in the Beast TU. The link reality: a generated streaming
  // client names this fallback unconditionally, so its binaries ALWAYS
  // link Boost, injected dialer or not — only hand-written wiring that
  // injects a dialer and never mentions this class stays dep-light.
  static WebSocketDialer Dialer();
};

}  // namespace smithy::http

#endif  // SMITHY_HTTP_WEBSOCKET_H_
