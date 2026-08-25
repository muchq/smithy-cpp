#ifndef SMITHY_EVENTSTREAM_JSONRPC_STREAM_SOCKET_H_
#define SMITHY_EVENTSTREAM_JSONRPC_STREAM_SOCKET_H_

#include <chrono>
#include <memory>
#include <optional>

#include "smithy/core/document.h"
#include "smithy/core/outcome.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"

namespace smithy::eventstream {

// The jsonRpc2 stream translation (ADR-0023), worn as a delegating
// WebSocket: above it, EventStream/AsyncEventStream and generated code
// trade the same envelope-bearing Messages every wire carries; below it,
// headerless raw-text Messages whose payload is one JSON-RPC 2.0 envelope
// per frame. Unlike ADR-0018's transport-internal mode, the wrapper runs
// above the socket — both ends wear one, on both seams, over Beast (whose
// raw-text mode carries the payloads as verbatim text frames) AND the
// in-memory pair (which carries Message values unchanged) — so every
// integration test exercises the actual JSON text.
//
// Sends translate events into notifications echoing the opening call's id;
// exception messages are refused (the generated serve path mints terminal
// envelopes — it owns the @httpError table). Receives classify per role:
// a notification arrives as exactly the event Message the binary wire
// would have carried; on the CLIENT end the terminal error envelope
// arrives as the exception Message (ADR-0016's terminal contract
// downstream applies unchanged) and the terminal result envelope is the
// stream's clean end (it surfaces as nullopt, never as a message).
//
// Envelope-level violations are policed here, per ADR-0023: a frame the
// codec classifies kViolation — and, on the SERVER end, any response
// envelope (terminals are server-minted) — is session-fatal. The server
// answers the reserved-code terminal error for the opening id BEFORE
// closing — the close AND the caller's completion both wait on the send's
// completion, so neither the wrapper's close nor whatever the resumed
// caller does next (a generated driver closes the session as it unwinds)
// can cancel the write; the client just fails closed. Either way the
// caller sees Error::Serialization on a session that is genuinely dead —
// a handler cannot (and need not) distinguish a violating peer from a
// failed wire.
//
// The opening request envelope never passes through here: generated code
// sends and reads it on the inner socket before constructing the wrapper.
// Everything else — one-receiver discipline, one-outstanding-per-class,
// Close-from-any-thread, backpressure — is the inner socket's contract,
// delegated untouched.
class JsonRpcStreamSocket final : public http::WebSocket {
 public:
  // Which end of the wire this wrapper serves. The roles differ only in
  // inbound policing (see above): the server answers violations and
  // refuses response envelopes; the client classifies them.
  enum class Role { kClient, kServer };

  // `id` is the opening call's id, echoed into every notification and
  // matched against every inbound frame.
  JsonRpcStreamSocket(std::shared_ptr<http::WebSocket> inner, Document id, Role role);

  // The borrowing form, mirroring EventStream's borrowed constructor: the
  // blocking serve seam owns its socket only for the route callback's
  // lifetime, and the wrapper must not outlive it — the caller keeps
  // `inner` alive past every call on this object AND past any async
  // completion it armed (on the blocking seam there are none; prefer the
  // owning form anywhere completions outlive the frame).
  JsonRpcStreamSocket(http::WebSocket& inner, Document id, Role role);

  Outcome<std::optional<Message>> Receive() override;
  // The inner session's deadline, worn by the wrapper: a timeout is the
  // inner Error::Timeout verbatim — no envelope classification runs on it,
  // nothing is closed, and the stream picks up where it left off (a
  // violation, by contrast, is still fatal).
  Outcome<std::optional<Message>> Receive(std::chrono::milliseconds timeout) override;
  Outcome<Unit> Send(const Message& message) override;
  void Close() override;
  void ReceiveAsync(ReceiveCallback callback) override;
  // The async deadline (#130), worn by the wrapper the way the blocking
  // one is: a timeout is the inner Error::Timeout verbatim — no envelope
  // classification runs on it, nothing is closed, and the stream picks up
  // where it left off.
  void ReceiveAsync(std::chrono::milliseconds timeout, ReceiveCallback callback) override;
  void SendAsync(const Message& message, SendCallback callback) override;
  bool SupportsAsync() const override;

 private:
  // Both receive overloads: police one inbound outcome per ADR-0023 —
  // answer a violation, close if the frame was fatal, hand back the rest.
  Outcome<std::optional<Message>> Police(Outcome<std::optional<Message>> raw);

  // Both async receives: the classifying completion that Police-es one
  // inbound outcome before handing it to `callback` — shared so the timed
  // and untimed paths cannot drift.
  ReceiveCallback PoliceAsync(ReceiveCallback callback);

  // Engaged only by the owning form; every call goes through inner_.
  std::shared_ptr<http::WebSocket> owner_;
  http::WebSocket* inner_;
  Document id_;
  Role role_;
};

}  // namespace smithy::eventstream

#endif  // SMITHY_EVENTSTREAM_JSONRPC_STREAM_SOCKET_H_
