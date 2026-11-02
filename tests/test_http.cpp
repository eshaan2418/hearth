#include <gtest/gtest.h>

#include "hearth/http.h"

namespace {

using hearth::Parse;
using hearth::ParseRequest;
using hearth::Request;

Parse P(const std::string& s, Request* r, int* st) { return ParseRequest(s, r, st); }

TEST(Parser, SimpleGetAndHeaders) {
  Request r;
  int st = 0;
  const std::string s = "GET /a/b?x=1 HTTP/1.1\r\nHost: example.com\r\nUser-Agent:  t  \r\n\r\nNEXT";
  ASSERT_EQ(P(s, &r, &st), Parse::kComplete);
  EXPECT_EQ(r.method, "GET");
  EXPECT_EQ(r.path, "/a/b");
  EXPECT_EQ(r.query, "x=1");
  EXPECT_EQ(r.header("HOST").value(), "example.com");
  EXPECT_EQ(r.header("user-agent").value(), "t");  // OWS trimmed
  EXPECT_TRUE(r.keep_alive);
  EXPECT_EQ(r.head_bytes, s.size() - 4);
}

TEST(Parser, IncompleteUntilBlankLine) {
  const std::string full = "GET / HTTP/1.1\r\nHost: h\r\n\r\n";
  for (std::size_t i = 0; i < full.size(); ++i) {
    Request r;
    int st = 0;
    EXPECT_EQ(P(full.substr(0, i), &r, &st), Parse::kIncomplete) << i;
  }
}

TEST(Parser, KeepAliveRules) {
  Request r;
  int st;
  ASSERT_EQ(P("GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", &r, &st), Parse::kComplete);
  EXPECT_FALSE(r.keep_alive);
  ASSERT_EQ(P("GET / HTTP/1.0\r\n\r\n", &r, &st), Parse::kComplete);
  EXPECT_FALSE(r.keep_alive);  // 1.0 defaults to close, and Host is optional
  ASSERT_EQ(P("GET / HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n", &r, &st), Parse::kComplete);
  EXPECT_TRUE(r.keep_alive);
}

struct Bad {
  const char* req;
  int status;
};

TEST(Parser, RejectsMalformedAndSmugglingVectors) {
  const Bad cases[] = {
      {"GET / HTTP/1.1\r\n\r\n", 400},                                                       // missing Host
      {"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400},                                 // two Hosts
      {"GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\n", 400},  // conflicting CL
      {"GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 5, 5\r\n\r\n", 400},
      {"GET / HTTP/1.1\r\nHost: a\r\nContent-Length: -1\r\n\r\n", 400},
      {"POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n", 501},
      {"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 3\r\nTransfer-Encoding: chunked\r\n\r\n", 400},  // CL + TE
      {"GET / HTTP/1.1\r\nHost : a\r\n\r\n", 400},            // whitespace before colon
      {"GET / HTTP/1.1\r\nHost: a\r\n folded\r\n\r\n", 400},  // obs-fold
      {"GET / HTTP/1.1\nHost: a\r\n\r\n", 400},               // bare LF
      {"GET  / HTTP/1.1\r\nHost: a\r\n\r\n", 400},            // double space
      {"GET http://x/ HTTP/1.1\r\nHost: a\r\n\r\n", 400},     // absolute-form
      {"G@T / HTTP/1.1\r\nHost: a\r\n\r\n", 400},
      {"GET / HTTP/2.0\r\nHost: a\r\n\r\n", 505},
      {"GET / HTTP/1.1\r\nHost: a\r\nX: a\x01b\r\n\r\n", 400},  // control character in value
  };
  for (const auto& c : cases) {
    Request r;
    int st = 0;
    EXPECT_EQ(P(c.req, &r, &st), Parse::kError) << c.req;
    EXPECT_EQ(st, c.status) << c.req;
  }
}

TEST(Parser, Limits) {
  Request r;
  int st = 0;
  std::string big = "GET / HTTP/1.1\r\nHost: a\r\nX: " + std::string(20000, 'y');
  EXPECT_EQ(P(big, &r, &st), Parse::kError);
  EXPECT_EQ(st, 431);
  std::string longuri = "GET /" + std::string(20000, 'a');
  EXPECT_EQ(P(longuri, &r, &st), Parse::kError);
  EXPECT_EQ(st, 414);
}

TEST(Path, SanitizeBlocksTraversal) {
  using hearth::SanitizePath;
  EXPECT_EQ(SanitizePath("/a/./b//c").value(), "/a/b/c");
  EXPECT_EQ(SanitizePath("/").value(), "/");
  EXPECT_EQ(SanitizePath("/dir/").value(), "/dir/");
  EXPECT_EQ(SanitizePath("/a%20b").value(), "/a b");
  for (const char* bad :
       {"/../etc/passwd", "/a/../../x", "/%2e%2e/x", "/%2E%2E%2fetc", "/a%00b", "/a\\b", "/%zz", "/%2"}) {
    EXPECT_FALSE(SanitizePath(bad).has_value()) << bad;
  }
}

TEST(Mime, Types) {
  EXPECT_EQ(hearth::MimeType("/x/INDEX.HTML"), "text/html; charset=utf-8");
  EXPECT_EQ(hearth::MimeType("/a.b/c"), "application/octet-stream");
}

}  // namespace
