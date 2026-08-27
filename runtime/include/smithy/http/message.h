#ifndef SMITHY_HTTP_MESSAGE_H_
#define SMITHY_HTTP_MESSAGE_H_

#include <string>

#include "smithy/http/headers.h"

namespace smithy::http {

// Request/response bodies are byte strings for now. The alias exists so the
// representation can grow into a stream-shaped type for Phase 8 (event
// streams) without touching every signature.
using Body = std::string;

// Every member carries a default member initializer so partial aggregate
// initialization (`HttpResponse{404, {}, "not found"}`) is warning-free under
// -Wextra's -Wmissing-field-initializers — here and in consumer code.
struct HttpRequest {
  std::string method = "GET";
  // Origin-form target: percent-encoded path plus optional query,
  // e.g. "/cities/a%20b?pageSize=10".
  std::string target = "/";
  Headers headers{};
  Body body{};
  // Server-side annotation, never read from or written to the wire: the
  // connection's remote endpoint as "ip:port" ("203.0.113.7:52814",
  // "[2001:db8::1]:443"), stamped by the server transport for logging and
  // source-based policy (issue #46). Empty when the transport has no peer
  // (Loopback) or the socket could not report one. Client-side Send()
  // ignores it.
  std::string peer_address{};
};

struct HttpResponse {
  int status = 200;
  Headers headers{};
  Body body{};
  // Server-side annotation, never written to the wire: the Smithy operation
  // whose route produced this response (stamped by the generated router so
  // observability middleware can label by operation; empty on 404/405/400
  // dispatch failures and hand-rolled handlers). Built-in endpoints that
  // answer off-model paths (HealthEndpoint, MetricsEndpoint) stamp that path
  // instead, so their traffic is distinguishable from a dispatch failure.
  std::string operation{};
};

}  // namespace smithy::http

#endif  // SMITHY_HTTP_MESSAGE_H_
