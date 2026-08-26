#include "smithy/http/beast_transport.h"

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <boost/asio/buffer.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/thread_pool.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/ssl.hpp>
#include <boost/beast/websocket.hpp>
#include <boost/beast/websocket/ssl.hpp>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "smithy/client/config.h"
#include "smithy/core/exception_guard.h"
#include "smithy/eventstream/json_frame.h"
#include "smithy/http/headers.h"
#include "smithy/http/server_dispatch.h"
#include "smithy/http/uri.h"

namespace smithy::http {
namespace {

namespace asio = boost::asio;
namespace beast = boost::beast;
namespace bhttp = boost::beast::http;
namespace bws = boost::beast::websocket;

// The exception backstop for a background io thread (ADR-0003, #109). Beast
// is not -fno-exceptions-clean, so this transport always builds with
// exceptions — and an exception escaping io_context::run() would leave run()
// via std::terminate, killing the whole process along with every other
// connection the thread multiplexes. Completions are contained at their own
// boundaries (handlers via InvokeHandlerGuarded, gates via InvokeGateGuarded,
// application callbacks via InvokeCompletion below), so reaching this loop is
// a last resort — a framework std::bad_alloc under memory pressure, say. Log
// it and re-enter run() so the remaining work drains instead of the process
// dying; a clean drain returns normally and the thread exits.
void RunIoThread(asio::io_context& io, const char* which) {
  for (;;) {
    try {
      io.run();
      return;  // clean drain: no more work, thread exits
    } catch (const std::exception& e) {
      std::clog << "smithy: " << which << " io thread caught: " << e.what()
                << "; re-entering run()\n";
    } catch (...) {
      std::clog << "smithy: " << which
                << " io thread caught a non-std exception; re-entering run()\n";
    }
  }
}

// Contains an application completion callback fired on an io thread. A
// throwing user callback is the application's bug, but per ADR-0003 it must
// not unwind the transport — losing follow-up work queued after the call
// (e.g. re-arming the read pump) or, without RunIoThread, terminating the
// process. Swallow after logging; the connection keeps running.
template <typename Fn, typename... Args>
void InvokeCompletion(const char* which, Fn&& fn, Args&&... args) {
  try {
    std::forward<Fn>(fn)(std::forward<Args>(args)...);
  } catch (const std::exception& e) {
    std::clog << "smithy: " << which << " callback threw: " << e.what() << "\n";
  } catch (...) {
    std::clog << "smithy: " << which << " callback threw a non-std exception\n";
  }
}

HttpRequest ToSmithyRequest(bhttp::request<bhttp::string_body> wire) {
  HttpRequest request;
  request.method = std::string(wire.method_string());
  request.target = std::string(wire.target());
  for (const auto& field : wire) {
    request.headers.Add(std::string(field.name_string()), std::string(field.value()));
  }
  request.body = std::move(wire.body());
  return request;
}

// The transport is authoritative for framing: keep_alive()/prepare_payload()
// below own these fields, and a handler-set copy would ride along beside
// them — a duplicate or conflicting content-length / transfer-encoding is
// the classic request-smuggling pair (the socket server strips the same
// set; issue #46).
bool IsFramingHeader(std::string_view name) {
  return beast::iequals(name, "content-length") || beast::iequals(name, "transfer-encoding") ||
         beast::iequals(name, "connection");
}

bhttp::response<bhttp::string_body> ToWireResponse(HttpResponse response, bool keep_alive) {
  bhttp::response<bhttp::string_body> wire;
  wire.result(static_cast<unsigned>(response.status));
  wire.version(11);
  for (const auto& [name, value] : response.headers.entries()) {
    if (IsFramingHeader(name)) continue;
    wire.insert(name, value);
  }
  wire.body() = std::move(response.body);
  wire.keep_alive(keep_alive);
  wire.prepare_payload();  // sets content-length
  return wire;
}

// The same response as a HEAD answers it: every header the GET would send,
// Content-Length included, and no body (RFC 9110 §9.3.2). Writing the body
// anyway desynchronizes a keep-alive connection, because the peer reads those
// bytes as the start of the next response (issue #192).
//
// A handler cannot do this itself. The body comes from the generated
// serializer, and IsFramingHeader above strips any Content-Length a handler
// sets, so suppressing the body in a handler would answer Content-Length: 0 —
// a different claim about the resource than the GET makes.
//
// empty_body, not a string_body with the body cleared out. Beast's serializer
// writes what the body holds, so a cleared string_body under a Content-Length
// of N is a message it never finishes: the headers go out, the write never
// completes, and the connection sits until the request timeout closes it —
// the next request on it never answered. empty_body carries no body to write
// and takes its length from the header, which is the HEAD contract exactly.
bhttp::response<bhttp::empty_body> ToHeadWireResponse(
    const bhttp::response<bhttp::string_body>& full, bool keep_alive) {
  bhttp::response<bhttp::empty_body> wire;
  wire.result(full.result());
  wire.version(full.version());
  for (const auto& field : full) {
    // Content-Length is restored below from the body the GET would have sent;
    // prepare_payload() may also have chosen to send none (204, 304), and
    // that choice is preserved by not adding one here.
    if (beast::iequals(field.name_string(), "content-length")) continue;
    wire.insert(field.name_string(), field.value());
  }
  wire.keep_alive(keep_alive);
  if (full.find(bhttp::field::content_length) != full.end()) {
    wire.content_length(full.body().size());
  }
  return wire;
}

// The connection's remote peer for the request, rejection, and
// connection-event stamps, rendered by the shared formatter
// (server_dispatch.h) so the transports' stamps cannot drift; empty when
// the socket can no longer report one.
template <typename Stream>
std::string PeerAddressOf(Stream& stream) {
  beast::error_code ec;
  const auto endpoint = beast::get_lowest_layer(stream).socket().remote_endpoint(ec);
  if (ec) return {};
  return FormatPeerAddress(endpoint.data(), static_cast<socklen_t>(endpoint.size()));
}

// Half-closes the connection under any stream type (plain or TLS; TLS skips
// the close_notify exchange, which peers must tolerate per HTTP practice).
template <typename Stream>
void CloseStream(Stream& stream) {
  beast::error_code ignored;
  (void)beast::get_lowest_layer(stream).socket().shutdown(asio::ip::tcp::socket::shutdown_send,
                                                          ignored);
}

// Lingering-close budgets for over-limit rejections (issue #94): after the
// 413/431 is written, read-and-discard at most this much of the remaining
// request before hard-closing. Unbounded draining would be a DoS vector; a
// client that never reads until it finishes sending may still see a reset —
// inherent to the recipe (nginx/envoy behave the same).
constexpr std::size_t kOverLimitDrainBudgetBytes = std::size_t{256} * 1024;
constexpr std::chrono::seconds kOverLimitDrainDeadline{2};

// Stop()'s teardown grace: after io/pool stop, the final joins get this long
// on a reaper thread before a wedged handler's teardown is abandoned and
// deliberately leaked (a thread cannot be killed safely). Worst-case Stop()
// is therefore about drain_timeout_seconds plus this.
constexpr std::chrono::seconds kJoinGrace{2};

// TLS 1.2 cipher policy: ECDHE + AEAD only (Mozilla "intermediate"). TLS 1.3
// suites are not governed by this list and every one of them is acceptable.
constexpr const char* kServerTls12Ciphers =
    "ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-GCM-SHA256:"
    "ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384:"
    "ECDHE-ECDSA-CHACHA20-POLY1305:ECDHE-RSA-CHACHA20-POLY1305";

// The version floor both transports share; it is the single source of truth
// (a floor of 1.2 subsumes the legacy no_sslv2/no_sslv3 option flags).
bool ApplyTls12Floor(asio::ssl::context& ssl_context) {
  ssl_context.set_options(asio::ssl::context::default_workarounds);
  return SSL_CTX_set_min_proto_version(ssl_context.native_handle(), TLS1_2_VERSION) == 1;
}

// ALPN select callback: answer http/1.1 when the client offers it; refuse
// the handshake (RFC 7301 no_application_protocol) when the offer lacks it,
// rather than silently speaking a protocol the client did not agree to. A
// client that sends no ALPN never invokes this callback. `in` is ALPN's wire
// format, length-prefixed protocol names — parsed by hand deliberately:
// SSL_select_next_proto's no-overlap contract is the classic misuse trap.
int SelectHttp11Alpn(SSL* /*ssl*/, const unsigned char** out, unsigned char* out_len,
                     const unsigned char* in, unsigned int in_len, void* /*arg*/) {
  constexpr std::string_view kHttp11 = "http/1.1";
  for (unsigned int i = 0; i < in_len; i += 1 + in[i]) {
    if (i + 1 + in[i] > in_len) break;  // malformed list
    if (std::string_view(reinterpret_cast<const char*>(in + i + 1), in[i]) == kHttp11) {
      *out = in + i + 1;
      *out_len = in[i];
      return SSL_TLSEXT_ERR_OK;
    }
  }
  // Aborts the handshake with a no_application_protocol alert in both
  // BoringSSL and OpenSSL (the former's SSL_TLSEXT_ERR_ALPN_FATAL alias
  // does not exist in the latter).
  return SSL_TLSEXT_ERR_ALERT_FATAL;
}

// The session type both ends share plus the teardown hook the owners need:
// the server transport aborts registered sessions at Stop(); the client
// handle aborts at destruction (which its contract permits only after
// every Send/Receive has returned — Close() is the live cancellation
// path).
class WebSocketSessionBase : public WebSocket {
 public:
  virtual void Abort(std::string reason) = 0;
};

// One live event-stream-over-WebSocket session (ADR-0015). Beast's
// synchronous websocket reads may write control-frame replies, so true full
// duplex cannot be two threads doing sync calls; instead every wire
// operation is asynchronous on the connection's executor and the blocking
// facade waits on it. The facade runs on application threads (the handler
// pool, or any client thread); nothing here blocks an io thread.
//
// Wire-side invariants (Beast's op contract): at most one async_read
// outstanding (the read pump), and at most one write-class op — a message
// write or the close — outstanding (`wire_write_busy_` + the deferred
// `close_requested_`). Once the close op starts it owns the read side too
// (Beast's close drains frames itself until the peer's close arrives), so
// the pump stops arming reads and a read it already had in flight
// completes operation_aborted, which OnRead filters. A Close that finds a
// message write in flight cannot wait for it (the peer may never drain
// it): it cancels the socket's outstanding ops instead, so the write
// completes operation_aborted, OnWrite fails the session, and the blocked
// Send wakes with a transport error (RequestCloseLocked).
// The raw-text wire's "codec" (ADR-0023): the payload IS the frame, one
// JSON-RPC envelope per text message minted by the layers above. Refuses
// what this wire cannot represent — any header at all (there is no header
// channel and no eventstream envelope here) — and the symmetric size
// bound, like every encode.
Outcome<std::string> EncodeRawTextFrame(const eventstream::Message& message) {
  if (!message.headers.empty()) {
    return Error::Validation(
        "websocket: the raw-text wire carries headerless messages only (framing belongs to the "
        "envelope layer above the transport, ADR-0023)");
  }
  if (message.payload.size() > eventstream::kMaxMessageBytes) {
    return Error::Validation("websocket: message over the 16 MiB limit");
  }
  return std::string(reinterpret_cast<const char*>(message.payload.data()), message.payload.size());
}

template <typename WsStream>
class WsSession final : public WebSocketSessionBase,
                        public std::enable_shared_from_this<WsSession<WsStream>> {
 public:
  // `keeper` is whatever must live exactly as long as the connection — for
  // server sessions, the aliasing pointer that holds the transport's
  // connection slot (max_connections accounting fires when this session
  // dies). Declared before ws_ so the socket closes first, then the slot
  // frees.
  WsSession(WsStream ws, std::shared_ptr<void> keeper)
      : keeper_(std::move(keeper)), ws_(std::move(ws)) {}

  WsStream& stream() { return ws_; }

  // Switches the session to the negotiated JSON-text wire (ADR-0018):
  // messages travel as text frames carrying the JSON envelope, and binary
  // frames become the protocol violation. Call before Start() — the wire
  // mode is read concurrently by the pumps and facade afterwards, unlocked.
  void EnableJsonFrames() { wire_ = Wire::kJsonFrames; }

  // Switches the session to the raw-text wire (ADR-0023): headerless
  // messages ride as verbatim text frames, binary frames are the protocol
  // violation. Unnegotiated — the application knows its endpoint speaks
  // JSON-RPC envelopes and says so. Same call-before-Start() rule.
  void EnableRawTextFrames() { wire_ = Wire::kRawText; }

  // Arms the read pump; call once, on the connection's executor.
  void Start() { PumpRead(); }

  Outcome<std::optional<eventstream::Message>> Receive() override {
    return ReceiveWithin(std::nullopt);
  }

  // The deadline overload: same outcomes, plus Error::Timeout when the
  // budget runs out with nothing to report. Only this call ends — the
  // session, the read pump, and the receive slot are untouched.
  Outcome<std::optional<eventstream::Message>> Receive(std::chrono::milliseconds timeout) override {
    return ReceiveWithin(timeout);
  }

  Outcome<std::optional<eventstream::Message>> ReceiveWithin(
      std::optional<std::chrono::milliseconds> timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    // A parked async receive owns the next arrival (Deliver hands it over
    // directly), so a concurrent blocking receiver waits behind it — the
    // serialize-by-waiting half of the ADR-0019 one-outstanding contract.
    auto settled = [this] {
      return (!received_.empty() && !pending_receive_) || peer_closed_ || failed_;
    };
    ++blocked_receivers_;
    bool ready = true;
    if (timeout.has_value()) {
      ready = wake_.wait_for(lock, *timeout, settled);
    } else {
      wake_.wait(lock, settled);
    }
    // Leaving the count is all a timed-out receiver owes the session:
    // nobody waits on it (ReceiveAsync only reads it to refuse), so there
    // is nothing to notify and nothing to unwind.
    --blocked_receivers_;
    if (!ready) {
      return Error::Timeout("websocket: no message within the receive deadline");
    }
    if (!received_.empty() && !pending_receive_) {
      // Messages that arrived before a close or failure still belong to
      // the application, in order.
      eventstream::Message message = std::move(received_.front());
      received_.pop_front();
      ResumeReadsIfPaused();
      return std::optional<eventstream::Message>(std::move(message));
    }
    if (peer_closed_) {
      return std::optional<eventstream::Message>();  // the stream's natural end
    }
    return Error::Transport("websocket: " + error_);
  }

  // The completion-driven twin (ADR-0019): one outstanding receive-class
  // operation per session, completion on the connection's executor.
  void ReceiveAsync(WebSocket::ReceiveCallback callback) override {
    ReceiveAsyncWithin(std::nullopt, std::move(callback));
  }

  // The deadline overload (#130): same immediate paths, and a parked
  // receive races its deadline instead of waiting forever.
  void ReceiveAsync(std::chrono::milliseconds timeout,
                    WebSocket::ReceiveCallback callback) override {
    ReceiveAsyncWithin(timeout, std::move(callback));
  }

  // Both async receives: `timeout` engaged bounds the park with a timer on
  // the connection's executor, disengaged parks until Deliver or a
  // terminal transition. The park generation settles the timer/delivery
  // race exactly once under mutex_: every park bumps it, the timer fires
  // only for the generation it was armed with, so a stale deadline can
  // never time out a later receive (timed or not).
  void ReceiveAsyncWithin(std::optional<std::chrono::milliseconds> timeout,
                          WebSocket::ReceiveCallback callback) {
    Outcome<std::optional<eventstream::Message>> immediate = std::optional<eventstream::Message>();
    bool ready_message = false;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (pending_receive_ || blocked_receivers_ > 0) {
        lock.unlock();
        callback(Error::Validation("websocket: a receive is already outstanding"));
        return;
      }
      if (!received_.empty()) {
        eventstream::Message message = std::move(received_.front());
        received_.pop_front();
        ResumeReadsIfPaused();
        immediate = std::optional<eventstream::Message>(std::move(message));
        ready_message = true;
      } else if (peer_closed_) {
        immediate = std::optional<eventstream::Message>();
      } else if (failed_) {
        immediate = Error::Transport("websocket: " + error_);
      } else if (timeout.has_value() && *timeout <= std::chrono::milliseconds::zero()) {
        // The blocking overload's poll shape: nothing already in hand is a
        // timeout, inline like the other immediates below.
        immediate = Error::Timeout("websocket: no message within the receive deadline");
      } else {
        pending_receive_ = std::move(callback);
        ++receive_park_generation_;  // a stale deadline must not fire this park
        if (timeout.has_value()) {
          try {
            ArmReceiveDeadlineLocked(*timeout);
          } catch (...) {
            // Could not schedule the deadline: an unbounded park would
            // betray the caller's whole ask, so refuse instead.
            WebSocket::ReceiveCallback refused = std::exchange(pending_receive_, nullptr);
            wake_.notify_all();
            lock.unlock();
            refused(Error::Transport("websocket: cannot schedule the receive deadline"));
            return;
          }
        }
        return;  // Deliver / the deadline / the terminal transitions complete it
      }
    }
    if (!ready_message) {
      // Terminal outcomes fire inline — the documented immediate path. A
      // post here could race Stop(): a stopped executor destroys queued
      // handlers unexecuted, which would strand the caller's continuation
      // (a Detached loop's frame) forever.
      callback(std::move(immediate));
      return;
    }
    // Ready results complete on the connection's executor, so a callback
    // that immediately re-arms cannot recurse into the session. The lambda
    // copies the callback (no move): if post throws, the catch still owns
    // a live one to refuse with.
    auto self = this->shared_from_this();
    try {
      asio::post(ws_.get_executor(), [self, callback, immediate = std::move(immediate)]() mutable {
        callback(std::move(immediate));
      });
    } catch (...) {
      callback(Error::Transport("websocket: cannot schedule the completion"));
    }
  }

  // With mutex_ held: (re)arms the one receive-deadline timer for the park
  // that just went in. Emplacing destroys a previous timer, cancelling its
  // pending wait (the handler sees operation_aborted and returns) — and the
  // generation check makes even an uncancelled stale wait harmless. The
  // handler captures the session weakly: a deadline must never extend a
  // session's life.
  void ArmReceiveDeadlineLocked(std::chrono::milliseconds timeout) {
    // Saturate far-future deadlines (milliseconds::max() as "practically
    // forever") instead of overflowing the timer's now + duration into the
    // past and firing a spurious instant timeout.
    constexpr std::chrono::milliseconds kMaxWait = std::chrono::hours(24 * 365);
    receive_deadline_.emplace(ws_.get_executor());
    receive_deadline_->expires_after(std::min(timeout, kMaxWait));
    receive_deadline_->async_wait(
        [weak = this->weak_from_this(),
         generation = receive_park_generation_](const boost::system::error_code& ec) {
          if (ec) return;  // cancelled: a newer park re-armed, or teardown
          if (auto self = weak.lock()) self->OnReceiveDeadline(generation);
        });
  }

  void OnReceiveDeadline(std::uint64_t generation) {
    WebSocket::ReceiveCallback expired;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!pending_receive_ || receive_park_generation_ != generation) {
        return;  // the park this deadline bounded already completed
      }
      expired = std::exchange(pending_receive_, nullptr);
      // A blocking receiver may be waiting behind the parked slot; the
      // freed slot is part of its wake condition.
      wake_.notify_all();
    }
    // The timer fired on the connection's executor — the completion
    // context — so contain a throwing application callback (ADR-0003).
    InvokeCompletion("websocket receive", expired,
                     Outcome<std::optional<eventstream::Message>>(
                         Error::Timeout("websocket: no message within the receive deadline")));
  }

  Outcome<Unit> Send(const eventstream::Message& message) override {
    auto frame = EncodeForWire(message);
    if (!frame.ok()) {
      return std::move(frame).error();  // the codec's Validation, verbatim
    }
    // Serializes concurrent senders; the wire itself allows one write op.
    const std::lock_guard<std::mutex> send_turn(send_mutex_);
    std::unique_lock<std::mutex> lock(mutex_);
    // An async send may be in flight (send_mutex_ only serializes blocking
    // callers); wait it out — serialize-by-waiting, per ADR-0019.
    wake_.wait(lock, [this] {
      return (write_complete_ && !pending_send_) || failed_ || peer_closed_ || close_requested_;
    });
    if (failed_ || peer_closed_ || close_requested_) {
      return Error::Transport("websocket: " +
                              (failed_ ? error_ : std::string("session is closed")));
    }
    write_complete_ = false;
    write_error_.clear();
    lock.unlock();
    auto self = this->shared_from_this();
    try {
      asio::post(ws_.get_executor(), [self, frame = std::move(*frame)]() mutable {
        self->StartWrite(std::move(frame));
      });
    } catch (...) {
      return Error::Transport("websocket: cannot schedule the write");
    }
    lock.lock();
    wake_.wait(lock, [this] { return write_complete_ || failed_; });
    if (write_complete_ && write_error_.empty()) {
      return Unit{};
    }
    return Error::Transport("websocket: " + (write_error_.empty() ? error_ : write_error_));
  }

  // The completion-driven twin (ADR-0019): one outstanding send-class
  // operation per session; OnWrite fires the completion on the
  // connection's executor.
  void SendAsync(const eventstream::Message& message, WebSocket::SendCallback callback) override {
    auto frame = EncodeForWire(message);
    if (!frame.ok()) {
      callback(std::move(frame).error());  // the codec's Validation, inline
      return;
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (failed_ || peer_closed_ || close_requested_) {
        const std::string why = failed_ ? error_ : std::string("session is closed");
        lock.unlock();
        callback(Error::Transport("websocket: " + why));
        return;
      }
      if (!write_complete_ || pending_send_) {
        lock.unlock();
        callback(Error::Validation("websocket: a send is already in flight"));
        return;
      }
      pending_send_ = std::move(callback);
      write_complete_ = false;
      write_error_.clear();
    }
    auto self = this->shared_from_this();
    try {
      asio::post(ws_.get_executor(), [self, frame = std::move(*frame)]() mutable {
        self->StartWrite(std::move(frame));
      });
    } catch (...) {
      WebSocket::SendCallback cb;
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        cb = std::exchange(pending_send_, nullptr);
        write_complete_ = true;
        wake_.notify_all();
      }
      if (cb) {
        cb(Error::Transport("websocket: cannot schedule the write"));
      }
    }
  }

  bool SupportsAsync() const override { return true; }

  void Close() override {
    auto self = this->shared_from_this();
    try {
      asio::post(ws_.get_executor(), [self] {
        std::unique_lock<std::mutex> lock(self->mutex_);
        self->RequestCloseLocked(lock);
      });
    } catch (...) {
      Abort("websocket: cannot schedule the close");
    }
  }

  // Fails both facade calls immediately and closes the socket out from
  // under any outstanding op — Stop()-time and client-teardown semantics,
  // safe from any thread even when the io context is already stopped.
  // Fail() is the whole failure story (an already-terminal session has no
  // parked waiters to complete); Abort adds only the socket close.
  void Abort(std::string reason) override {
    Fail(std::move(reason));
    auto self = this->shared_from_this();
    try {
      asio::post(ws_.get_executor(), [self] {
        beast::error_code ignored;
        (void)beast::get_lowest_layer(self->ws_).socket().close(ignored);
      });
    } catch (...) {
      // io already gone; the socket closes with the session.
    }
  }

 private:
  static constexpr std::size_t kReceiveQueueDepth = 8;

  // --- everything below runs on the connection's executor ---

  void PumpRead() {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (failed_ || peer_closed_ || close_started_) {
        return;  // ended, or the close op owns the read side now
      }
      if (received_.size() >= kReceiveQueueDepth) {
        // Backpressure: stop reading until Receive drains the queue; TCP
        // then pushes back on the peer. ResumeReadsIfPaused re-arms.
        read_paused_ = true;
        return;
      }
    }
    auto self = this->shared_from_this();
    ws_.async_read(read_buffer_,
                   [self](beast::error_code ec, std::size_t n) { self->OnRead(ec, n); });
  }

  void OnRead(beast::error_code ec, std::size_t n) {
    if (ec == bws::error::closed) {
      AsyncWaiters waiters;
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        peer_closed_ = true;
        waiters = TakeAsyncWaitersLocked();
        wake_.notify_all();
      }
      CompleteAsyncWaiters(std::move(waiters), /*clean=*/true, "");
      return;
    }
    if (ec) {
      {
        // Our own close displaces the outstanding read: Beast's close op
        // takes over the read side to finish the closing handshake itself,
        // and a Close that escalated past an in-flight write cancelled the
        // socket's ops outright (RequestCloseLocked). Neither abort is an
        // outcome — the close/write completions decide clean-versus-error.
        const std::lock_guard<std::mutex> lock(mutex_);
        if (close_requested_ && ec == asio::error::operation_aborted) {
          return;
        }
      }
      Fail(ec.message());
      return;
    }
    const auto data = read_buffer_.data();
    const std::string_view bytes(static_cast<const char*>(data.data()), data.size());
    if (wire_ == Wire::kRawText) {
      // The raw-text wire (ADR-0023): the text frame IS the message
      // payload, headerless — the JSON-RPC envelope layer above decodes
      // it. A binary message is the protocol violation here.
      if (ws_.got_binary()) {
        FailAndClose("binary message on a raw-text websocket");
        return;
      }
      eventstream::Message message;
      message.payload = Blob::FromString(std::string(bytes));
      Deliver(std::move(message), n);
      return;
    }
    if (wire_ == Wire::kJsonFrames) {
      // The negotiated JSON-text wire (ADR-0018): one JSON envelope per
      // text message; a binary message is the protocol violation here —
      // the exact mirror of binary mode's posture on text.
      if (ws_.got_binary()) {
        FailAndClose("binary message on a JSON-text event-stream socket");
        return;
      }
      auto decoded = eventstream::DecodeJsonFrame(bytes);
      if (!decoded.ok()) {
        FailAndClose(decoded.error().message());
        return;
      }
      Deliver(std::move(*decoded), n);
      return;
    }
    if (!ws_.got_binary()) {
      FailAndClose("text message on an event-stream socket");
      return;
    }
    auto decoded = eventstream::DecodeMessage(bytes);
    // Exactly one frame per binary message (ADR-0015): WebSocket framing
    // provides the boundaries, so a partial, trailing-bytes, or malformed
    // payload is a protocol violation, not a feed-me-more.
    if (!decoded.ok()) {
      FailAndClose(decoded.error().message());
      return;
    }
    std::optional<eventstream::DecodedFrame>& frame = *decoded;
    if (!frame.has_value() || frame->bytes_consumed != bytes.size()) {
      FailAndClose("binary message is not exactly one event-stream frame");
      return;
    }
    Deliver(std::move(frame->message), n);
  }

  // Hands one decoded message to a parked async receive, else queues it
  // for the blocking facade, and re-arms the pump.
  void Deliver(eventstream::Message message, std::size_t bytes_read) {
    read_buffer_.consume(bytes_read);
    WebSocket::ReceiveCallback receive;
    std::optional<eventstream::Message> handoff;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (pending_receive_) {
        receive = std::exchange(pending_receive_, nullptr);
        handoff.emplace(std::move(message));
      } else {
        received_.push_back(std::move(message));
        wake_.notify_all();
      }
    }
    if (receive) {
      // The pump runs on the connection's executor — this IS the completion
      // context, so contain a throwing app callback here rather than let it
      // abandon PumpRead() below or unwind the io thread (ADR-0003).
      InvokeCompletion("websocket receive", receive, std::move(handoff));
    }
    PumpRead();
  }

  // Takes the parked async completions; called with mutex_ held on every
  // terminal transition, fired by the caller after unlocking. std::exchange
  // (never a bare std::move: libc++'s small-buffer std::function move
  // leaves the source still engaged) empties the slots, so a completion
  // can never fire twice and the slots' emptiness stays the busy signal.
  using AsyncWaiters = WebSocket::TerminalWaiters;
  AsyncWaiters TakeAsyncWaitersLocked() {
    return {std::exchange(pending_receive_, nullptr), std::exchange(pending_send_, nullptr)};
  }

  // Fires the taken completions with the session's terminal outcome:
  // nullopt for a clean end, the recorded error otherwise. TerminalWaiters
  // owns the send-before-receive order (websocket.h, #173); the
  // invoker is where a throwing application callback is contained, since
  // this runs on an io thread (ADR-0003).
  void CompleteAsyncWaiters(AsyncWaiters waiters, bool clean, const std::string& reason) {
    Outcome<std::optional<eventstream::Message>> received =
        clean ? Outcome<std::optional<eventstream::Message>>(std::optional<eventstream::Message>())
              : Outcome<std::optional<eventstream::Message>>(
                    Error::Transport("websocket: " + reason));
    std::move(waiters).Fire(
        Error::Transport("websocket: " + (clean ? "session is closed" : reason)),
        std::move(received), [](const char* which, const auto& callback, auto outcome) {
          InvokeCompletion(which, callback, std::move(outcome));
        });
  }

  void StartWrite(std::string frame) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (failed_ || peer_closed_ || close_requested_) {
      write_complete_ = true;
      write_error_ = failed_ ? error_ : "session is closed";
      WebSocket::SendCallback send = std::exchange(pending_send_, nullptr);
      const std::string why = write_error_;
      wake_.notify_all();
      lock.unlock();
      if (send) {
        InvokeCompletion("websocket send", send, Error::Transport("websocket: " + why));
      }
      return;
    }
    wire_write_busy_ = true;
    write_buffer_ = std::move(frame);
    lock.unlock();
    ws_.binary(wire_ == Wire::kBinary);  // text frames on the JSON and raw-text wires
    auto self = this->shared_from_this();
    ws_.async_write(asio::buffer(write_buffer_),
                    [self](beast::error_code ec, std::size_t) { self->OnWrite(ec); });
  }

  void OnWrite(beast::error_code ec) {
    WebSocket::SendCallback send;
    std::string send_error;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      wire_write_busy_ = false;
      write_complete_ = true;
      if (ec) {
        // Close's escalation cancels an in-flight write (the peer may never
        // drain it); name that for the Send caller instead of the raw
        // operation_aborted text.
        write_error_ = close_requested_ && ec == asio::error::operation_aborted
                           ? "session closed while a send was in flight"
                           : ec.message();
        if (!failed_ && !peer_closed_) {
          failed_ = true;  // a failed write means the wire is gone for both directions
          error_ = write_error_;
        }
      }
      send = std::exchange(pending_send_, nullptr);
      send_error = write_error_;
      wake_.notify_all();
      if (!ec && close_requested_ && !close_started_) {
        StartCloseLocked(lock);  // leaves the lock released
      }
    }
    if (send) {
      // On the connection's executor — the completion context.
      if (send_error.empty()) {
        InvokeCompletion("websocket send", send, Unit{});
      } else {
        InvokeCompletion("websocket send", send, Error::Transport("websocket: " + send_error));
      }
    }
  }

  // Records the close request. On an idle wire the close op starts now
  // (close is a write-class op on this wire). An in-flight message write
  // cannot simply be deferred behind — the peer may have stopped reading
  // and the write may never complete, and Close's contract is to unblock a
  // blocked Send — so Close escalates: cancel the socket's outstanding ops,
  // the write completes operation_aborted, and OnWrite fails the session
  // and wakes the Send with a transport error. The receiver then observes
  // an error rather than a clean nullopt — honest for a wire aborted
  // mid-frame. When the write races the cancel and completes cleanly,
  // OnWrite runs the deferred close instead and the session still ends
  // with the normal close handshake.
  void RequestCloseLocked(std::unique_lock<std::mutex>& lock) {
    if (close_requested_ || failed_ || peer_closed_) {
      return;
    }
    close_requested_ = true;
    if (!wire_write_busy_) {
      StartCloseLocked(lock);
      return;
    }
    lock.unlock();
    beast::error_code ignored;
    (void)beast::get_lowest_layer(ws_).socket().cancel(ignored);
  }

  void StartCloseLocked(std::unique_lock<std::mutex>& lock) {
    close_started_ = true;
    const bws::close_code code = close_code_;
    lock.unlock();
    auto self = this->shared_from_this();
    // Beast's close op performs the whole closing handshake (it reads
    // until the peer's close arrives, then tears down): its success IS
    // the session's clean end.
    ws_.async_close(code, [self](beast::error_code ec) {
      AsyncWaiters waiters;
      bool clean = false;
      std::string why;
      {
        const std::lock_guard<std::mutex> lock(self->mutex_);
        if (!self->failed_ && !self->peer_closed_) {
          if (ec) {
            self->failed_ = true;
            self->error_ = ec.message();
          } else {
            self->peer_closed_ = true;
          }
        }
        waiters = self->TakeAsyncWaitersLocked();
        clean = !self->failed_;
        why = self->error_;
        self->wake_.notify_all();
      }
      self->CompleteAsyncWaiters(std::move(waiters), clean, why);
    });
  }

  void Fail(std::string reason) {
    AsyncWaiters waiters;
    std::string why;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (!failed_ && !peer_closed_) {
        failed_ = true;
        error_ = std::move(reason);
      }
      waiters = TakeAsyncWaitersLocked();
      why = error_;
      wake_.notify_all();
    }
    CompleteAsyncWaiters(std::move(waiters), /*clean=*/false, why);
  }

  // A protocol violation by the peer: fail the session for the
  // application, and tell the peer why — as a protocol_error close frame,
  // deferred behind any in-flight write (OnWrite runs the deferred close).
  void FailAndClose(std::string reason) {
    AsyncWaiters waiters;
    std::string why;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!failed_ && !peer_closed_) {
        failed_ = true;
        error_ = std::move(reason);
      }
      waiters = TakeAsyncWaitersLocked();
      why = error_;
      wake_.notify_all();
      close_code_ = bws::close_code::protocol_error;
      close_requested_ = true;
      if (!close_started_ && !wire_write_busy_) {
        StartCloseLocked(lock);  // leaves the lock released
      }
    }
    CompleteAsyncWaiters(std::move(waiters), /*clean=*/false, why);
  }

  // Called with mutex_ held, from the facade side.
  void ResumeReadsIfPaused() {
    if (!read_paused_ || failed_ || peer_closed_ || received_.size() >= kReceiveQueueDepth) {
      return;
    }
    read_paused_ = false;
    auto self = this->shared_from_this();
    try {
      asio::post(ws_.get_executor(), [self] { self->PumpRead(); });
    } catch (...) {
      failed_ = true;
      error_ = "cannot schedule the read";
      wake_.notify_all();
    }
  }

  // The session's wire encoding: one event-stream frame per binary message
  // (the default), the negotiated JSON envelope on text frames (ADR-0018),
  // or verbatim raw text (ADR-0023). Set once before Start(), read-only
  // afterwards — safe unlocked from the pumps and the facade.
  enum class Wire { kBinary, kJsonFrames, kRawText };

  Outcome<std::string> EncodeForWire(const eventstream::Message& message) const {
    switch (wire_) {
      case Wire::kJsonFrames:
        return eventstream::EncodeJsonFrame(message);
      case Wire::kRawText:
        return EncodeRawTextFrame(message);
      case Wire::kBinary:
        break;
    }
    return eventstream::EncodeMessage(message);
  }

  std::shared_ptr<void> keeper_;
  WsStream ws_;
  Wire wire_ = Wire::kBinary;
  beast::flat_buffer read_buffer_;
  std::string write_buffer_;

  std::mutex send_mutex_;  // serializes Send callers end to end

  std::mutex mutex_;
  std::condition_variable wake_;
  std::deque<eventstream::Message> received_;
  // The ADR-0019 parked completions: at most one of each; every terminal
  // transition takes and fires them exactly once (TakeAsyncWaitersLocked).
  WebSocket::ReceiveCallback pending_receive_;
  WebSocket::SendCallback pending_send_;
  // Bumped on every pending_receive_ park; a receive deadline fires only
  // for the generation it was armed with (the timed-receive race settles
  // here). The timer is optional so it can be (re)constructed on the
  // connection's executor per timed park.
  std::uint64_t receive_park_generation_ = 0;
  std::optional<asio::steady_timer> receive_deadline_;
  int blocked_receivers_ = 0;
  bool read_paused_ = false;
  bool peer_closed_ = false;  // clean close: Receive's nullopt
  bool failed_ = false;       // broken/violated/aborted: Receive's error
  std::string error_;
  bool write_complete_ = true;
  std::string write_error_;
  bool wire_write_busy_ = false;
  bool close_requested_ = false;
  bool close_started_ = false;
  bws::close_code close_code_ = bws::close_code::normal;
};

// The websocket_gate and on_websocket callbacks are contained the way
// InvokeHandlerGuarded contains handlers: application policy must never
// take down the wire it runs on. A gate that throws refuses the upgrade
// with a 500 — never a completed handshake by accident.
std::optional<HttpResponse> InvokeGateGuarded(
    const std::function<std::optional<HttpResponse>(const HttpRequest&)>& gate,
    const HttpRequest& request) {
  if (!gate) {
    return std::nullopt;
  }
  try {
    return gate(request);
  } catch (const std::exception& e) {
    std::clog << "smithy: websocket_gate threw: " << e.what() << "\n";
  } catch (...) {
    std::clog << "smithy: websocket_gate threw a non-std exception\n";
  }
  HttpResponse refusal;
  refusal.status = 500;
  return refusal;
}

void InvokeServeGuarded(const std::function<void(const HttpRequest&, WebSocket&)>& serve,
                        const HttpRequest& request, WebSocket& socket) {
  try {
    serve(request, socket);
  } catch (const std::exception& e) {
    std::clog << "smithy: on_websocket threw: " << e.what() << "\n";
  } catch (...) {
    std::clog << "smithy: on_websocket threw a non-std exception\n";
  }
}

void InvokeServeSessionGuarded(
    const std::function<void(const HttpRequest&, std::shared_ptr<WebSocket>)>& serve,
    const HttpRequest& request, std::shared_ptr<WebSocket> socket) {
  try {
    serve(request, std::move(socket));
  } catch (const std::exception& e) {
    std::clog << "smithy: on_websocket_session threw: " << e.what() << "\n";
  } catch (...) {
    std::clog << "smithy: on_websocket_session threw a non-std exception\n";
  }
}

// The ADR-0018 subprotocol token as the Beast string type, for 101
// decorators and handshake-response comparisons.
beast::string_view JsonFramesToken() {
  return {eventstream::kJsonFramesSubprotocol.data(), eventstream::kJsonFramesSubprotocol.size()};
}

// True when the upgrade request offers the ADR-0018 JSON-frames
// subprotocol: any position in Sec-WebSocket-Protocol's comma-separated
// list, across repeated headers. Tokens are case-sensitive (RFC 6455) and
// unknown ones are simply not selected — the 101 then carries no
// subprotocol and a client that required one fails the connection itself.
bool OffersJsonFrames(const HttpRequest& request) {
  for (const std::string& value : request.headers.GetAll("sec-websocket-protocol")) {
    for (const std::string& token : SplitHeaderListValues(value)) {
      if (token == eventstream::kJsonFramesSubprotocol) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace

// Shared by the acceptor loop, sessions, and the transport object; sessions
// may outlive Stop() briefly, so everything they touch lives here.
struct BeastServerTransport::State : std::enable_shared_from_this<State> {
  explicit State(const Options& options)
      : io(options.threads), acceptor(asio::make_strand(io)), opts(options) {}

  asio::io_context io;
  // With handlers on their own pool, the io_context can go momentarily
  // workless — accept paused at the connection cap, a request's read done,
  // its write not yet started because the handler is still running — and
  // io_context::run() would return, killing every io thread mid-request.
  // The guard keeps them in run() until Shutdown's explicit io.stop(),
  // which overrides it.
  asio::executor_work_guard<asio::io_context::executor_type> work_guard{asio::make_work_guard(io)};
  asio::ip::tcp::acceptor acceptor;
  Options opts;
  RequestHandler handler;
  // Engaged when handler_threads > 0; created by Start, used by Dispatch.
  std::unique_ptr<asio::thread_pool> handler_pool;
  // Engaged when TLS termination is configured (Start validated cert + key).
  std::optional<asio::ssl::context> ssl;
  std::atomic<bool> stopping{false};
  // Requests read off the wire whose responses are not fully written yet;
  // Stop() drains until this reaches zero (or the drain deadline passes).
  std::atomic<int> active{0};

  // Accept-limit state, confined to the acceptor's strand (accept
  // completions and the posted session-closed notifications both run
  // there), so plain non-atomic values are safe.
  std::size_t open_connections = 0;
  bool accept_paused = false;

  // Live upgraded sessions (ADR-0015): Stop() aborts them so serve
  // callbacks blocked in Send/Receive wake and the handler pool can join.
  // Weak entries — the sessions' own lifetime stays with their pumps.
  // The aborted flag closes the Stop()-vs-registration race: a session
  // whose accept completes after the one abort sweep ran must self-abort,
  // or its serve callback would block forever and wedge the pool join.
  std::mutex websockets_mutex;
  std::vector<std::weak_ptr<WebSocketSessionBase>> websockets;
  bool websockets_aborted = false;
  std::string websockets_abort_reason;

  void RegisterWebSocket(const std::shared_ptr<WebSocketSessionBase>& session) {
    std::string abort_reason;
    {
      const std::lock_guard<std::mutex> lock(websockets_mutex);
      if (!websockets_aborted) {
        std::erase_if(websockets, [](const auto& weak) { return weak.expired(); });
        websockets.push_back(session);
        return;
      }
      abort_reason = websockets_abort_reason;
    }
    session->Abort(std::move(abort_reason));  // lost the race with the sweep
  }

  void AbortWebSockets(const std::string& reason) {
    std::vector<std::shared_ptr<WebSocketSessionBase>> live;
    {
      const std::lock_guard<std::mutex> lock(websockets_mutex);
      websockets_aborted = true;
      websockets_abort_reason = reason;
      for (const auto& weak : websockets) {
        if (auto session = weak.lock()) {
          live.push_back(std::move(session));
        }
      }
    }
    for (const auto& session : live) {
      session->Abort(reason);
    }
  }

  // All completion handlers capture State weakly: handlers queued inside the
  // io_context must not own the State that owns the io_context, or abandoned
  // handlers at shutdown would form a reference cycle and leak everything.

  // A connection's stream plus its max_connections accounting. Handlers
  // hold the stream through an aliasing shared_ptr; when the last one lets
  // go the session is destroyed — which is when the fd truly closes, so the
  // count bounds fds, not requests — and the destructor posts the slot
  // release to the acceptor strand, resuming a paused accept loop.
  template <typename Stream>
  struct Session {
    template <typename... Args>
    explicit Session(std::weak_ptr<State> owner, Args&&... args)
        : state(std::move(owner)), stream(std::forward<Args>(args)...) {}
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;
    ~Session() {
      auto self = state.lock();
      if (self == nullptr) {
        return;  // Transport already torn down; nothing left to resume.
      }
      try {
        asio::post(self->acceptor.get_executor(), [weak = std::move(state)] {
          auto locked = weak.lock();
          if (locked != nullptr) {
            locked->OnSessionClosed();
          }
        });
      } catch (...) {
        // Allocation failure posting the release leaks one slot; the
        // destructor must not throw.
      }
    }
    std::weak_ptr<State> state;
    Stream stream;
  };

  void Accept() {
    if (opts.max_connections > 0 && open_connections >= opts.max_connections) {
      // At the cap: stop accepting instead of allocating another session.
      // New connections queue in the kernel's listen backlog (bounded; SYNs
      // beyond it drop and retry) — genuine backpressure under a connection
      // flood (issue #46). OnSessionClosed re-arms when a slot frees up.
      accept_paused = true;
      return;
    }
    acceptor.async_accept(
        asio::make_strand(io),
        [weak = weak_from_this()](beast::error_code ec, asio::ip::tcp::socket socket) {
          auto self = weak.lock();
          if (self == nullptr || ec || self->stopping) {
            return;  // Acceptor closed or shutting down.
          }
          self->RunSession(std::move(socket));
          self->Accept();
        });
  }

  // Posted to the acceptor strand by ~Session when a connection's stream is
  // destroyed (its fd is gone).
  void OnSessionClosed() {
    --open_connections;
    if (accept_paused && !stopping) {
      accept_paused = false;
      Accept();
    }
  }

  void RunSession(asio::ip::tcp::socket socket) {
    ++open_connections;  // Released by ~Session when the fd closes.
    if (!ssl.has_value()) {
      auto session =
          std::make_shared<Session<beast::tcp_stream>>(weak_from_this(), std::move(socket));
      ReadNext(std::shared_ptr<beast::tcp_stream>(session, &session->stream));
      return;
    }
    auto session = std::make_shared<Session<asio::ssl::stream<beast::tcp_stream>>>(
        weak_from_this(), std::move(socket), *ssl);
    std::shared_ptr<asio::ssl::stream<beast::tcp_stream>> stream(session, &session->stream);
    beast::get_lowest_layer(*stream).expires_after(
        std::chrono::seconds(opts.request_timeout_seconds));
    auto& stream_ref = *stream;
    stream_ref.async_handshake(
        asio::ssl::stream_base::server,
        [weak = weak_from_this(), stream, phase = Phase(*stream)](beast::error_code ec) mutable {
          auto self = weak.lock();
          if (self == nullptr) {
            return;
          }
          if (ec) {
            // Drop the connection — observed only for handshakes that went
            // wrong, not connections that went away (ADR-0013): TCP health
            // probes and scanners connect and leave (eof/stream_truncated)
            // or idle into the deadline, and that noise would scale with
            // infrastructure, not incidents. A handshake that exchanged
            // bytes and failed — plaintext to the TLS port, a
            // version/cipher/ALPN mismatch — is the misrouting alarm.
            const bool probe_shape = ec == asio::error::eof ||
                                     ec == asio::ssl::error::stream_truncated ||
                                     ec == beast::error::timeout;
            if (!probe_shape) {
              using Kind = BeastServerTransport::ConnectionEvent::Kind;
              self->NotifyConnectionEvent(Kind::kTlsHandshakeFailure, ec, std::move(phase));
            }
            return;
          }
          self->ReadNext(stream);
        });
  }

  // One observation per transport-written rejection (Options::on_rejected):
  // called before the 413/431 is written, with whatever the parser got to.
  // Contained like the middleware hooks — an observer must never take down
  // the rejection path it is watching.
  template <typename Stream>
  void NotifyRejected(Stream& stream, const bhttp::request<bhttp::string_body>& partial,
                      bhttp::status status) {
    if (!opts.on_rejected) {
      return;
    }
    const BeastServerTransport::RejectedRequest rejected{
        .status = static_cast<int>(status),
        .peer_address = PeerAddressOf(stream),
        .method = std::string(partial.method_string()),
        .target = std::string(partial.target())};
    try {
      opts.on_rejected(rejected);
    } catch (const std::exception& e) {
      std::clog << "smithy: on_rejected observer threw: " << e.what() << "\n";
    } catch (...) {
      std::clog << "smithy: on_rejected observer threw a non-std exception\n";
    }
  }

  // A wire phase's identity, captured when the phase BEGINS: by the time a
  // failure completes, the deadline machinery may already have closed the
  // socket and getpeername has nothing to report (a timed-out stream is
  // closed before its handler runs). The peer lookup happens only when the
  // hook is installed, so the unobserved path pays nothing.
  struct PhaseStart {
    std::string peer;
    std::chrono::steady_clock::time_point at;
  };
  template <typename Stream>
  PhaseStart Phase(Stream& stream) const {
    return {opts.on_connection_event ? PeerAddressOf(stream) : std::string(),
            std::chrono::steady_clock::now()};
  }

  // One observation per transport-terminated connection (ADR-0013,
  // Options::on_connection_event), contained like NotifyRejected. Nothing
  // is reported while stopping: shutdown cancellations are lifecycle, not
  // incident.
  void NotifyConnectionEvent(BeastServerTransport::ConnectionEvent::Kind kind,
                             const beast::error_code& ec, PhaseStart phase) const {
    if (!opts.on_connection_event || stopping) {
      return;
    }
    const BeastServerTransport::ConnectionEvent event{
        .kind = kind,
        .peer_address = std::move(phase.peer),
        .detail = ec.message(),
        .elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - phase.at)};
    try {
      opts.on_connection_event(event);
    } catch (const std::exception& e) {
      std::clog << "smithy: on_connection_event observer threw: " << e.what() << "\n";
    } catch (...) {
      std::clog << "smithy: on_connection_event observer threw a non-std exception\n";
    }
  }

  // Classifies a failed request read (ADR-0013's taxonomy). Silent for the
  // healthy shapes: a clean close at a message boundary, and a timeout with
  // nothing received — idle keep-alive reaping, indistinguishable from a
  // healthy client leaving.
  void NotifyReadFailure(const beast::error_code& ec, bool got_some, PhaseStart phase) const {
    using Kind = BeastServerTransport::ConnectionEvent::Kind;
    if (ec == bhttp::error::end_of_stream) {
      return;
    }
    if (ec == asio::ssl::error::stream_truncated) {
      // A TLS peer that skips close_notify — as this repo's own client and
      // CloseStream do, tolerated HTTP practice — surfaces here rather than
      // as end_of_stream. Same shape as a plain close: silent between
      // messages, a drop mid-message.
      if (got_some) {
        NotifyConnectionEvent(Kind::kDropped, ec, std::move(phase));
      }
      return;
    }
    if (ec == beast::error::timeout) {
      if (got_some) {
        NotifyConnectionEvent(Kind::kReadTimeout, ec, std::move(phase));
      }
      return;
    }
    if (ec == bhttp::error::partial_message) {
      NotifyConnectionEvent(Kind::kDropped, ec, std::move(phase));
      return;
    }
    // The rest of Beast's HTTP error category is the parse family; anything
    // from another category is transport-level (reset, broken pipe, ...).
    const bool parse_error =
        ec.category() == bhttp::make_error_code(bhttp::error::bad_method).category();
    NotifyConnectionEvent(parse_error ? Kind::kFramingError : Kind::kDropped, ec, std::move(phase));
  }

  // Answers an over-limit request with a minimal 413/431 + Connection: close,
  // then performs a bounded lingering close: half-close the write side and
  // read-and-discard the request's remainder within a small time/byte budget
  // before fully closing. Closing with unread bytes in the kernel's receive
  // buffer would trigger an RST that can destroy the already-sent response in
  // flight — the drain is what makes the status readable (RFC 9112 §9.6;
  // issue #94). One absolute deadline covers the write and the whole drain.
  template <typename Stream>
  void RejectOverLimit(const std::shared_ptr<Stream>& stream, bhttp::status status) {
    auto response = std::make_shared<bhttp::response<bhttp::string_body>>();
    response->result(status);
    response->version(11);
    response->keep_alive(false);
    response->set(bhttp::field::content_type, "text/plain");
    response->body() = status == bhttp::status::payload_too_large
                           ? "request body exceeds the server's limit"
                           : "request headers exceed the server's limit";
    response->prepare_payload();
    beast::get_lowest_layer(*stream).expires_after(kOverLimitDrainDeadline);
    auto& stream_ref = *stream;
    auto& response_ref = *response;
    bhttp::async_write(
        stream_ref, response_ref,
        [weak = weak_from_this(), stream, response](beast::error_code write_ec, std::size_t) {
          CloseStream(*stream);  // half-close: no more writes either way
          auto self = weak.lock();
          if (self == nullptr || write_ec) {
            beast::error_code ignored;
            (void)beast::get_lowest_layer(*stream).socket().close(ignored);
            return;
          }
          self->DrainThenClose(stream, kOverLimitDrainBudgetBytes);
        });
  }

  // Discards raw bytes from the lowest layer (works under TLS too — the
  // discarded ciphertext never needs decrypting) until EOF, error, deadline
  // (set by RejectOverLimit), or budget exhaustion, then closes the socket.
  template <typename Stream>
  void DrainThenClose(const std::shared_ptr<Stream>& stream, std::size_t budget) {
    auto scratch = std::make_shared<std::array<char, 8192>>();
    beast::get_lowest_layer(*stream).async_read_some(
        asio::buffer(*scratch),
        [weak = weak_from_this(), stream, scratch, budget](beast::error_code ec, std::size_t n) {
          auto self = weak.lock();
          if (self == nullptr || ec || n >= budget) {
            beast::error_code ignored;
            (void)beast::get_lowest_layer(*stream).socket().close(ignored);
            return;
          }
          self->DrainThenClose(stream, budget - n);
        });
  }

  template <typename Stream>
  void ReadNext(const std::shared_ptr<Stream>& stream) {
    auto buffer = std::make_shared<beast::flat_buffer>();
    auto parser = std::make_shared<bhttp::request_parser<bhttp::string_body>>();
    parser->body_limit(opts.max_body_bytes);
    parser->header_limit(static_cast<std::uint32_t>(opts.max_header_bytes));
    beast::get_lowest_layer(*stream).expires_after(
        std::chrono::seconds(opts.request_timeout_seconds));
    auto& stream_ref = *stream;
    bhttp::async_read(stream_ref, *buffer, *parser,
                      [weak = weak_from_this(), stream, buffer, parser, phase = Phase(*stream)](
                          beast::error_code ec, std::size_t) mutable {
                        auto self = weak.lock();
                        if (self == nullptr) {
                          CloseStream(*stream);
                          return;
                        }
                        if (ec == bhttp::error::body_limit || ec == bhttp::error::header_limit) {
                          // Over-limit requests get a real status before the close instead
                          // of a bare connection abort (issue #94); every other read error
                          // keeps the close-only path below.
                          const bhttp::status status =
                              ec == bhttp::error::body_limit
                                  ? bhttp::status::payload_too_large
                                  : bhttp::status::request_header_fields_too_large;
                          self->NotifyRejected(*stream, parser->get(), status);
                          self->RejectOverLimit(stream, status);
                          return;
                        }
                        if (ec) {
                          self->NotifyReadFailure(ec, parser->got_some(), std::move(phase));
                          CloseStream(*stream);
                          return;
                        }
                        if ((self->opts.on_websocket || self->opts.on_websocket_session) &&
                            bws::is_upgrade(parser->get())) {
                          if (self->stopping) {
                            // No new streams during the drain (the normal
                            // path folds stopping into keep_alive the same
                            // way).
                            CloseStream(*stream);
                            return;
                          }
                          // The upgrade path (ADR-0015): the request is fully
                          // read and within the transport's limits; the gate
                          // and handshake take it from here. Not a tracked
                          // request — a stream has no pending response.
                          self->Upgrade(stream, parser);
                          return;
                        }
                        self->active.fetch_add(1);
                        const bool keep_alive = parser->get().keep_alive() && !self->stopping;
                        HttpRequest request = ToSmithyRequest(parser->release());
                        // Per request rather than per connection (with the
                        // event hook installed, Phase() above adds a second
                        // lookup per read arm — the price of observability);
                        // Dispatch threads this stamp to the write-phase
                        // event, where a fresh lookup could already be
                        // empty.
                        request.peer_address = PeerAddressOf(*stream);
                        self->Dispatch(stream, std::move(request), keep_alive);
                      });
  }

  // Runs the handler — on the handler pool when configured, else inline on
  // the io thread that completed the read — then hands the response to
  // Respond on the connection's strand. Stream internals (including the
  // per-request timer armed in ReadNext, whose expiry handler runs on that
  // strand) are only ever touched there, so the pool thread never races the
  // stream. InvokeHandlerGuarded contains any exception the handler throws
  // as a 500 — otherwise it would unwind out of the executor and terminate
  // the process, dropping every in-flight request.
  template <typename Stream>
  void Dispatch(const std::shared_ptr<Stream>& stream, HttpRequest request, bool keep_alive) {
    // The request's own peer stamp rides along for the write-phase event:
    // by write time the peer may already be gone (an RST while the handler
    // ran) and a fresh getpeername would come back empty. The method rides
    // along for the same reason — framing needs it (issue #192) and the
    // request is gone by write time.
    std::string peer = request.peer_address;
    // Exact, not case-insensitive: RFC 9110 §9.1 makes the method token
    // case-sensitive, and Router::Route looks its bucket up by exact string
    // (router.cc). Matching loosely here would strip the body from a "head"
    // request that routing had already treated as some other method.
    const bool head = request.method == "HEAD";
    if (handler_pool == nullptr) {
      Respond(stream, InvokeHandlerGuarded(handler, std::move(request)), keep_alive,
              std::move(peer), head);
      return;
    }
    asio::post(*handler_pool, [weak = weak_from_this(), stream, request = std::move(request),
                               keep_alive, peer = std::move(peer), head]() mutable {
      // A handler-pool worker has no RunIoThread backstop, so contain any
      // throw here — the handler itself is already contained by
      // InvokeHandlerGuarded, leaving only a framework bad_alloc, which must
      // not terminate the process (ADR-0003).
      InvokeCompletion("beast handler dispatch", [&] {
        auto self = weak.lock();
        if (self == nullptr) {
          return;  // Torn down; the abandoned stream closes the fd.
        }
        HttpResponse response = InvokeHandlerGuarded(self->handler, std::move(request));
        asio::post(stream->get_executor(), [weak, stream, response = std::move(response),
                                            keep_alive, peer = std::move(peer), head]() mutable {
          auto self = weak.lock();
          if (self == nullptr) {
            CloseStream(*stream);
            return;
          }
          self->Respond(stream, std::move(response), keep_alive, std::move(peer), head);
        });
      });
    });
  }

  // By value, like ToWireResponse below it: every caller is done with the
  // response, so the body — handler-produced, so as large as the handler
  // makes it (max_body_bytes caps only the inbound direction) — moves
  // through to the wire object instead of being copied per request.
  template <typename Stream>
  void Respond(const std::shared_ptr<Stream>& stream, HttpResponse response, bool keep_alive,
               std::string peer, bool head) {
    if (FindUnsafeHeader(response.headers).has_value()) {
      // The header-injection defense (issue #109): a handler echoing
      // CR/LF-bearing text into a header is response splitting. The whole
      // response is replaced — fixed text, nothing echoed — loudly enough
      // that the handler bug gets found instead of shipped.
      response = HttpResponse{};
      response.status = 500;
      response.headers.Set("content-type", "text/plain");
      response.body = "internal error: response header contains forbidden bytes";
    }
    auto full = ToWireResponse(std::move(response), keep_alive);
    if (head) {
      // Same headers, no body (issue #192). Its own message type, because a
      // string_body emptied under a non-zero Content-Length is a message
      // Beast never finishes writing, which strands the connection instead of
      // serving the next request on it.
      WriteWire(stream,
                std::make_shared<bhttp::response<bhttp::empty_body>>(
                    ToHeadWireResponse(full, keep_alive)),
                keep_alive, std::move(peer));
      return;
    }
    WriteWire(stream, std::make_shared<bhttp::response<bhttp::string_body>>(std::move(full)),
              keep_alive, std::move(peer));
  }

  // The write itself, shared by both response shapes above: the completion
  // arm — drop accounting, keep-alive teardown, the next read — is the same
  // whether or not a body follows the headers.
  template <typename Stream, typename Message>
  void WriteWire(const std::shared_ptr<Stream>& stream, std::shared_ptr<Message> wire,
                 bool keep_alive, std::string peer) {
    // Each wire phase gets its own request_timeout_seconds budget: Beast
    // expiries are absolute and outlive the op, so without a re-arm the
    // write would run under the deadline set at READ start — and a handler
    // outrunning the residue would have its response cancelled and the
    // healthy, waiting peer misreported as a drop (ADR-0013). Handler time
    // is bounded by the drain/executor policy, not the wire deadline.
    beast::get_lowest_layer(*stream).expires_after(
        std::chrono::seconds(opts.request_timeout_seconds));
    auto& wire_stream = *stream;
    auto& wire_ref = *wire;
    bhttp::async_write(wire_stream, wire_ref,
                       [weak = weak_from_this(), stream, wire, keep_alive,
                        phase = PhaseStart{std::move(peer), std::chrono::steady_clock::now()}](
                           beast::error_code write_ec, std::size_t) mutable {
                         auto self = weak.lock();
                         if (self != nullptr) {
                           self->active.fetch_sub(1);
                         }
                         if (self == nullptr || write_ec || !keep_alive) {
                           if (self != nullptr && write_ec) {
                             // The peer vanished mid-response (ADR-0013).
                             using Kind = BeastServerTransport::ConnectionEvent::Kind;
                             self->NotifyConnectionEvent(Kind::kDropped, write_ec,
                                                         std::move(phase));
                           }
                           CloseStream(*stream);
                           return;
                         }
                         self->ReadNext(stream);
                       });
  }

  // The WebSocket upgrade path (ADR-0015), entered from ReadNext's success
  // arm on the connection strand. The gate is application policy and may
  // block, so it runs on the handler pool; the handshake and session
  // machinery stay on the strand.
  template <typename Stream>
  void Upgrade(const std::shared_ptr<Stream>& stream,
               const std::shared_ptr<bhttp::request_parser<bhttp::string_body>>& parser) {
    // The smithy view of the upgrade request, for the gate and the serve
    // callback; the wire request object stays alive in the parser for
    // async_accept.
    HttpRequest request = ToSmithyRequest(bhttp::request<bhttp::string_body>(parser->get()));
    request.peer_address = PeerAddressOf(*stream);
    asio::post(*handler_pool, [weak = weak_from_this(), stream, parser,
                               request = std::move(request)]() mutable {
      // Handler-pool worker: no RunIoThread backstop, and the gate is already
      // contained by InvokeGateGuarded, so contain any residual (framework)
      // throw here to keep a pool worker from terminating the process.
      InvokeCompletion("beast upgrade dispatch", [&] {
        auto self = weak.lock();
        if (self == nullptr) {
          return;  // Torn down; the abandoned stream closes the fd.
        }
        std::optional<HttpResponse> refusal = InvokeGateGuarded(self->opts.websocket_gate, request);
        asio::post(stream->get_executor(), [weak, stream, parser, request = std::move(request),
                                            refusal = std::move(refusal)]() mutable {
          auto self = weak.lock();
          if (self == nullptr || self->stopping) {
            // Torn down, or Stop() began while the gate ran: no new streams.
            CloseStream(*stream);
            return;
          }
          if (refusal.has_value()) {
            // Refused before any 101 exists: the answer is a plain HTTP
            // response on the plain HTTP connection, keep-alive intact.
            self->active.fetch_add(1);
            const bool keep_alive = parser->get().keep_alive() && !self->stopping;
            // An upgrade request is a GET by definition (RFC 6455 §4.1), so
            // the refusal is never a HEAD response.
            self->Respond(stream, *std::move(refusal), keep_alive, std::move(request.peer_address),
                          /*head=*/false);
            return;
          }
          self->CompleteUpgrade(stream, parser, std::move(request));
        });
      });
    });
  }

  template <typename Stream>
  void CompleteUpgrade(const std::shared_ptr<Stream>& stream,
                       const std::shared_ptr<bhttp::request_parser<bhttp::string_body>>& parser,
                       HttpRequest request) {
    auto phase = Phase(*stream);  // before the move: the socket must still answer getpeername
    // The websocket stream owns the wire's timers from here (handshake
    // budget + idle-with-pings); the HTTP per-phase deadline must not keep
    // ticking underneath it.
    beast::get_lowest_layer(*stream).expires_never();
    using Ws = bws::stream<Stream>;
    // The session adopts the stream; the aliasing `stream` pointer rides
    // along as the keeper, so the transport's connection slot stays held
    // exactly until the session dies (~Session accounting, unchanged).
    auto session = std::make_shared<WsSession<Ws>>(Ws(std::move(*stream)), stream);
    session->stream().set_option(bws::stream_base::timeout{
        .handshake_timeout = std::chrono::seconds(std::max(opts.request_timeout_seconds, 1)),
        .idle_timeout = std::chrono::seconds(std::max(opts.websocket_idle_timeout_seconds, 1)),
        .keep_alive_pings = true});
    // The transport refuses what the codec could not represent — the
    // symmetric-bounds line (ADR-0014), extended to the wire.
    session->stream().read_message_max(eventstream::kMaxMessageBytes);
    if (opts.websocket_raw_text_frames) {
      // The raw-text wire (ADR-0023): unnegotiated — the whole listener
      // speaks it (Start refuses it beside the JSON-frames negotiation),
      // headerless 101 included, so a browser connects with plain
      // `new WebSocket(url)`.
      session->EnableRawTextFrames();
    } else if (opts.websocket_accept_json_frames && OffersJsonFrames(request)) {
      // The negotiated JSON-text mode (ADR-0018), on the accept-decorator
      // seam ADR-0015 reserved: echo the token in the 101 and flip the
      // session's wire encoding before the pump arms.
      session->EnableJsonFrames();
      session->stream().set_option(bws::stream_base::decorator([](bws::response_type& response) {
        response.set(bhttp::field::sec_websocket_protocol, JsonFramesToken());
      }));
    }
    session->stream().async_accept(
        parser->get(), [weak = weak_from_this(), session, parser, request = std::move(request),
                        phase = std::move(phase)](beast::error_code ec) mutable {
          auto self = weak.lock();
          if (self == nullptr) {
            return;
          }
          if (ec) {
            // The gate admitted it and the handshake still failed: the
            // 101 never completed, no response will ever exist — ADR-0013's
            // exact contract, as the ADR-0015 kind.
            using Kind = BeastServerTransport::ConnectionEvent::Kind;
            self->NotifyConnectionEvent(Kind::kUpgradeFailure, ec, std::move(phase));
            return;
          }
          if (self->stopping) {
            // The 101 completed as Stop() began; the abort sweep may
            // already have run, so this session must not wait for it.
            session->Abort("server stopping");
            return;
          }
          self->RegisterWebSocket(session);
          session->Start();  // on the strand: the accept completion runs here
          asio::post(*self->handler_pool, [weak, session, request = std::move(request)]() mutable {
            auto self = weak.lock();
            if (self == nullptr) {
              session->Abort("websocket: server torn down");
              return;
            }
            if (self->opts.on_websocket_session) {
              // The shared seam (ADR-0019): the callback owns the session
              // from here and its return ends nothing — the session lives
              // until a Close, the idle timeout, or Stop()'s abort sweep
              // (it registered above like every session).
              InvokeServeSessionGuarded(self->opts.on_websocket_session, request,
                                        std::move(session));
              return;
            }
            InvokeServeGuarded(self->opts.on_websocket, request, *session);
            // Serve returned: the session ends with a close handshake; the
            // pumps wind down and the connection slot frees with the
            // session.
            session->Close();
          });
        });
  }
};

BeastServerTransport::BeastServerTransport(Options options) : options_(std::move(options)) {}

BeastServerTransport::~BeastServerTransport() { Shutdown(); }

Outcome<Unit> BeastServerTransport::Start(RequestHandler handler) {
  // No exception may cross this Outcome boundary (ADR-0003). The throw vector
  // is resource construction — a std::thread ctor failing with EAGAIN under
  // load, a bad_alloc — which Contain turns into a transport Error rather
  // than unwinding into the caller. The destructor's Shutdown() joins any
  // threads that did start.
  return smithy::internal::Contain(
      [&]() -> Outcome<Unit> { return StartContained(std::move(handler)); },
      [](const char* what) -> Outcome<Unit> {
        return Error::Transport(std::string("beast: start failed: ") +
                                (what != nullptr ? what : "unknown exception"));
      });
}

Outcome<Unit> BeastServerTransport::StartContained(RequestHandler handler) {
  if (state_ != nullptr) {
    return Error::Validation("beast: transport already started");
  }
  auto state = std::make_shared<State>(options_);
  state->handler = std::move(handler);

  if (options_.on_websocket && options_.on_websocket_session) {
    return Error::Validation(
        "beast: on_websocket and on_websocket_session cannot both be set (the borrowed and "
        "shared serve seams cannot both claim an upgrade)");
  }
  const bool serves_websockets = options_.on_websocket || options_.on_websocket_session;
  if (serves_websockets && options_.handler_threads <= 0) {
    // The borrowed serve callback blocks by design, and even the shared
    // launch callback is application code; inline-on-io would wedge the
    // wire it serves.
    return Error::Validation(
        "beast: on_websocket / on_websocket_session needs handler_threads > 0");
  }
  if (options_.websocket_gate && !serves_websockets) {
    // The gate only guards the upgrade path, which exists only with a
    // serve callback — a gate alone would be silently dead config.
    return Error::Validation(
        "beast: websocket_gate without on_websocket / on_websocket_session never runs");
  }
  if (options_.websocket_accept_json_frames && !serves_websockets) {
    // Same dead-config shape: negotiation happens on the upgrade path.
    return Error::Validation(
        "beast: websocket_accept_json_frames without on_websocket / on_websocket_session never "
        "negotiates");
  }
  if (options_.websocket_raw_text_frames && !serves_websockets) {
    // Same dead-config shape again: the wire mode applies on upgrades only.
    return Error::Validation(
        "beast: websocket_raw_text_frames without on_websocket / on_websocket_session never "
        "applies");
  }
  if (options_.websocket_raw_text_frames && options_.websocket_accept_json_frames) {
    // One listener speaks one wire family: raw text is unnegotiated and
    // claims every upgrade, so the JSON-frames negotiation could never win.
    return Error::Validation(
        "beast: websocket_raw_text_frames and websocket_accept_json_frames cannot both be set "
        "(raw text claims every upgrade; nothing is left to negotiate)");
  }

  const bool has_cert = !options_.tls_certificate_chain_pem.empty();
  const bool has_key = !options_.tls_private_key_pem.empty();
  if (has_cert != has_key) {
    return Error::Validation(
        "beast: TLS termination needs both tls_certificate_chain_pem and tls_private_key_pem");
  }
  if (has_cert) {
    asio::ssl::context& ssl_context = state->ssl.emplace(asio::ssl::context::tls_server);
    boost::system::error_code ssl_ec;
    (void)ssl_context.use_certificate_chain(asio::buffer(options_.tls_certificate_chain_pem),
                                            ssl_ec);
    if (!ssl_ec) {
      (void)ssl_context.use_private_key(asio::buffer(options_.tls_private_key_pem),
                                        asio::ssl::context::pem, ssl_ec);
    }
    if (ssl_ec) {
      return Error::Validation("beast: invalid TLS certificate/key: " + ssl_ec.message());
    }
    // The fixed server TLS posture — floor, ciphers, ALPN. Deliberately not
    // knobs; the rationale lives on the Options doc (beast_transport.h).
    if (!ApplyTls12Floor(ssl_context) ||
        SSL_CTX_set_cipher_list(ssl_context.native_handle(), kServerTls12Ciphers) != 1) {
      return Error::Validation("beast: cannot apply the TLS posture (version floor/ciphers)");
    }
    SSL_CTX_set_alpn_select_cb(ssl_context.native_handle(), SelectHttp11Alpn, nullptr);
  }

  boost::system::error_code ec;
  const auto address = asio::ip::make_address(options_.address, ec);
  if (ec) {
    return Error::Validation("beast: invalid bind address: " + options_.address);
  }
  const asio::ip::tcp::endpoint endpoint(address, static_cast<unsigned short>(options_.port));
  (void)state->acceptor.open(endpoint.protocol(), ec);
  if (!ec) {
    (void)state->acceptor.set_option(asio::socket_base::reuse_address(true), ec);
  }
  if (!ec) {
    (void)state->acceptor.bind(endpoint, ec);
  }
  if (!ec) {
    (void)state->acceptor.listen(asio::socket_base::max_listen_connections, ec);
  }
  if (ec) {
    return Error::Transport("beast: cannot listen on " + options_.address + ":" +
                            std::to_string(options_.port) + ": " + ec.message());
  }

  if (options_.handler_threads > 0) {
    // Created only now, after the listener is up: a Start that fails
    // validation or bind/listen never spawns handler threads.
    state->handler_pool =
        std::make_unique<asio::thread_pool>(static_cast<std::size_t>(options_.handler_threads));
  }
  state->Accept();
  state_ = state;
  const int threads = options_.threads > 0 ? options_.threads : 1;
  threads_.reserve(static_cast<std::size_t>(threads));
  for (int i = 0; i < threads; ++i) {
    threads_.emplace_back([state] { RunIoThread(state->io, "beast server"); });
  }
  return Unit{};
}

int BeastServerTransport::port() const {
  if (state_ == nullptr) {
    return 0;
  }
  boost::system::error_code ec;
  const auto endpoint = state_->acceptor.local_endpoint(ec);
  return ec ? 0 : endpoint.port();
}

void BeastServerTransport::Stop() { Shutdown(); }

void BeastServerTransport::Shutdown() noexcept {
  if (state_ == nullptr) {
    return;
  }
  state_->stopping = true;
  std::shared_ptr<std::vector<std::thread>> orphans;
  const auto join_all = [](std::vector<std::thread>& threads, State& state) {
    for (std::thread& thread : threads) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    if (state.handler_pool != nullptr) {
      state.handler_pool->join();
    }
  };
  try {
    // Stop accepting first: close the acceptor on its strand (the accept
    // loop may be touching it on an io thread). The posted closure captures
    // State weakly so an abandoned handler cannot keep State alive.
    asio::post(state_->acceptor.get_executor(), [weak = std::weak_ptr<State>(state_)] {
      auto self = weak.lock();
      if (self != nullptr) {
        boost::system::error_code ignored;
        (void)self->acceptor.close(ignored);
      }
    });
    // Drain: requests already read get up to drain_timeout_seconds to finish
    // writing; keep-alive is off (stopping), so served sessions then close.
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(std::max(state_->opts.drain_timeout_seconds, 0));
    while (state_->active.load() > 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Live stream sessions never count toward the drain (they have no
    // pending response); abort them now so serve callbacks blocked in
    // Send/Receive wake with an error and the pool below can join
    // (ADR-0015).
    state_->AbortWebSockets("server stopping");
    // Past the deadline: abandon handler work that hasn't started yet
    // (running handlers finish; their responses are dropped once io stops).
    if (state_->handler_pool != nullptr) {
      state_->handler_pool->stop();
    }
    // Once no io thread is running, nothing may be posted into the
    // io_context (an abandoned handler owning State would form a reference
    // cycle and leak everything).
    state_->io.stop();
    // The joins are unbounded — the stops above do not interrupt an
    // executing handler, and a thread cannot be killed safely — so a reaper
    // thread performs them and Stop() waits only kJoinGrace. Normal case:
    // the joins finish in microseconds and the reaper is joined here.
    // Wedged case: the reaper is detached and State deliberately leaks; if
    // the handler ever returns, the reaper finishes the cleanup. The
    // shared_ptr captures (never moves of raw members) keep a throwing
    // std::thread constructor from unwinding through joinable threads.
    orphans = std::make_shared<std::vector<std::thread>>(std::move(threads_));
    std::promise<void> reaped;
    std::future<void> done = reaped.get_future();
    std::thread reaper([reaped = std::move(reaped), state = state_, orphans, join_all]() mutable {
      join_all(*orphans, *state);
      reaped.set_value();
    });
    if (done.wait_for(kJoinGrace) == std::future_status::ready) {
      reaper.join();
    } else {
      // An unhandled wedge must never be silent (the server_dispatch
      // convention): this line is the operator's only trace of the leak.
      std::clog << "smithy: Stop() grace expired with a handler still running; "
                   "abandoning teardown (state deliberately leaks; it is "
                   "reclaimed if the handler ever returns)\n"
                << std::flush;
      reaper.detach();
    }
  } catch (...) {
    // Teardown or reaper spawn failed: join inline (unbounded fallback), on
    // whichever container holds the threads at the point of the throw.
    join_all(threads_, *state_);
    if (orphans != nullptr) {
      join_all(*orphans, *state_);
    }
  }
  threads_.clear();
  state_.reset();
}

// ---------------------------------------------------------------------------
// BeastHttpClient
// ---------------------------------------------------------------------------

namespace {

// Client-side TLS context setup shared by the HTTP client and the
// WebSocket dial (ADR-0007 posture): version floor, verify mode, trust
// anchors. Returns an error message; empty is success.
std::string SetupClientTlsContext(asio::ssl::context& ssl_context, const TlsOptions& tls) {
  // A client that verifies peers by default has no business completing a
  // downgraded handshake either.
  if (!ApplyTls12Floor(ssl_context)) {
    return "cannot set the TLS 1.2 version floor";
  }
  boost::system::error_code ec;
  if (!tls.verify_peer) {
    (void)ssl_context.set_verify_mode(asio::ssl::verify_none, ec);
    return "";
  }
  (void)ssl_context.set_verify_mode(asio::ssl::verify_peer, ec);
  if (ec) {
    return "cannot enable TLS verification: " + ec.message();
  }
  if (!tls.ca_pem.empty()) {
    (void)ssl_context.add_certificate_authority(asio::buffer(tls.ca_pem), ec);
    if (ec) {
      return "invalid ca_pem: " + ec.message();
    }
  } else {
    // Best effort: platforms without system OpenSSL paths simply fail
    // verification at handshake time.
    (void)ssl_context.set_default_verify_paths(ec);
  }
  return "";
}

// One dialed connection. Beast's stream timeouts only apply to asynchronous
// operations, so Send() runs async chains and drives the connection's own
// io_context to completion — which also makes concurrent Send() calls
// independent (each uses a distinct connection).
struct ClientConnection {
  ClientConnection() : io(1) {}

  asio::io_context io;
  // Exactly one of these is set, chosen when the connection is dialed.
  std::unique_ptr<beast::tcp_stream> plain;
  std::unique_ptr<asio::ssl::stream<beast::tcp_stream>> tls;

  beast::tcp_stream& lowest() const {
    return tls != nullptr ? beast::get_lowest_layer(*tls) : *plain;
  }

  // Runs the handlers queued so far to completion, then rearms for reuse.
  void Run() {
    io.run();
    io.restart();
  }
};

}  // namespace

struct BeastHttpClient::State {
  explicit State(Options options) : opts(std::move(options)) {
    if (!opts.tls) {
      return;
    }
    const std::string error =
        SetupClientTlsContext(ssl.emplace(asio::ssl::context::tls_client), opts.tls_options);
    if (!error.empty()) {
      setup_error = "beast client: " + error;
    }
  }

  Options opts;
  std::optional<asio::ssl::context> ssl;
  std::string setup_error;
  std::mutex mutex;
  std::vector<std::unique_ptr<ClientConnection>> idle;

  std::chrono::milliseconds Timeout() const {
    return std::chrono::milliseconds(std::max(opts.request_timeout_ms, 1));
  }

  std::unique_ptr<ClientConnection> TakeIdle() {
    const std::lock_guard<std::mutex> lock(mutex);
    if (idle.empty()) {
      return nullptr;
    }
    auto connection = std::move(idle.back());
    idle.pop_back();
    return connection;
  }

  void ReturnIdle(std::unique_ptr<ClientConnection> connection) {
    const std::lock_guard<std::mutex> lock(mutex);
    if (idle.size() < opts.max_idle_connections) {
      idle.push_back(std::move(connection));
    }
  }

  Outcome<std::unique_ptr<ClientConnection>> Dial() {
    auto connection = std::make_unique<ClientConnection>();
    asio::ip::tcp::resolver resolver(connection->io);
    boost::system::error_code resolve_ec;
    const auto results = resolver.resolve(opts.host, std::to_string(opts.port), resolve_ec);
    if (resolve_ec) {
      return Error::Transport("beast client: cannot resolve " + opts.host + ": " +
                              resolve_ec.message());
    }
    if (opts.tls) {
      if (!ssl.has_value()) {
        return Error::Validation("beast client: TLS context was not configured");
      }
      asio::ssl::context& ssl_context = *ssl;
      connection->tls =
          std::make_unique<asio::ssl::stream<beast::tcp_stream>>(connection->io, ssl_context);
      // SNI: virtual-hosted servers need the name before the handshake.
      if (SSL_set_tlsext_host_name(connection->tls->native_handle(), opts.host.c_str()) != 1) {
        return Error::Transport("beast client: cannot set SNI host name");
      }
      if (opts.tls_options.verify_peer) {
        boost::system::error_code verify_ec;
        (void)connection->tls->set_verify_callback(asio::ssl::host_name_verification(opts.host),
                                                   verify_ec);
        if (verify_ec) {
          return Error::Transport("beast client: cannot enable hostname verification: " +
                                  verify_ec.message());
        }
      }
    } else {
      connection->plain = std::make_unique<beast::tcp_stream>(connection->io);
    }

    connection->lowest().expires_after(Timeout());
    beast::error_code connect_ec;
    connection->lowest().async_connect(
        results,
        [&connect_ec](beast::error_code ec, const asio::ip::tcp::endpoint&) { connect_ec = ec; });
    connection->Run();
    if (connect_ec) {
      return Error::Transport("beast client: cannot connect to " + opts.host + ":" +
                              std::to_string(opts.port) + ": " + connect_ec.message());
    }
    if (opts.tls) {
      connection->lowest().expires_after(Timeout());
      beast::error_code handshake_ec;
      connection->tls->async_handshake(
          asio::ssl::stream_base::client,
          [&handshake_ec](beast::error_code ec) { handshake_ec = ec; });
      connection->Run();
      if (handshake_ec) {
        // Verification failures are configuration/identity problems, not
        // transient transport blips: not retryable.
        return Error::Transport(
            "beast client: TLS handshake with " + opts.host + " failed: " + handshake_ec.message(),
            /*retryable=*/false);
      }
    }
    return connection;
  }

  bhttp::request<bhttp::string_body> ToWireRequest(const HttpRequest& request) const {
    bhttp::request<bhttp::string_body> wire;
    wire.method_string(request.method);
    wire.target(request.target.empty() ? "/" : request.target);
    wire.version(11);
    const int default_port = opts.tls ? 443 : 80;
    wire.set(bhttp::field::host,
             opts.port == default_port ? opts.host : opts.host + ":" + std::to_string(opts.port));
    for (const auto& [name, value] : request.headers.entries()) {
      wire.insert(name, value);
    }
    wire.body() = request.body;
    wire.keep_alive(true);
    wire.prepare_payload();
    return wire;
  }

  // One request/response over the connection. `stale` reports failures that
  // look like a dead keep-alive connection (safe to retry on a fresh dial);
  // `keep_alive` reports whether the response permits reusing the connection.
  Outcome<HttpResponse> RoundTrip(ClientConnection& connection,
                                  const bhttp::request<bhttp::string_body>& wire, bool* stale,
                                  bool* keep_alive) const {
    *stale = false;
    *keep_alive = false;
    connection.lowest().expires_after(Timeout());
    beast::error_code write_ec;
    auto write_handler = [&write_ec](beast::error_code ec, std::size_t) { write_ec = ec; };
    if (connection.tls != nullptr) {
      bhttp::async_write(*connection.tls, wire, write_handler);
    } else {
      bhttp::async_write(*connection.plain, wire, write_handler);
    }
    connection.Run();
    if (write_ec) {
      // A reused connection the server already closed fails the write.
      *stale = true;
      return Error::Transport("beast client: write failed: " + write_ec.message());
    }

    beast::flat_buffer buffer;
    bhttp::response_parser<bhttp::string_body> parser;
    parser.body_limit(boost::none);  // Clients accept responses of any size.
    // A HEAD response carries the Content-Length the equivalent GET would
    // and no octets (RFC 9110 §9.3.2), so the parser has to be told the
    // message ends at the headers. Without this it waits for a body that is
    // required not to arrive, and the request only ends at the timeout.
    parser.skip(wire.method_string() == "HEAD");
    connection.lowest().expires_after(Timeout());
    beast::error_code read_ec;
    auto read_handler = [&read_ec](beast::error_code ec, std::size_t) { read_ec = ec; };
    if (connection.tls != nullptr) {
      bhttp::async_read(*connection.tls, buffer, parser, read_handler);
    } else {
      bhttp::async_read(*connection.plain, buffer, parser, read_handler);
    }
    connection.Run();
    if (read_ec) {
      // A failure before any response bytes means the reused connection went
      // away between requests (clean EOF on Linux, ECONNRESET on macOS) —
      // retryable on a fresh connection. Timeouts and mid-response failures
      // are real; a redial would only repeat them.
      *stale = !parser.got_some() && read_ec != beast::error::timeout;
      return Error::Transport("beast client: read failed: " + read_ec.message());
    }

    const auto& wire_response = parser.get();
    HttpResponse response;
    response.status = static_cast<int>(wire_response.result_int());
    for (const auto& field : wire_response) {
      response.headers.Add(std::string(field.name_string()), std::string(field.value()));
    }
    response.body = wire_response.body();
    *keep_alive = wire_response.keep_alive();
    return response;
  }
};

BeastHttpClient::BeastHttpClient(Options options)
    : state_(std::make_shared<State>(std::move(options))) {}

BeastHttpClient::~BeastHttpClient() = default;

Outcome<std::shared_ptr<BeastHttpClient>> BeastHttpClient::FromConfig(const ClientConfig& config) {
  // The make_shared (State ctor allocates the io_context) is the throw
  // vector; contain it so no exception crosses the Outcome boundary
  // (ADR-0003).
  return smithy::internal::Contain(
      [&]() -> Outcome<std::shared_ptr<BeastHttpClient>> {
        auto endpoint = ParseEndpoint(config.endpoint);
        if (!endpoint) {
          return std::move(endpoint).error();
        }
        Options options;
        options.host = endpoint->host;
        options.port = endpoint->port;
        options.tls = endpoint->tls();
        options.tls_options = config.tls;
        options.request_timeout_ms = config.request_timeout_ms;
        options.max_idle_connections = config.max_idle_connections;
        return std::make_shared<BeastHttpClient>(std::move(options));
      },
      [](const char* what) -> Outcome<std::shared_ptr<BeastHttpClient>> {
        return Error::Transport(std::string("beast client: FromConfig failed: ") +
                                (what != nullptr ? what : "unknown exception"));
      });
}

Outcome<HttpResponse> BeastHttpClient::Send(const HttpRequest& request) {
  // No exception may cross this Outcome boundary (ADR-0003). Network ops are
  // all error_code-based, so the residual throw vector is a std::bad_alloc
  // from the sync drive under memory pressure; Contain turns it into a
  // transport Error instead of unwinding into the caller.
  return smithy::internal::Contain(
      [&]() -> Outcome<HttpResponse> { return SendContained(request); },
      [](const char* what) -> Outcome<HttpResponse> {
        return Error::Transport(std::string("beast client: send failed: ") +
                                (what != nullptr ? what : "unknown exception"));
      });
}

Outcome<HttpResponse> BeastHttpClient::SendContained(const HttpRequest& request) {
  if (state_->opts.host.empty()) {
    return Error::Validation("beast client: options need a host");
  }
  // Request-line injection defense (issue #109): Beast's target()/method
  // do not reject CR/LF, so a raw one would split the request line. Refused
  // before the dial. (An empty target is Beast-defaulted to "/" below.)
  if (request.method.empty() || !ValidRequestLineField(request.method)) {
    return Error::Validation("beast client: request method contains forbidden bytes or is empty");
  }
  if (!ValidRequestLineField(request.target)) {
    return Error::Validation("beast client: request target contains forbidden bytes");
  }
  if (const auto unsafe = FindUnsafeHeader(request.headers); unsafe.has_value()) {
    // The header-injection defense (issue #109): refused before anything
    // is written — an embedded CR/LF would split the request on the wire.
    return Error::Validation("beast client: request header \"" + *unsafe +
                             "\" contains forbidden bytes");
  }
  if (!state_->setup_error.empty()) {
    return Error::Validation(state_->setup_error);
  }
  const auto wire = state_->ToWireRequest(request);

  bool reused = true;
  auto connection = state_->TakeIdle();
  if (connection == nullptr) {
    reused = false;
    auto dialed = state_->Dial();
    if (!dialed) {
      return std::move(dialed).error();
    }
    connection = std::move(dialed).value();
  }

  bool stale = false;
  bool keep_alive = false;
  auto outcome = state_->RoundTrip(*connection, wire, &stale, &keep_alive);
  if (!outcome && reused && stale) {
    // The pooled connection died between requests; retry once on a fresh one.
    auto dialed = state_->Dial();
    if (!dialed) {
      return std::move(dialed).error();
    }
    connection = std::move(dialed).value();
    outcome = state_->RoundTrip(*connection, wire, &stale, &keep_alive);
  }
  if (!outcome) {
    return std::move(outcome).error();
  }
  if (keep_alive) {
    state_->ReturnIdle(std::move(connection));
  }
  return outcome;
}

// ---------------------------------------------------------------------------
// BeastWebSocketClient (ADR-0015)
// ---------------------------------------------------------------------------

namespace {

// One dialed WebSocket connection's io machinery. Unlike ClientConnection's
// drive-to-completion model, a stream is long-lived and full-duplex: after
// the handshakes, the session's pumps run on this io_context from one
// dedicated background thread (the accepted ADR-0015 cost).
struct DialedConnection {
  DialedConnection() : io(1) {}

  asio::io_context io;
  // The dial's TLS context must outlive the stream that speaks it.
  std::optional<asio::ssl::context> ssl;
  // Exactly one is set before the upgrade; the websocket stream adopts it.
  std::unique_ptr<beast::tcp_stream> plain;
  std::unique_ptr<asio::ssl::stream<beast::tcp_stream>> tls;

  beast::tcp_stream& lowest() const {
    return tls != nullptr ? beast::get_lowest_layer(*tls) : *plain;
  }

  void Run() {
    io.run();
    io.restart();
  }
};

// The handle the application owns: delegates to the session and keeps the
// io thread alive for the connection's lifetime. Destruction (an app
// thread — pump callbacks never own this object) aborts the session, stops
// the io, and joins.
class DialedWebSocket final : public WebSocket {
 public:
  DialedWebSocket(std::shared_ptr<WebSocketSessionBase> session,
                  std::unique_ptr<DialedConnection> connection)
      : connection_(std::move(connection)),
        session_(std::move(session)),
        work_guard_(asio::make_work_guard(connection_->io)),
        runner_([this] { RunIoThread(connection_->io, "beast client websocket"); }) {}

  ~DialedWebSocket() override {
    session_->Abort("client released the session");
    work_guard_.reset();
    connection_->io.stop();
    if (runner_.joinable()) {
      runner_.join();
    }
  }

  Outcome<std::optional<eventstream::Message>> Receive() override { return session_->Receive(); }
  Outcome<std::optional<eventstream::Message>> Receive(std::chrono::milliseconds timeout) override {
    return session_->Receive(timeout);
  }
  Outcome<Unit> Send(const eventstream::Message& message) override {
    return session_->Send(message);
  }
  void Close() override { session_->Close(); }
  void ReceiveAsync(WebSocket::ReceiveCallback callback) override {
    session_->ReceiveAsync(std::move(callback));
  }
  void ReceiveAsync(std::chrono::milliseconds timeout,
                    WebSocket::ReceiveCallback callback) override {
    session_->ReceiveAsync(timeout, std::move(callback));
  }
  void SendAsync(const eventstream::Message& message, WebSocket::SendCallback callback) override {
    session_->SendAsync(message, std::move(callback));
  }
  bool SupportsAsync() const override { return session_->SupportsAsync(); }

 private:
  // Declaration order is load-bearing: members destroy in reverse, so the
  // session (whose websocket stream references the io_context) must be
  // released BEFORE connection_ destroys that context.
  std::unique_ptr<DialedConnection> connection_;
  std::shared_ptr<WebSocketSessionBase> session_;
  asio::executor_work_guard<asio::io_context::executor_type> work_guard_;
  std::thread runner_;
};

// The upgrade handshake plus session wiring, generic over the carrier the
// dial produced (plain or TLS). The carrier arrives as the unique_ptr
// itself and its husk is destroyed HERE, while the io_context lives:
// a moved-from Beast stream still owns a fresh impl whose timer touches
// the io_context's timer service at destruction, so a husk left in the
// caller's frame would outlive the io on the refusal path (ASan/UBSan,
// found by CI).
template <typename Stream>
Outcome<std::shared_ptr<WebSocket>> FinishDial(std::unique_ptr<DialedConnection> connection,
                                               std::unique_ptr<Stream> carrier,
                                               const BeastWebSocketClient::Options& options) {
  using Ws = bws::stream<Stream>;
  auto session = std::make_shared<WsSession<Ws>>(Ws(std::move(*carrier)), nullptr);
  carrier.reset();  // the husk dies while its timer service is still alive
  Ws& ws = session->stream();
  // The websocket timeout machinery owns the wire from here; the
  // tcp_stream deadline armed during connect must not keep ticking.
  beast::get_lowest_layer(ws).expires_never();
  ws.set_option(bws::stream_base::timeout{
      .handshake_timeout = std::chrono::milliseconds(std::max(options.handshake_timeout_ms, 1)),
      .idle_timeout = std::chrono::seconds(std::max(options.idle_timeout_seconds, 1)),
      .keep_alive_pings = true});
  ws.read_message_max(eventstream::kMaxMessageBytes);
  if (!options.headers.entries().empty() || options.offer_json_frames) {
    // One decorator carries both jobs — a second set_option would replace
    // the first. The offer is set last, so it wins over a same-named entry
    // smuggled through Options::headers (that header is a negotiation
    // channel, not a transport-bypass one).
    ws.set_option(bws::stream_base::decorator(
        [headers = options.headers, offer = options.offer_json_frames](bws::request_type& req) {
          for (const auto& [name, value] : headers.entries()) {
            req.insert(name, value);
          }
          if (offer) {
            req.set(bhttp::field::sec_websocket_protocol, JsonFramesToken());
          }
        }));
  }

  const int default_port = options.tls ? 443 : 80;
  const std::string host_header = options.port == default_port
                                      ? options.host
                                      : options.host + ":" + std::to_string(options.port);
  beast::error_code upgrade_ec;
  bool handshake_done = false;
  // The response-capturing overload: on a refusal the server's actual HTTP
  // answer is the diagnosis (401 vs 404 vs 503), not Beast's generic
  // "upgrade declined" text — and on success the 101's
  // Sec-WebSocket-Protocol is the negotiation result (ADR-0018).
  bws::response_type handshake_response;
  ws.async_handshake(handshake_response, host_header, options.target.empty() ? "/" : options.target,
                     [&upgrade_ec, &handshake_done](beast::error_code ec) {
                       upgrade_ec = ec;
                       handshake_done = true;
                     });
  // Not Run(): a failed upgrade leaves the websocket timeout timer pending
  // and run() would sit out the whole handshake budget after the refusal
  // already arrived. Pump handlers only until the handshake completes.
  while (!handshake_done && connection->io.run_one() > 0) {
  }
  connection->io.restart();
  if (upgrade_ec) {
    // Tear down in order before returning: the session (and its websocket
    // timeout timer) first, then run the io dry so the timer's canceled
    // wait executes now — nothing may still reference the io_context's
    // services when the connection is destroyed.
    session.reset();
    connection->Run();
    // A refused upgrade is the server's decision (auth, routing); a retry
    // would only repeat it. Beast reports a non-101 answer as
    // upgrade_declined with the response captured above — fold the status
    // and reason into the error, the operator's first question.
    std::string detail;
    if (upgrade_ec == bws::error::upgrade_declined) {
      detail = " refused: HTTP " + std::to_string(handshake_response.result_int());
      const std::string reason(handshake_response.reason());
      if (!reason.empty()) {
        detail += " " + reason;
      }
    } else {
      detail = " failed: " + upgrade_ec.message();
    }
    return Error::Transport("beast websocket: upgrade of " + options.host + options.target + detail,
                            /*retryable=*/false);
  }
  const beast::string_view selected = handshake_response[bhttp::field::sec_websocket_protocol];
  if (!selected.empty()) {
    if (!options.offer_json_frames || selected != JsonFramesToken()) {
      // A server may only select what was offered (RFC 6455); anything
      // else is a peer this client cannot trust to speak either wire.
      session.reset();
      connection->Run();
      return Error::Transport(
          "beast websocket: server selected an unoffered subprotocol: " + std::string(selected),
          /*retryable=*/false);
    }
    // The echo selects the JSON-text wire; no echo is the silent binary
    // fallback (ADR-0018) — both modes carry the same messages.
    session->EnableJsonFrames();
  }
  if (options.raw_text_frames) {
    // The raw-text wire (ADR-0023) is unnegotiated: the caller knows the
    // endpoint speaks JSON-RPC envelopes. (An echoed subprotocol cannot
    // reach here — Dial refuses raw_text_frames + offer_json_frames, and
    // an unoffered echo already failed above.)
    session->EnableRawTextFrames();
  }
  asio::post(connection->io, [session] { session->Start(); });
  return std::shared_ptr<WebSocket>(
      std::make_shared<DialedWebSocket>(std::move(session), std::move(connection)));
}

}  // namespace

Outcome<std::shared_ptr<WebSocket>> BeastWebSocketClient::Dial(Options options) {
  if (options.host.empty()) {
    return Error::Validation("beast websocket: options need a host");
  }
  if (const auto unsafe = FindUnsafeHeader(options.headers); unsafe.has_value()) {
    // The header-injection defense (issue #109): these ride the upgrade
    // GET verbatim, so CR/LF here is request splitting.
    return Error::Validation("beast websocket: upgrade header \"" + *unsafe +
                             "\" contains forbidden bytes");
  }
  if (!ValidRequestLineField(options.target)) {
    // Request-line injection (issue #109): the target rides the upgrade
    // GET's request line, and Beast's handshake target() does not reject
    // CR/LF any more than the HTTP client's does. Guarded here for parity
    // with the upgrade-header check above and the HTTP-client target guard.
    // (The method is always GET, set by Beast, so it needs no check.)
    return Error::Validation("beast websocket: upgrade target contains forbidden bytes");
  }
  if (options.raw_text_frames && options.offer_json_frames) {
    return Error::Validation(
        "beast websocket: raw_text_frames and offer_json_frames cannot both be set (raw text is "
        "unnegotiated; the JSON-frames offer would select a different wire)");
  }
  if (options.port == 0) {
    options.port = options.tls ? 443 : 80;  // the scheme default
  }
  auto connection = std::make_unique<DialedConnection>();
  const auto handshake_budget =
      std::chrono::milliseconds(std::max(options.handshake_timeout_ms, 1));

  asio::ip::tcp::resolver resolver(connection->io);
  boost::system::error_code resolve_ec;
  const auto results = resolver.resolve(options.host, std::to_string(options.port), resolve_ec);
  if (resolve_ec) {
    return Error::Transport("beast websocket: cannot resolve " + options.host + ": " +
                            resolve_ec.message());
  }

  if (options.tls) {
    asio::ssl::context& tls_context = connection->ssl.emplace(asio::ssl::context::tls_client);
    const std::string tls_error = SetupClientTlsContext(tls_context, options.tls_options);
    if (!tls_error.empty()) {
      return Error::Validation("beast websocket: " + tls_error);
    }
    connection->tls =
        std::make_unique<asio::ssl::stream<beast::tcp_stream>>(connection->io, tls_context);
    // SNI: virtual-hosted servers need the name before the handshake.
    if (SSL_set_tlsext_host_name(connection->tls->native_handle(), options.host.c_str()) != 1) {
      return Error::Transport("beast websocket: cannot set SNI host name");
    }
    if (options.tls_options.verify_peer) {
      boost::system::error_code verify_ec;
      (void)connection->tls->set_verify_callback(asio::ssl::host_name_verification(options.host),
                                                 verify_ec);
      if (verify_ec) {
        return Error::Transport("beast websocket: cannot enable hostname verification: " +
                                verify_ec.message());
      }
    }
  } else {
    connection->plain = std::make_unique<beast::tcp_stream>(connection->io);
  }

  connection->lowest().expires_after(handshake_budget);
  beast::error_code connect_ec;
  connection->lowest().async_connect(
      results,
      [&connect_ec](beast::error_code ec, const asio::ip::tcp::endpoint&) { connect_ec = ec; });
  connection->Run();
  if (connect_ec) {
    return Error::Transport("beast websocket: cannot connect to " + options.host + ":" +
                            std::to_string(options.port) + ": " + connect_ec.message());
  }

  if (options.tls) {
    connection->lowest().expires_after(handshake_budget);
    beast::error_code handshake_ec;
    connection->tls->async_handshake(asio::ssl::stream_base::client,
                                     [&handshake_ec](beast::error_code ec) { handshake_ec = ec; });
    connection->Run();
    if (handshake_ec) {
      // Verification failures are configuration/identity problems, not
      // transient transport blips: not retryable.
      return Error::Transport("beast websocket: TLS handshake with " + options.host +
                                  " failed: " + handshake_ec.message(),
                              /*retryable=*/false);
    }
    auto carrier = std::move(connection->tls);
    return FinishDial(std::move(connection), std::move(carrier), options);
  }
  auto carrier = std::move(connection->plain);
  return FinishDial(std::move(connection), std::move(carrier), options);
}

WebSocketDialer BeastWebSocketClient::Dialer() {
  return [](const WebSocketDialRequest& request) {
    Options options;
    options.host = request.host;
    options.port = request.port;
    options.tls = request.tls;
    options.tls_options = request.tls_options;
    options.target = request.target;
    options.headers = request.headers;
    options.raw_text_frames = request.raw_text_frames;
    options.handshake_timeout_ms = request.handshake_timeout_ms;
    options.idle_timeout_seconds = request.idle_timeout_seconds;
    return Dial(std::move(options));
  };
}

}  // namespace smithy::http
