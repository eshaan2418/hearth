// HTTP/1.1 request parsing (RFC 9112) and request-target sanitisation.
#ifndef HEARTH_HTTP_H
#define HEARTH_HTTP_H

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hearth {

// A parsed request head. All views point into the caller's buffer.
struct Request {
  std::string_view method;
  std::string_view target;  // raw request-target
  std::string_view path;    // target without the query
  std::string_view query;
  int minor_version = 1;  // HTTP/1.x
  std::vector<std::pair<std::string_view, std::string_view>> headers;
  bool keep_alive = true;
  std::size_t content_length = 0;
  std::size_t head_bytes = 0;  // bytes of request line + headers + final CRLF

  // Case-insensitive lookup of the first header with this name.
  std::optional<std::string_view> header(std::string_view name) const;
};

enum class Parse { kIncomplete, kComplete, kError };

struct ParseOptions {
  std::size_t max_head_bytes = 16 * 1024;
  std::size_t max_headers = 100;
};

// Parse one request head from `buf`. On kError, `*status` is the HTTP status to answer
// (400, 414, 431, 501, 505). The parser is strict where leniency enables request smuggling:
// bare LF line endings, obsolete line folding, whitespace before the colon, conflicting or
// malformed Content-Length, and Transfer-Encoding are all rejected.
Parse ParseRequest(std::string_view buf, Request* req, int* status, const ParseOptions& opts = {});

// Decode and normalise an origin-form path for filesystem lookup. Returns nullopt for paths
// that must be refused: invalid percent-encoding, NUL, backslash, or any ".." segment.
// The result always starts with '/' and contains no empty, "." or ".." segments.
std::optional<std::string> SanitizePath(std::string_view path);

const char* ReasonPhrase(int status);
std::string_view MimeType(std::string_view path);

}  // namespace hearth

#endif  // HEARTH_HTTP_H
