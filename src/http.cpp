#include "hearth/http.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace hearth {

namespace {

bool IsTchar(unsigned char c) { return std::isalnum(c) != 0 || std::strchr("!#$%&'*+-.^_`|~", c) != nullptr; }

bool EqualsCI(std::string_view a, std::string_view b) {
  return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
           return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
         });
}

std::string_view TrimOWS(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool HasToken(std::string_view list, std::string_view token) {
  while (!list.empty()) {
    const std::size_t comma = list.find(',');
    const std::string_view item = TrimOWS(list.substr(0, comma));
    if (EqualsCI(item, token)) return true;
    if (comma == std::string_view::npos) break;
    list.remove_prefix(comma + 1);
  }
  return false;
}

int HexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

}  // namespace

std::optional<std::string_view> Request::header(std::string_view name) const {
  for (const auto& [k, v] : headers) {
    if (EqualsCI(k, name)) return v;
  }
  return std::nullopt;
}

Parse ParseRequest(std::string_view buf, Request* req, int* status, const ParseOptions& opts) {
  // Only parse once the whole head is present; this keeps the parser stateless.
  const std::size_t end = buf.find("\r\n\r\n");
  if (end == std::string_view::npos) {
    if (buf.size() > opts.max_head_bytes) {
      *status = buf.find("\r\n") == std::string_view::npos ? 414 : 431;
      return Parse::kError;
    }
    return Parse::kIncomplete;
  }
  if (end + 4 > opts.max_head_bytes) {
    *status = 431;
    return Parse::kError;
  }
  *status = 400;
  *req = Request{};
  req->head_bytes = end + 4;
  const std::string_view head = buf.substr(0, end + 2);  // includes the last header's CRLF
  if (head.find('\n') != std::string_view::npos) {
    // Every LF must be preceded by CR (no bare LF line endings).
    for (std::size_t i = 0; i < head.size(); ++i) {
      if (head[i] == '\n' && (i == 0 || head[i - 1] != '\r')) return Parse::kError;
      if (head[i] == '\r' && (i + 1 >= head.size() || head[i + 1] != '\n')) return Parse::kError;
    }
  }

  // Request line: method SP target SP HTTP/1.x
  std::size_t eol = head.find("\r\n");
  const std::string_view line = head.substr(0, eol);
  const std::size_t sp1 = line.find(' ');
  const std::size_t sp2 = sp1 == std::string_view::npos ? sp1 : line.find(' ', sp1 + 1);
  if (sp1 == 0 || sp1 == std::string_view::npos || sp2 == std::string_view::npos || sp2 == sp1 + 1 ||
      line.find(' ', sp2 + 1) != std::string_view::npos) {
    return Parse::kError;
  }
  req->method = line.substr(0, sp1);
  if (!std::all_of(req->method.begin(), req->method.end(),
                   [](char c) { return IsTchar(static_cast<unsigned char>(c)); })) {
    return Parse::kError;
  }
  req->target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  for (char c : req->target) {
    if (static_cast<unsigned char>(c) <= 0x20 || c == 0x7f) return Parse::kError;
  }
  const std::string_view version = line.substr(sp2 + 1);
  if (version.size() != 8 || version.substr(0, 5) != "HTTP/" || version[6] != '.' ||
      !std::isdigit(static_cast<unsigned char>(version[5])) || !std::isdigit(static_cast<unsigned char>(version[7]))) {
    return Parse::kError;
  }
  if (version[5] != '1') {
    *status = 505;
    return Parse::kError;
  }
  req->minor_version = version[7] - '0';
  if (req->target == "*") {
    if (req->method != "OPTIONS") return Parse::kError;
  } else if (req->target.empty() || req->target.front() != '/') {
    return Parse::kError;  // only origin-form (absolute-form is for proxies)
  }
  const std::size_t q = req->target.find('?');
  req->path = req->target.substr(0, q);
  req->query = q == std::string_view::npos ? std::string_view{} : req->target.substr(q + 1);

  // Header fields.
  std::size_t pos = eol + 2;
  int hosts = 0;
  bool have_cl = false;
  while (pos < head.size()) {
    eol = head.find("\r\n", pos);
    const std::string_view field = head.substr(pos, eol - pos);
    pos = eol + 2;
    if (field.front() == ' ' || field.front() == '\t') return Parse::kError;  // obs-fold
    const std::size_t colon = field.find(':');
    if (colon == 0 || colon == std::string_view::npos) return Parse::kError;
    const std::string_view name = field.substr(0, colon);
    if (!std::all_of(name.begin(), name.end(), [](char c) { return IsTchar(static_cast<unsigned char>(c)); })) {
      return Parse::kError;  // includes whitespace before the colon
    }
    const std::string_view value = TrimOWS(field.substr(colon + 1));
    for (char c : value) {
      if ((static_cast<unsigned char>(c) < 0x20 && c != '\t') || c == 0x7f) return Parse::kError;
    }
    if (req->headers.size() >= opts.max_headers) {
      *status = 431;
      return Parse::kError;
    }
    req->headers.emplace_back(name, value);
    if (EqualsCI(name, "host")) {
      ++hosts;
    } else if (EqualsCI(name, "content-length")) {
      if (value.empty() || value.size() > 15 ||
          !std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return Parse::kError;
      }
      std::size_t n = 0;
      for (char c : value) n = n * 10 + static_cast<std::size_t>(c - '0');
      if (have_cl && n != req->content_length) return Parse::kError;  // conflicting lengths
      have_cl = true;
      req->content_length = n;
    } else if (EqualsCI(name, "transfer-encoding")) {
      *status = have_cl || req->header("content-length") ? 400 : 501;
      return Parse::kError;  // chunked request bodies are not supported; TE + CL is smuggling
    }
  }
  if (req->minor_version >= 1 && hosts != 1) return Parse::kError;
  if (const auto conn = req->header("connection")) {
    if (HasToken(*conn, "close")) {
      req->keep_alive = false;
    } else if (req->minor_version == 0) {
      req->keep_alive = HasToken(*conn, "keep-alive");
    }
  } else {
    req->keep_alive = req->minor_version >= 1;
  }
  *status = 0;
  return Parse::kComplete;
}

std::optional<std::string> SanitizePath(std::string_view path) {
  std::string decoded;
  decoded.reserve(path.size());
  for (std::size_t i = 0; i < path.size(); ++i) {
    char c = path[i];
    if (c == '%') {
      if (i + 2 >= path.size()) return std::nullopt;
      const int hi = HexVal(path[i + 1]), lo = HexVal(path[i + 2]);
      if (hi < 0 || lo < 0) return std::nullopt;
      c = static_cast<char>(hi * 16 + lo);
      i += 2;
    }
    if (c == '\0' || c == '\\') return std::nullopt;
    decoded.push_back(c);
  }
  std::string out;
  std::size_t i = 0;
  while (i < decoded.size()) {
    while (i < decoded.size() && decoded[i] == '/') ++i;
    const std::size_t j = std::min(decoded.find('/', i), decoded.size());
    const std::string_view seg(decoded.data() + i, j - i);
    if (seg == "..") return std::nullopt;
    if (!seg.empty() && seg != ".") {
      out.push_back('/');
      out.append(seg);
    }
    i = j;
  }
  if (out.empty() || (!decoded.empty() && decoded.back() == '/')) out.push_back('/');
  return out;
}

const char* ReasonPhrase(int s) {
  switch (s) {
    case 200:
      return "OK";
    case 206:
      return "Partial Content";
    case 301:
      return "Moved Permanently";
    case 304:
      return "Not Modified";
    case 400:
      return "Bad Request";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 408:
      return "Request Timeout";
    case 413:
      return "Content Too Large";
    case 414:
      return "URI Too Long";
    case 416:
      return "Range Not Satisfiable";
    case 431:
      return "Request Header Fields Too Large";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    case 503:
      return "Service Unavailable";
    case 505:
      return "HTTP Version Not Supported";
    default:
      return "Unknown";
  }
}

std::string_view MimeType(std::string_view path) {
  static constexpr std::pair<std::string_view, std::string_view> kTypes[] = {
      {".html", "text/html; charset=utf-8"},
      {".htm", "text/html; charset=utf-8"},
      {".css", "text/css; charset=utf-8"},
      {".js", "text/javascript; charset=utf-8"},
      {".json", "application/json"},
      {".txt", "text/plain; charset=utf-8"},
      {".md", "text/markdown; charset=utf-8"},
      {".svg", "image/svg+xml"},
      {".png", "image/png"},
      {".jpg", "image/jpeg"},
      {".jpeg", "image/jpeg"},
      {".gif", "image/gif"},
      {".webp", "image/webp"},
      {".ico", "image/x-icon"},
      {".wasm", "application/wasm"},
      {".pdf", "application/pdf"},
      {".woff2", "font/woff2"},
      {".xml", "application/xml"},
      {".mp4", "video/mp4"},
      {".gz", "application/gzip"},
  };
  const std::size_t dot = path.rfind('.');
  const std::size_t slash = path.rfind('/');
  if (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash)) {
    const std::string_view ext = path.substr(dot);
    for (const auto& [e, t] : kTypes) {
      if (EqualsCI(e, ext)) return t;
    }
  }
  return "application/octet-stream";
}

}  // namespace hearth
