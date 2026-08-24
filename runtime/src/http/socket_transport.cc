#include "smithy/http/socket_transport.h"

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <iostream>
#include <string_view>
#include <utility>

#include "smithy/core/exception_guard.h"
#include "smithy/http/headers.h"
#include "smithy/http/http1.h"
#include "smithy/http/server_dispatch.h"

namespace smithy::http {
namespace {

using SocketFd = int;
constexpr SocketFd kInvalidSocket = -1;

void SetTimeouts(SocketFd fd, int timeout_ms) {
  timeval value{};
  value.tv_sec = timeout_ms / 1000;
  value.tv_usec = static_cast<decltype(value.tv_usec)>((timeout_ms % 1000) * 1000L);
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#ifdef SO_NOSIGPIPE
  // macOS/BSD have no MSG_NOSIGNAL; a peer that closes mid-send must surface
  // as an EPIPE write error, never a process-killing SIGPIPE.
  const int no_sigpipe = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &no_sigpipe, sizeof(no_sigpipe));
#endif
}

bool SendAll(SocketFd fd, std::string_view data) {
#ifdef MSG_NOSIGNAL
  // Linux: writes to a peer-closed socket fail with EPIPE instead of raising
  // SIGPIPE (whose default action would kill the process).
  constexpr int kSendFlags = MSG_NOSIGNAL;
#else
  constexpr int kSendFlags = 0;  // macOS/BSD use SO_NOSIGPIPE (set above).
#endif
  while (!data.empty()) {
    const std::size_t chunk = data.size() > 65536 ? 65536 : data.size();
    const auto sent = send(fd, data.data(), chunk, kSendFlags);
    if (sent <= 0) return false;
    data.remove_prefix(static_cast<std::size_t>(sent));
  }
  return true;
}

// Reads one HTTP/1.1 message (the parser itself lives in http1.cc, where the
// hostile-input bank and the fuzz harness exercise it without a socket). For
// responses without Content-Length the body extends to EOF (we always
// request/emit Connection: close).
Outcome<Http1Message> ReadMessage(SocketFd fd, bool body_until_eof, bool has_body = true) {
  return ReadHttp1Message(
      [fd](char* buffer, std::size_t capacity) {
        return static_cast<long>(recv(fd, buffer, capacity, 0));
      },
      body_until_eof, has_body);
}

}  // namespace

Outcome<HttpResponse> SocketHttpClient::Send(const HttpRequest& request) {
  // Request-line injection defense (issue #109): method and target are
  // spliced into "METHOD SP TARGET SP HTTP/1.1" below, so a space or CR/LF
  // in either splits or corrupts the request line. Refused before connect.
  if (request.method.empty() || !ValidRequestLineField(request.method)) {
    return Error::Validation("http: request method contains forbidden bytes or is empty");
  }
  if (!ValidRequestLineField(request.target)) {
    return Error::Validation("http: request target contains forbidden bytes");
  }
  if (const auto unsafe = FindUnsafeHeader(request.headers); unsafe.has_value()) {
    // The header-injection defense (issue #109): the writer below splices
    // names and values between CRLFs verbatim, so an embedded CR/LF would
    // split the request. Refused before anything connects.
    return Error::Validation("http: request header \"" + *unsafe + "\" contains forbidden bytes");
  }
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  const std::string port_text = std::to_string(port_);
  if (getaddrinfo(host_.c_str(), port_text.c_str(), &hints, &results) != 0 || results == nullptr) {
    return Error::Transport("http: cannot resolve host " + host_);
  }

  SocketFd fd = kInvalidSocket;
  for (const addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (fd == kInvalidSocket) continue;
    SetTimeouts(fd, timeout_ms_);
    if (connect(fd, entry->ai_addr, entry->ai_addrlen) == 0) break;
    close(fd);
    fd = kInvalidSocket;
  }
  freeaddrinfo(results);
  if (fd == kInvalidSocket) {
    return Error::Transport("http: cannot connect to " + host_ + ":" + port_text);
  }

  std::string wire = request.method + " " + request.target + " HTTP/1.1\r\n";
  wire += "host: " + host_ + ":" + port_text + "\r\n";
  if (!request.headers.Has("content-length")) {
    wire += "content-length: " + std::to_string(request.body.size()) + "\r\n";
  }
  wire += "connection: close\r\n";
  for (const auto& [name, value] : request.headers.entries()) {
    wire += name;
    wire += ": ";
    wire += value;
    wire += "\r\n";
  }
  wire += "\r\n";
  wire += request.body;

  if (!SendAll(fd, wire)) {
    close(fd);
    return Error::Transport("http: write failed");
  }
  // A HEAD response is headers only, however its Content-Length reads
  // (issue #192) — honouring that header would block for a body the server
  // is required not to send.
  auto message = ReadMessage(fd, /*body_until_eof=*/true,
                             /*has_body=*/request.method != "HEAD");
  close(fd);
  if (!message) return std::move(message).error();

  // Status line: "HTTP/1.1 200 OK".
  auto status = ParseStatusLine(message->start_line);
  if (!status) return std::move(status).error();
  HttpResponse response;
  response.status = *status;
  response.headers = std::move(message->headers);
  response.body = std::move(message->body);
  return response;
}

SocketHttpServer::~SocketHttpServer() { Shutdown(); }

Outcome<Unit> SocketHttpServer::Start(RequestHandler handler) {
  handler_ = std::move(handler);
  stopping_ = false;

  const SocketFd fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd == kInvalidSocket) return Error::Transport("http: cannot create listener");
  const int enable = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<std::uint16_t>(requested_port_));
  if (bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
      listen(fd, 16) != 0) {
    close(fd);
    return Error::Transport("http: cannot bind 127.0.0.1:" + std::to_string(requested_port_));
  }
  sockaddr_in bound{};
  socklen_t bound_size = sizeof(bound);
  getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_size);
  bound_port_ = ntohs(bound.sin_port);
  listener_ = static_cast<std::intptr_t>(fd);
  // Relegation notice (ADR-0006, issue #46): docs already point production at
  // BeastServerTransport, but a deployment that reached for this class anyway
  // deserves the operator-visible trace, not a silent one-request-at-a-time
  // service (the server_dispatch clog convention).
  std::clog << "smithy: SocketHttpServer is a test-only transport (one connection at a time, "
               "loopback only); production serving is BeastServerTransport\n";
  accept_thread_ = std::thread([this] { AcceptLoop(); });
  return Unit{};
}

void SocketHttpServer::AcceptLoop() {
  const auto listener = static_cast<SocketFd>(listener_);
  while (!stopping_) {
    sockaddr_storage peer{};
    socklen_t peer_length = sizeof(peer);
    const SocketFd connection = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_length);
    if (connection == kInvalidSocket) {
      if (stopping_) break;
      continue;
    }
    if (stopping_) {
      close(connection);
      break;
    }
    SetTimeouts(connection, 30000);
    auto message = ReadMessage(connection, /*body_until_eof=*/false);
    HttpResponse response;
    // Framing needs the method (issue #192), and the request is moved into
    // the handler below. False when the request line never parsed: there is
    // no method then, and the 400 carries its own body.
    bool head = false;
    if (message) {
      // Request line: "GET /target HTTP/1.1".
      HttpRequest request;
      if (!ParseRequestLine(message->start_line, &request.method, &request.target)) {
        response = HttpResponse{400, {}, "malformed request line"};
      } else {
        // Exact: the method token is case-sensitive (RFC 9110 §9.1), and the
        // router matches it by exact string.
        head = request.method == "HEAD";
        request.headers = std::move(message->headers);
        request.body = std::move(message->body);
        request.peer_address =
            FormatPeerAddress(reinterpret_cast<const sockaddr*>(&peer), peer_length);
        response = InvokeHandlerGuarded(handler_, std::move(request));
      }
    } else {
      response = HttpResponse{400, {}, message.error().message()};
    }

    // The transport is authoritative for framing, so drop any copies a handler
    // set — otherwise they are emitted twice (a duplicate content-length is
    // exactly the smuggling vector a strict peer now rejects), and a
    // handler-set transfer-encoding beside our content-length would be the
    // classic smuggling pair.
    response.headers.Remove("content-length");
    response.headers.Remove("transfer-encoding");
    response.headers.Remove("connection");
    if (FindUnsafeHeader(response.headers).has_value()) {
      // The header-injection defense (issue #109), at the same authority
      // point that owns the framing set above: replace the whole response
      // with fixed text rather than write a splittable field line.
      response = HttpResponse{};
      response.status = 500;
      response.headers.Set("content-type", "text/plain");
      response.body = "internal error: response header contains forbidden bytes";
    }
    // Content-Length is the length the equivalent GET would report, whether
    // or not the body follows it — that is what makes a HEAD response
    // answerable (RFC 9110 §9.3.2), and it is computed before the body is
    // withheld below.
    std::string wire = "HTTP/1.1 " + std::to_string(response.status) + " \r\n";
    wire += "content-length: " + std::to_string(response.body.size()) + "\r\n";
    wire += "connection: close\r\n";
    for (const auto& [name, value] : response.headers.entries()) {
      wire += name;
      wire += ": ";
      wire += value;
      wire += "\r\n";
    }
    wire += "\r\n";
    // A HEAD response is the headers and nothing else. This transport closes
    // every connection, so a stray body could not desynchronize a subsequent
    // request the way it does on the keep-alive Beast path — but a client
    // reading Content-Length bytes after a HEAD is reading the next thing on
    // the socket either way.
    if (!head) {
      wire += response.body;
    }
    SendAll(connection, wire);
    close(connection);
  }
}

void SocketHttpServer::Stop() { Shutdown(); }

void SocketHttpServer::Shutdown() noexcept {
  if (!accept_thread_.joinable()) return;
  stopping_ = true;
  // Teardown must not propagate out of a destructor; Contain swallows any
  // throw (and is a no-op wrapper under -fno-exceptions).
  smithy::internal::Contain(
      [&] {
        // Nudge the accept loop awake with a throwaway connection, then close.
        {
          SocketHttpClient nudge("127.0.0.1", bound_port_, /*timeout_ms=*/1000);
          HttpRequest wake;
          wake.method = "GET";
          wake.target = "/";
          (void)nudge.Send(wake);
        }
        accept_thread_.join();
      },
      [](const char*) {});
  close(static_cast<SocketFd>(listener_));
  listener_ = -1;
  handler_ = nullptr;
}

}  // namespace smithy::http
