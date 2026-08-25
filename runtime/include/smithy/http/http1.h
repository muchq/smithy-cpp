#ifndef SMITHY_HTTP_HTTP1_H_
#define SMITHY_HTTP_HTTP1_H_

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

#include "smithy/core/outcome.h"
#include "smithy/http/headers.h"

namespace smithy::http {

// The HTTP/1.1 message reader behind SocketHttpClient/SocketHttpServer,
// factored out of the socket layer so hostile-input tests and the fuzz
// harness can drive it byte-for-byte without a live file descriptor.
//
// The framing contract is deliberately strict — both transports emit
// Connection: close, so a message is a definite Content-Length body or (for
// responses) body-until-EOF. Chunked transfer-encoding and conflicting
// content-lengths are rejected outright: they are the classic
// request-smuggling desync vectors.

struct Http1Message {
  std::string start_line;
  Headers headers;
  std::string body;
};

// Byte source with recv() semantics: fills up to `capacity` bytes of
// `buffer`, returns the count read, 0 on EOF, negative on error.
using Http1ReadFn = std::function<long(char* buffer, std::size_t capacity)>;

// Reads one HTTP/1.1 message from the byte source. When body_until_eof is
// true (client reading a response), a message without Content-Length extends
// to EOF; when false (server reading a request), it ends with the headers.
//
// has_body false reads the headers and stops, whatever they frame. That is
// the HEAD response: RFC 9110 §9.3.2 has it carry the Content-Length the
// equivalent GET would, with no body behind it, so a reader that honoured
// the header would block for bytes that are never coming (issue #192).
Outcome<Http1Message> ReadHttp1Message(const Http1ReadFn& read, bool body_until_eof,
                                       bool has_body = true);

// Splits a request line "GET /target HTTP/1.1" into method and target;
// false when the line does not have its two spaces.
bool ParseRequestLine(std::string_view line, std::string* method, std::string* target);

// Extracts the status code from a status line "HTTP/1.1 200 OK"; an error
// when the line is not HTTP-shaped or the status is implausible.
Outcome<int> ParseStatusLine(std::string_view line);

}  // namespace smithy::http

#endif  // SMITHY_HTTP_HTTP1_H_
