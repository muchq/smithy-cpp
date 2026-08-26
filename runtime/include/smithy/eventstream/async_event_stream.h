#ifndef SMITHY_EVENTSTREAM_ASYNC_EVENT_STREAM_H_
#define SMITHY_EVENTSTREAM_ASYNC_EVENT_STREAM_H_

#include <atomic>
#include <cassert>
#include <chrono>
#include <coroutine>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>

#include "smithy/core/outcome.h"
#include "smithy/eventstream/event_stream.h"
#include "smithy/eventstream/frame.h"
#include "smithy/http/websocket.h"

namespace smithy::eventstream {

// The session-loop coroutine type (ADR-0019): fire and forget. A Detached
// coroutine starts eagerly, owns nothing after launch (its frame frees
// itself at the end), and contains unhandled exceptions to a log line —
// the transport's containment posture, since these loops run on wire
// threads. The coroutine surface here is Detached, StreamTask (ADR-0021),
// and AsyncEventStream's awaitables.
struct Detached {
  struct promise_type {
    // NOLINTBEGIN(readability-convert-member-functions-to-static) — the
    // coroutine machinery calls these through the promise instance, and
    // making them static just trips static-accessed-through-instance at
    // every coroutine that uses the type.
    Detached get_return_object() noexcept { return {}; }
    std::suspend_never initial_suspend() noexcept { return {}; }
    std::suspend_never final_suspend() noexcept { return {}; }
    void return_void() noexcept {}
    void unhandled_exception() noexcept {
      // A coroutine promise must declare unhandled_exception(), but under
      // -fno-exceptions it is unreachable — nothing can throw — so the
      // containment body compiles only when exceptions are enabled.
#if defined(__cpp_exceptions)
      try {
        std::rethrow_exception(std::current_exception());
      } catch (const std::exception& e) {
        std::clog << "smithy: detached stream coroutine threw: " << e.what() << "\n";
      } catch (...) {
        std::clog << "smithy: detached stream coroutine threw a non-std exception\n";
      }
#endif
    }
    // NOLINTEND(readability-convert-member-functions-to-static)
  };
};

// The handler-task coroutine type (ADR-0021): the shape a generated async
// streaming handler returns. Lazy — nothing runs until the generated
// Detached wrapper co_awaits it — and single-shot: the await starts the
// handler, the handler's `co_return Outcome<Unit>` resumes the awaiter by
// symmetric transfer, and a handler that throws completes with
// Error::Unknown instead of terminating (the containment posture, since
// completion contexts have no caller to rethrow to). Every path must
// co_return an Outcome — `smithy::Unit{}` is the clean close (a bare
// co_return does not compile). Handlers may factor their logic into
// StreamTask-returning sub-coroutines and co_await each exactly once;
// sub-results beyond the Outcome travel by out-parameter (the result
// type is deliberately not generic). Not a task framework: no executor,
// no generic result — exactly "await one handler, get its outcome",
// which is all the generated serve path needs to restore the blocking
// contract's framework-framed typed errors.
class [[nodiscard]] StreamTask {
 public:
  struct promise_type {
    // NOLINTBEGIN(readability-convert-member-functions-to-static) — the
    // coroutine machinery calls these through the promise instance.
    StreamTask get_return_object() noexcept {
      return StreamTask(std::coroutine_handle<promise_type>::from_promise(*this));
    }
    std::suspend_always initial_suspend() noexcept { return {}; }
    struct FinalAwaiter {
      bool await_ready() const noexcept { return false; }
      std::coroutine_handle<> await_suspend(std::coroutine_handle<promise_type> h) noexcept {
        return h.promise().continuation;  // symmetric transfer to the awaiter
      }
      void await_resume() const noexcept {}
    };
    FinalAwaiter final_suspend() noexcept { return {}; }
    void return_value(Outcome<Unit> outcome) noexcept { result = std::move(outcome); }
    void unhandled_exception() noexcept {
      // Required by the coroutine machinery, but unreachable under
      // -fno-exceptions; the containment body compiles only with exceptions.
#if defined(__cpp_exceptions)
      try {
        std::rethrow_exception(std::current_exception());
      } catch (const std::exception& e) {
        result = Error::Unknown(std::string("streaming handler threw: ") + e.what());
      } catch (...) {
        result = Error::Unknown("streaming handler threw a non-std exception");
      }
#endif
    }
    // NOLINTEND(readability-convert-member-functions-to-static)
    Outcome<Unit> result = Unit{};
    std::coroutine_handle<> continuation = std::noop_coroutine();
  };

  explicit StreamTask(std::coroutine_handle<promise_type> handle) : handle_(handle) {}
  StreamTask(StreamTask&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}
  StreamTask(const StreamTask&) = delete;
  StreamTask& operator=(const StreamTask&) = delete;
  StreamTask& operator=(StreamTask&&) = delete;  // single-shot: no reseating
  ~StreamTask() {
    // Destroys a suspended frame: never-awaited (parked at the initial
    // suspend) or completed (parked at the final suspend) — both legal.
    if (handle_) handle_.destroy();
  }

  // co_await, exactly once: starts the handler by symmetric transfer and
  // resumes the awaiter with the handler's Outcome when it completes — on
  // whatever thread the handler last resumed on, which is the awaiting
  // thread itself when the handler never suspended. (The NOLINT: the
  // awaitable protocol calls await_ready through the instance.)
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  bool await_ready() const noexcept { return false; }
  std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
    // Double-await and await-of-moved-from are contract violations the
    // type cannot express; fail loudly in debug builds instead of
    // resuming a done or null frame.
    assert(handle_ && !handle_.done());
    handle_.promise().continuation = awaiter;
    return handle_;
  }
  Outcome<Unit> await_resume() noexcept { return std::move(handle_.promise().result); }

 private:
  std::coroutine_handle<promise_type> handle_;
};

// The generated launch wrapper's exception-frame send (ADR-0021): awaits
// one raw, already-framed message on the socket. Exists so the wrapper's
// frame — and the stream that frame owns — stays alive until the wire has
// taken a refusal: destroying the stream closes the session, and a close
// over a busy wire may cancel the in-flight write (the Beast escalation),
// silently dropping the typed error. Best-effort by convention — callers
// discard the outcome, close, and end.
class [[nodiscard]] SendMessageAwaitable {
 public:
  SendMessageAwaitable(std::shared_ptr<http::WebSocket> socket, Message message)
      : socket_(std::move(socket)), message_(std::move(message)) {}
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  bool await_ready() const noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> coroutine) {
    // The second-arrival-resumes race, as in AsyncEventStream's awaitables.
    socket_->SendAsync(message_, [this, coroutine](Outcome<Unit> sent) {
      sent_ = std::move(sent);
      if (arrived_.exchange(true)) coroutine.resume();
    });
    return !arrived_.exchange(true);  // suspend iff the callback has not run
  }
  Outcome<Unit> await_resume() noexcept { return std::move(sent_); }

 private:
  std::shared_ptr<http::WebSocket> socket_;
  Message message_;
  std::atomic<bool> arrived_{false};
  Outcome<Unit> sent_ = Unit{};
};

inline SendMessageAwaitable SendMessage(std::shared_ptr<http::WebSocket> socket, Message message) {
  return SendMessageAwaitable(std::move(socket), std::move(message));
}

// SendMessage's receive twin (ADR-0023): awaits one raw message from the
// socket before any stream exists — how the generated jsonRpc2 session
// route reads the opening request envelope that selects the operation,
// inside its Detached launch body where blocking is forbidden. Same
// outcomes as WebSocket::Receive: nullopt is the peer's clean close.
// One-outstanding-per-class passes through; single-shot like its twin.
class [[nodiscard]] ReceiveMessageAwaitable {
 public:
  explicit ReceiveMessageAwaitable(std::shared_ptr<http::WebSocket> socket,
                                   std::optional<std::chrono::milliseconds> timeout = std::nullopt)
      : socket_(std::move(socket)), timeout_(timeout) {}
  // NOLINTNEXTLINE(readability-convert-member-functions-to-static)
  bool await_ready() const noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> coroutine) {
    // The second-arrival-resumes race, as in AsyncEventStream's awaitables.
    auto completion = [this, coroutine](Outcome<std::optional<Message>> message) {
      received_ = std::move(message);
      if (arrived_.exchange(true)) coroutine.resume();
    };
    if (timeout_.has_value()) {
      socket_->ReceiveAsync(*timeout_, std::move(completion));
    } else {
      socket_->ReceiveAsync(std::move(completion));
    }
    return !arrived_.exchange(true);  // suspend iff the callback has not run
  }
  Outcome<std::optional<Message>> await_resume() noexcept { return std::move(received_); }

 private:
  std::shared_ptr<http::WebSocket> socket_;
  std::optional<std::chrono::milliseconds> timeout_;
  std::atomic<bool> arrived_{false};
  // Outcome has no default constructor; the placeholder is overwritten
  // before any resume. NOLINT(readability-redundant-member-init)
  Outcome<std::optional<Message>> received_ = std::optional<Message>();  // NOLINT
};

inline ReceiveMessageAwaitable ReceiveMessage(std::shared_ptr<http::WebSocket> socket) {
  return ReceiveMessageAwaitable(std::move(socket));
}

// The same await under a deadline (#130): a coroutine that must not park
// forever on a peer that may never send. Resolves with the socket's timed
// receive outcomes — the message, the clean close, the terminal error, or
// Error::Timeout ("TimeoutError"), which is not terminal: the session and
// its receive slot are exactly as a completed receive leaves them, so the
// loop can await again, send, or close on its own schedule.
inline ReceiveMessageAwaitable ReceiveMessage(std::shared_ptr<http::WebSocket> socket,
                                              std::chrono::milliseconds timeout) {
  return ReceiveMessageAwaitable(std::move(socket), timeout);
}

// The typed session's coroutine adapter (ADR-0019): EventStream's contract
// over the completion-driven socket primitives, with `co_await` where the
// blocking facade parks a thread. Owns its session (shared_ptr — the async
// session's natural shape, per ADR-0016's forecast); typically it lives in
// the frame of the Detached loop serving it, so the session ends when the
// loop does.
//
//   smithy::eventstream::Detached Serve(AsyncEventStream<Tx, Rx> stream) {
//     while (true) {
//       auto event = co_await stream.Receive();
//       if (!event.ok() || !event->has_value()) break;
//       ... co_await stream.Send(reply) ...
//     }
//   }
//
// Resumption runs on the transport's completion context (a Beast io
// thread; the peer's thread on the in-memory pair): never block there —
// blocking work belongs on application threads, reached through Share().
// The socket's one-outstanding contracts pass through: one co_await
// Receive and one co_await Send at a time, which a single sequential
// coroutine satisfies by construction. Close() from any thread completes
// an outstanding await with the stream's terminal outcome — cancellation
// stays "close the session".
//
// Two lifetime rules for the Detached pattern: (1) everything the
// coroutine references (registry, hub state) must outlive the transport —
// the loops resume on io threads long after the launch callback returned.
// (2) This destructor revokes Share() handles and WAITS for any pinned
// handle operation to drain (ADR-0017); at the end of a Detached loop
// that wait runs on the completion context, and the completion it waits
// for needs an io thread — so leave transports that mix Detached sessions
// with handle traffic more than one io thread (the Beast default is 4).
template <typename Tx, typename Rx>
class AsyncEventStream {
 public:
  using Encoder = std::function<Outcome<Message>(const Tx&)>;
  using Decoder = std::function<Outcome<Rx>(const Message&)>;

  AsyncEventStream(std::shared_ptr<http::WebSocket> socket, Encoder encode, Decoder decode)
      : socket_(std::move(socket)), encode_(std::move(encode)), decode_(std::move(decode)) {}

  // Move-only, like EventStream: the view owner revokes on destruction, so
  // handles minted by Share() fail softly once the stream is gone.
  ~AsyncEventStream() = default;
  AsyncEventStream(AsyncEventStream&&) noexcept = default;
  // No explicit noexcept: the computed one depends on std::function's
  // move-assignment, which the standard does not make noexcept.
  AsyncEventStream& operator=(AsyncEventStream&&) = default;
  AsyncEventStream(const AsyncEventStream&) = delete;
  AsyncEventStream& operator=(const AsyncEventStream&) = delete;

  // Awaits the next event: nullopt is the peer's clean close. A message
  // the decoder rejects — a received exception (decoded to its modeled
  // Error) or an undecodable message — is terminal (ADR-0016): the session
  // is closed and the error is the awaited result.
  class [[nodiscard]] ReceiveAwaitable {
   public:
    explicit ReceiveAwaitable(AsyncEventStream* stream,
                              std::optional<std::chrono::milliseconds> timeout = std::nullopt)
        : stream_(stream), timeout_(timeout) {}
    bool await_ready() const noexcept { return false; }
    bool await_suspend(std::coroutine_handle<> coroutine) {
      // The completion may fire before this returns (immediate refusals,
      // the in-memory pair, a ready queue whose post beats us): whichever
      // side arrives second owns the resume. A synchronous completion
      // resumes by returning false — flattening would-be recursion into
      // iteration and never touching the frame from two threads at once —
      // and only a truly asynchronous completion resumes from the callback.
      // The flag can live in the awaitable: the frame dies only after a
      // resume, and every resume is sequenced after both exchanges.
      auto completion = [this, coroutine](Outcome<std::optional<Message>> message) {
        raw_ = std::move(message);
        if (arrived_.exchange(true)) coroutine.resume();
      };
      if (timeout_.has_value()) {
        stream_->socket_->ReceiveAsync(*timeout_, std::move(completion));
      } else {
        stream_->socket_->ReceiveAsync(std::move(completion));
      }
      return !arrived_.exchange(true);  // suspend iff the callback has not run
    }
    Outcome<std::optional<Rx>> await_resume() {
      // A timeout (code "TimeoutError") arrives here as raw_'s error and
      // passes through untouched: it is not terminal, nothing is closed,
      // and the next co_await picks the stream up where it left off. Only
      // a decoder rejection below ends the session.
      if (!raw_.ok()) return std::move(raw_).error();
      std::optional<Message>& message = *raw_;
      if (!message.has_value()) return std::optional<Rx>();
      auto event = stream_->decode_(*message);
      if (!event.ok()) {
        stream_->Close();
        return std::move(event).error();
      }
      return std::optional<Rx>(std::move(*event));
    }

   private:
    AsyncEventStream* stream_;
    std::optional<std::chrono::milliseconds> timeout_;
    std::atomic<bool> arrived_{false};
    // Outcome has no default constructor; the placeholder is overwritten
    // before any resume. NOLINT(readability-redundant-member-init)
    Outcome<std::optional<Message>> raw_ = std::optional<Message>();  // NOLINT
  };

  ReceiveAwaitable Receive() { return ReceiveAwaitable(this); }

  // Receive under a deadline (#130) — the async twin of EventStream's
  // timed Receive, for a loop that must not park forever on an event the
  // peer may never send:
  //
  //   auto event = co_await stream.Receive(std::chrono::seconds(2));
  //   if (!event.ok() && event.error().code() == "TimeoutError") { ... }
  //
  // Same outcomes plus Error::Timeout, and the same rule as the blocking
  // overload: a timeout is not terminal — the session stays usable, the
  // receive slot is released, and an event the wire delivers after the
  // deadline waits in the session for the next await. A non-positive
  // timeout polls. The deadline bounds THIS await only; the session's own
  // idle timeout still governs a quiet wire.
  ReceiveAwaitable Receive(std::chrono::milliseconds timeout) {
    return ReceiveAwaitable(this, timeout);
  }

  // Awaits one event onto the wire. Encoder failures surface without
  // suspending and leave the session untouched; wire failures are the
  // socket's (Error::Transport once the session ended). Uncallable when Tx
  // is NoEvents, like every send in this direction.
  class [[nodiscard]] SendAwaitable {
   public:
    SendAwaitable(AsyncEventStream* stream, Outcome<Message> message)
        : stream_(stream), message_(std::move(message)) {}
    bool await_ready() const noexcept { return !message_.ok(); }
    bool await_suspend(std::coroutine_handle<> coroutine) {
      // Same second-arrival-resumes race as ReceiveAwaitable.
      stream_->socket_->SendAsync(*message_, [this, coroutine](Outcome<Unit> sent) {
        sent_ = std::move(sent);
        if (arrived_.exchange(true)) coroutine.resume();
      });
      return !arrived_.exchange(true);  // suspend iff the callback has not run
    }
    Outcome<Unit> await_resume() {
      if (!message_.ok()) return std::move(message_).error();
      return std::move(sent_);
    }

   private:
    AsyncEventStream* stream_;
    std::atomic<bool> arrived_{false};
    Outcome<Message> message_;
    Outcome<Unit> sent_ = Unit{};
  };

  SendAwaitable Send(const Tx& event) {
    static_assert(!std::is_same_v<Tx, NoEvents>,
                  "this stream models no events in this direction: Send is not callable on a "
                  "receive-only AsyncEventStream (use Receive/Close)");
    return SendAwaitable(this, encode_(event));
  }

  // Initiates the close handshake; idempotent and safe from any thread.
  // An outstanding await completes with the terminal outcome.
  void Close() { socket_->Close(); }

  // Whether the wrapped session implements the async primitives — false
  // means every co_await completes immediately with the base default's
  // polite refusal (websocket.h); check before adopting a socket of
  // unknown provenance.
  bool SupportsAsync() const { return socket_->SupportsAsync(); }

  // The same owning handle EventStream mints (ADR-0017), over the same
  // revocable view: blocking or async sends from any thread, safe to hold
  // beyond this stream's lifetime, and the SessionRegistry composes
  // unchanged. The handle never extends the session — destroying this
  // stream after a Share() closes it, waits out pinned operations, and
  // leaves every handle failing softly.
  EventStreamHandle<Tx> Share() {
    return EventStreamHandle<Tx>(view_.Ensure(socket_.get()), encode_);
  }

 private:
  std::shared_ptr<http::WebSocket> socket_;
  Encoder encode_;
  Decoder decode_;
  // Last member on purpose: its teardown (End) runs first and needs the
  // socket above still alive.
  internal::SharedViewOwner view_;
};

}  // namespace smithy::eventstream

#endif  // SMITHY_EVENTSTREAM_ASYNC_EVENT_STREAM_H_
