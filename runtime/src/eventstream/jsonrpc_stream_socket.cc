#include "smithy/eventstream/jsonrpc_stream_socket.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "smithy/core/blob.h"
#include "smithy/core/error.h"
#include "smithy/eventstream/jsonrpc_frame.h"

namespace smithy::eventstream {
namespace {

using Role = JsonRpcStreamSocket::Role;

// One outbound event as the headerless raw-text Message the wire carries.
Outcome<Message> TranslateOutbound(const Message& message, const Document& id) {
  auto text = EncodeJsonRpcNotification(message, id);
  if (!text.ok()) return std::move(text).error();
  Message raw;
  raw.payload = Blob::FromString(*std::move(text));
  return raw;
}

// One inbound completion, classified per role. `violation_text` is the
// terminal error the server end answers before the close; `close` is the
// fail-closed half both ends share on a violation.
struct Inbound {
  // Explicit: the value state is a deliberate default, not an accident of
  // Outcome's own. NOLINT(readability-redundant-member-init)
  Outcome<std::optional<Message>> result = std::optional<Message>();  // NOLINT
  std::optional<std::string> violation_text;
  bool close = false;
};

Inbound FromViolation(int code, const std::string& reason, const Document& id, Role role) {
  Inbound inbound;
  inbound.result = Error::Serialization("jsonrpc stream: " + reason);
  inbound.close = true;
  if (role == Role::kServer) {
    inbound.violation_text = EncodeJsonRpcViolationResponse(code, reason, id);
  }
  return inbound;
}

Inbound ClassifyInbound(Outcome<std::optional<Message>> raw, const Document& id, Role role) {
  if (!raw.ok()) return {std::move(raw), std::nullopt, false};
  const std::optional<Message>& maybe = *raw;
  if (!maybe.has_value()) return {std::move(raw), std::nullopt, false};
  const Message& message = *maybe;
  if (!message.headers.empty()) {
    // A peer speaking the eventstream envelope wire into a JSON-RPC stream
    // (only the in-memory pair can even deliver this; the raw-text
    // transport yields headerless messages by construction).
    return FromViolation(-32600, "an event-stream framed message on the JSON-RPC text wire", id,
                         role);
  }
  JsonRpcStreamFrame frame = DecodeJsonRpcStreamFrame(
      std::string_view(reinterpret_cast<const char*>(message.payload.data()),
                       message.payload.size()),
      id);
  switch (frame.kind) {
    case JsonRpcStreamFrame::Kind::kEvent:
      return {std::optional<Message>(std::move(frame.message)), std::nullopt, false};
    case JsonRpcStreamFrame::Kind::kException:
      if (role == Role::kServer) {
        return FromViolation(-32600, "a response envelope from the client", id, role);
      }
      // The client's terminal error: exactly the exception Message the
      // downstream decoder turns into the modeled error (ADR-0016).
      return {std::optional<Message>(std::move(frame.message)), std::nullopt, false};
    case JsonRpcStreamFrame::Kind::kResult:
      if (role == Role::kServer) {
        // Terminal response envelopes are server-minted; a client-sent
        // result must not read as the peer's clean close.
        return FromViolation(-32600, "a response envelope from the client", id, role);
      }
      // The clean end — the server closes right behind it, so nothing
      // meaningful can follow.
      return {std::optional<Message>(), std::nullopt, false};
    case JsonRpcStreamFrame::Kind::kViolation:
      return FromViolation(frame.code, frame.reason, id, role);
  }
  return FromViolation(-32600, "unclassifiable frame", id, role);  // unreachable
}

}  // namespace

JsonRpcStreamSocket::JsonRpcStreamSocket(std::shared_ptr<http::WebSocket> inner, Document id,
                                         Role role)
    : owner_(std::move(inner)), inner_(owner_.get()), id_(std::move(id)), role_(role) {}

JsonRpcStreamSocket::JsonRpcStreamSocket(http::WebSocket& inner, Document id, Role role)
    : inner_(&inner), id_(std::move(id)), role_(role) {}

Outcome<std::optional<Message>> JsonRpcStreamSocket::Receive() { return Police(inner_->Receive()); }

Outcome<std::optional<Message>> JsonRpcStreamSocket::Receive(std::chrono::milliseconds timeout) {
  // A timeout arrives as !ok() and rides through untouched (no violation,
  // no close) — the wrapper stays exactly as usable as the socket under it.
  return Police(inner_->Receive(timeout));
}

Outcome<std::optional<Message>> JsonRpcStreamSocket::Police(Outcome<std::optional<Message>> raw) {
  Inbound inbound = ClassifyInbound(std::move(raw), id_, role_);
  if (inbound.violation_text.has_value()) {
    // Best-effort, blocking: Send returns once the frame is on the wire,
    // so the close below cannot cancel it.
    Message terminal;
    terminal.payload = Blob::FromString(*std::move(inbound.violation_text));
    (void)inner_->Send(terminal);
  }
  if (inbound.close) inner_->Close();
  return std::move(inbound.result);
}

Outcome<Unit> JsonRpcStreamSocket::Send(const Message& message) {
  auto raw = TranslateOutbound(message, id_);
  if (!raw.ok()) return std::move(raw).error();
  return inner_->Send(*raw);
}

void JsonRpcStreamSocket::Close() { inner_->Close(); }

void JsonRpcStreamSocket::ReceiveAsync(ReceiveCallback callback) {
  inner_->ReceiveAsync(PoliceAsync(std::move(callback)));
}

void JsonRpcStreamSocket::ReceiveAsync(std::chrono::milliseconds timeout,
                                       ReceiveCallback callback) {
  // A timeout arrives as !ok() and rides through the classifier untouched
  // (no violation, no close) — the wrapper stays exactly as usable as the
  // socket under it, mirroring the blocking deadline overload.
  inner_->ReceiveAsync(timeout, PoliceAsync(std::move(callback)));
}

JsonRpcStreamSocket::ReceiveCallback JsonRpcStreamSocket::PoliceAsync(ReceiveCallback callback) {
  // State travels by value into the completion: the wrapper may be gone by
  // the time the transport completes. The owning form pins the inner
  // socket through `owner`; the borrowing form rides its documented
  // blocking-seam lifetime contract.
  return [id = id_, role = role_, owner = owner_, inner = inner_,
          callback = std::move(callback)](Outcome<std::optional<Message>> raw) mutable {
    Inbound inbound = ClassifyInbound(std::move(raw), id, role);
    if (inbound.violation_text.has_value()) {
      // The close AND the caller's resumption both ride the send's
      // completion: the close cannot cancel the terminal write (the
      // ADR-0021 lesson), and neither can the caller — a generated driver
      // closes the session as it unwinds, which would escalate past the
      // in-flight terminal — because it never runs until the frame is on
      // the wire and the close is already requested.
      auto terminal = std::make_shared<Message>();
      terminal->payload = Blob::FromString(*std::move(inbound.violation_text));
      inner->SendAsync(*terminal,
                       [terminal, owner, inner, result = std::move(inbound.result),
                        callback = std::move(callback)](const Outcome<Unit>& /*sent*/) mutable {
                         inner->Close();
                         callback(std::move(result));
                       });
      return;
    }
    if (inbound.close) inner->Close();
    callback(std::move(inbound.result));
  };
}

void JsonRpcStreamSocket::SendAsync(const Message& message, SendCallback callback) {
  auto raw = TranslateOutbound(message, id_);
  if (!raw.ok()) {
    // Inline refusal, the facade's immediate-refusal shape: the session is
    // untouched and stays usable.
    callback(std::move(raw).error());
    return;
  }
  // The raw message must outlive the call: the facade never promises the
  // transport is done with the reference before the completion fires.
  auto keep_alive = std::make_shared<Message>(*std::move(raw));
  inner_->SendAsync(*keep_alive, [keep_alive, callback = std::move(callback)](Outcome<Unit> sent) {
    callback(std::move(sent));
  });
}

bool JsonRpcStreamSocket::SupportsAsync() const { return inner_->SupportsAsync(); }

}  // namespace smithy::eventstream
