#include "smithy/http/http1.h"

#include <array>
#include <cstdlib>
#include <string>
#include <string_view>

#include "smithy/core/error.h"

namespace smithy::http {
namespace {

constexpr std::size_t kMaxHeaderBytes = std::size_t{64} * 1024;
constexpr std::size_t kMaxBodyBytes = std::size_t{64} * 1024 * 1024;

}  // namespace

Outcome<Http1Message> ReadHttp1Message(const Http1ReadFn& read, bool body_until_eof,
                                       bool has_body) {
  std::string buffer;
  std::size_t header_end = std::string::npos;
  std::array<char, 8192> chunk{};
  while (header_end == std::string::npos) {
    if (buffer.size() > kMaxHeaderBytes) return Error::Transport("http: headers too large");
    const long received = read(chunk.data(), chunk.size());
    if (received < 0) return Error::Transport("http: read failed");
    if (received == 0) return Error::Transport("http: connection closed mid-headers");
    buffer.append(chunk.data(), static_cast<std::size_t>(received));
    header_end = buffer.find("\r\n\r\n");
  }

  Http1Message message;
  std::string_view head(buffer.data(), header_end);
  const auto line_end = head.find("\r\n");
  message.start_line = std::string(head.substr(0, line_end));
  std::string_view header_block =
      line_end == std::string_view::npos ? std::string_view{} : head.substr(line_end + 2);
  while (!header_block.empty()) {
    const auto eol = header_block.find("\r\n");
    const std::string_view line =
        eol == std::string_view::npos ? header_block : header_block.substr(0, eol);
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) return Error::Transport("http: malformed header line");
    std::string_view name = line.substr(0, colon);
    std::string_view value = line.substr(colon + 1);
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
      value.remove_prefix(1);
    }
    while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
      value.remove_suffix(1);
    }
    message.headers.Add(name, value);
    if (eol == std::string_view::npos) break;
    header_block.remove_prefix(eol + 2);
  }

  // Reject ambiguous or unsupported framing before trusting a body length —
  // the classic request-smuggling desync vectors. Neither transport direction
  // implements chunked transfer, and conflicting content-lengths let a proxy
  // and this server disagree on message boundaries.
  if (message.headers.GetAll("content-length").size() > 1) {
    return Error::Transport("http: conflicting content-length");
  }
  if (message.headers.Has("transfer-encoding")) {
    return Error::Transport("http: transfer-encoding is not supported");
  }

  message.body = buffer.substr(header_end + 4);
  if (!has_body) {
    // The headers are the whole message. Anything already in the buffer past
    // them belongs to whatever comes next on the connection, not to this
    // response, so it is not claimed as a body.
    message.body.clear();
    return message;
  }
  if (const auto length_text = message.headers.Get("content-length")) {
    // Strict RFC 9110 field value: digits only. strtoull alone would also
    // accept "+4", leading whitespace, or an empty value (as 0) — laxity a
    // desync attack can hide in. Overflow clamps to ULLONG_MAX, which the
    // size cap below rejects.
    if (length_text->empty() || length_text->find_first_not_of("0123456789") != std::string::npos) {
      return Error::Transport("http: invalid content-length");
    }
    char* end = nullptr;
    const unsigned long long length = std::strtoull(length_text->c_str(), &end, 10);
    if (end != length_text->c_str() + length_text->size() || length > kMaxBodyBytes) {
      return Error::Transport("http: invalid content-length");
    }
    while (message.body.size() < length) {
      const long received = read(chunk.data(), chunk.size());
      if (received <= 0) return Error::Transport("http: connection closed mid-body");
      message.body.append(chunk.data(), static_cast<std::size_t>(received));
    }
    if (message.body.size() != length) return Error::Transport("http: excess body bytes");
  } else if (body_until_eof) {
    while (true) {
      if (message.body.size() > kMaxBodyBytes) return Error::Transport("http: body too large");
      const long received = read(chunk.data(), chunk.size());
      if (received < 0) return Error::Transport("http: read failed");
      if (received == 0) break;
      message.body.append(chunk.data(), static_cast<std::size_t>(received));
    }
  }
  return message;
}

bool ParseRequestLine(std::string_view line, std::string* method, std::string* target) {
  const auto first = line.find(' ');
  const auto second =
      first == std::string_view::npos ? std::string_view::npos : line.find(' ', first + 1);
  if (second == std::string_view::npos) return false;
  *method = std::string(line.substr(0, first));
  *target = std::string(line.substr(first + 1, second - first - 1));
  return true;
}

Outcome<int> ParseStatusLine(std::string_view line) {
  const auto space = line.find(' ');
  if (space == std::string_view::npos || line.size() < space + 4 || line.substr(0, 5) != "HTTP/") {
    return Error::Transport("http: malformed status line: " + std::string(line));
  }
  const int status = std::atoi(std::string(line.substr(space + 1)).c_str());
  if (status < 100 || status > 599) {
    return Error::Transport("http: implausible status in: " + std::string(line));
  }
  return status;
}

}  // namespace smithy::http
