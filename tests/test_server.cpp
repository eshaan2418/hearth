#include <arpa/inet.h>
#include <gtest/gtest.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "hearth/server.h"

namespace fs = std::filesystem;

namespace {

struct Response {
  int status = 0;
  std::string head;
  std::string body;
  std::string header(const std::string& name) const {
    const auto p = head.find("\r\n" + name + ": ");
    if (p == std::string::npos) return "";
    const auto s = p + name.size() + 4;
    return head.substr(s, head.find("\r\n", s) - s);
  }
};

int Connect(int port) {
  const int fd = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in a{};
  a.sin_family = AF_INET;
  a.sin_port = htons(static_cast<uint16_t>(port));
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  timeval tv{5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
  if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) return -1;
  return fd;
}

void Send(int fd, const std::string& s) { ASSERT_EQ(write(fd, s.data(), s.size()), static_cast<ssize_t>(s.size())); }

// Read one response (head + Content-Length body; HEAD responses pass head_only).
bool ReadResponse(int fd, std::string* pending, Response* out, bool head_only = false) {
  char buf[65536];
  while (pending->find("\r\n\r\n") == std::string::npos) {
    const ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) return false;
    pending->append(buf, static_cast<std::size_t>(n));
  }
  const auto e = pending->find("\r\n\r\n");
  out->head = pending->substr(0, e + 2);
  out->status = std::atoi(out->head.c_str() + 9);
  const std::size_t len =
      head_only || out->status == 304 ? 0 : std::strtoull(out->header("Content-Length").c_str(), nullptr, 10);
  pending->erase(0, e + 4);
  while (pending->size() < len) {
    const ssize_t n = read(fd, buf, sizeof buf);
    if (n <= 0) return false;
    pending->append(buf, static_cast<std::size_t>(n));
  }
  out->body = pending->substr(0, len);
  pending->erase(0, len);
  return true;
}

Response Get(int port, const std::string& request, bool head_only = false) {
  const int fd = Connect(port);
  Send(fd, request);
  std::string pending;
  Response r;
  ReadResponse(fd, &pending, &r, head_only);
  close(fd);
  return r;
}

bool PeerClosed(int fd) {
  char c;
  return read(fd, &c, 1) == 0;
}

class ServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    root_ =
        fs::temp_directory_path() / ("hearth-test-" + std::to_string(::getpid()) + "-" + std::to_string(counter_++));
    fs::create_directories(root_ / "sub");
    std::ofstream(root_ / "index.html") << "<h1>home</h1>";
    std::ofstream(root_ / "sub" / "index.html") << "sub index";
    std::ofstream(root_ / "a.txt") << "hello world";
    big_.resize(3 << 20);
    std::mt19937 rng(1);
    for (auto& ch : big_) ch = static_cast<char>(rng());
    std::ofstream(root_ / "big.bin", std::ios::binary).write(big_.data(), static_cast<std::streamsize>(big_.size()));
    outside_ = fs::temp_directory_path() / ("hearth-secret-" + std::to_string(::getpid()));
    std::ofstream(outside_) << "secret";
    fs::create_symlink(outside_, root_ / "escape");
    hearth::Options o;
    o.root = root_.string();
    o.port = 0;
    o.threads = 2;
    o.header_timeout_ms = 400;
    o.idle_timeout_ms = 800;
    server_ = std::make_unique<hearth::Server>(o);
    server_->Start();
    port_ = server_->port();
  }
  void TearDown() override {
    server_->Stop(1000);
    fs::remove_all(root_);
    fs::remove(outside_);
  }
  static inline int counter_ = 0;
  fs::path root_, outside_;
  std::string big_;
  std::unique_ptr<hearth::Server> server_;
  int port_ = 0;
};

TEST_F(ServerTest, ServesFilesWithHeaders) {
  auto r = Get(port_, "GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.body, "hello world");
  EXPECT_EQ(r.header("Content-Type"), "text/plain; charset=utf-8");
  EXPECT_FALSE(r.header("ETag").empty());
  EXPECT_FALSE(r.header("Last-Modified").empty());
  r = Get(port_, "GET / HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_EQ(r.body, "<h1>home</h1>");
  r = Get(port_, "GET /sub HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_EQ(r.status, 301);
  EXPECT_EQ(r.header("Location"), "/sub/");
  r = Get(port_, "HEAD /a.txt HTTP/1.1\r\nHost: x\r\n\r\n", true);
  EXPECT_EQ(r.status, 200);
  EXPECT_EQ(r.header("Content-Length"), "11");
  EXPECT_EQ(Get(port_, "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n").status, 404);
  EXPECT_EQ(Get(port_, "DELETE /a.txt HTTP/1.1\r\nHost: x\r\n\r\n").status, 405);
}

TEST_F(ServerTest, RefusesTraversalAndSymlinkEscape) {
  EXPECT_EQ(Get(port_, "GET /../../etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n").status, 400);
  EXPECT_EQ(Get(port_, "GET /%2e%2e/%2e%2e/etc/passwd HTTP/1.1\r\nHost: x\r\n\r\n").status, 400);
  const auto r = Get(port_, "GET /escape HTTP/1.1\r\nHost: x\r\n\r\n");
  EXPECT_EQ(r.status, 404);
  EXPECT_EQ(r.body.find("secret"), std::string::npos);
}

TEST_F(ServerTest, LargeFileIsByteExact) {
  const auto r = Get(port_, "GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n");
  ASSERT_EQ(r.status, 200);
  EXPECT_EQ(r.body.size(), big_.size());
  EXPECT_TRUE(r.body == big_);
}

TEST_F(ServerTest, ConditionalRequests) {
  const auto r = Get(port_, "GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n");
  const auto etag = r.header("ETag"), lm = r.header("Last-Modified");
  EXPECT_EQ(Get(port_, "GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-None-Match: " + etag + "\r\n\r\n").status, 304);
  EXPECT_EQ(Get(port_, "GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-Modified-Since: " + lm + "\r\n\r\n").status, 304);
  EXPECT_EQ(Get(port_, "GET /a.txt HTTP/1.1\r\nHost: x\r\nIf-None-Match: \"other\"\r\n\r\n").status, 200);
}

TEST_F(ServerTest, Ranges) {
  auto r = Get(port_, "GET /a.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=0-4\r\n\r\n");
  EXPECT_EQ(r.status, 206);
  EXPECT_EQ(r.body, "hello");
  EXPECT_EQ(r.header("Content-Range"), "bytes 0-4/11");
  r = Get(port_, "GET /a.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=-5\r\n\r\n");
  EXPECT_EQ(r.body, "world");
  r = Get(port_, "GET /a.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=6-\r\n\r\n");
  EXPECT_EQ(r.body, "world");
  r = Get(port_, "GET /big.bin HTTP/1.1\r\nHost: x\r\nRange: bytes=1000000-1999999\r\n\r\n");
  EXPECT_EQ(r.status, 206);
  EXPECT_TRUE(r.body == big_.substr(1000000, 1000000));
  EXPECT_EQ(Get(port_, "GET /a.txt HTTP/1.1\r\nHost: x\r\nRange: bytes=50-60\r\n\r\n").status, 416);
}

TEST_F(ServerTest, KeepAliveAndPipelining) {
  const int fd = Connect(port_);
  std::string pipelined;
  for (int i = 0; i < 20; ++i) pipelined += "GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n";
  pipelined += "GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\nGET / HTTP/1.1\r\nHost: x\r\n\r\n";
  Send(fd, pipelined);
  std::string pending;
  for (int i = 0; i < 20; ++i) {
    Response r;
    ASSERT_TRUE(ReadResponse(fd, &pending, &r));
    ASSERT_EQ(r.body, "hello world") << i;
  }
  Response big, last;
  ASSERT_TRUE(ReadResponse(fd, &pending, &big));
  EXPECT_EQ(big.body.size(), big_.size());
  ASSERT_TRUE(ReadResponse(fd, &pending, &last));
  EXPECT_EQ(last.body, "<h1>home</h1>");  // responses stay in request order
  Send(fd, "GET /a.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
  Response c;
  ASSERT_TRUE(ReadResponse(fd, &pending, &c));
  EXPECT_EQ(c.header("Connection"), "close");
  EXPECT_TRUE(PeerClosed(fd));
  close(fd);
}

TEST_F(ServerTest, MalformedRequestGets400AndClose) {
  const int fd = Connect(port_);
  Send(fd, "GET / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n");
  std::string pending;
  Response r;
  ASSERT_TRUE(ReadResponse(fd, &pending, &r));
  EXPECT_EQ(r.status, 400);
  EXPECT_TRUE(PeerClosed(fd));
  close(fd);
}

TEST_F(ServerTest, SlowlorisAndIdleTimeouts) {
  const int slow = Connect(port_);
  Send(slow, "GET / HTTP/1.1\r\nHost: x\r\n");  // never finishes the head
  const int idle = Connect(port_);
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(PeerClosed(slow));  // closed by the 400 ms header timeout
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(3));
  EXPECT_TRUE(PeerClosed(idle));  // closed by the 800 ms idle timeout
  close(slow);
  close(idle);
  EXPECT_GE(server_->stats().timeouts, 2u);
}

TEST_F(ServerTest, ConcurrentClients) {
  std::atomic<int> bad{0};
  std::vector<std::thread> ts;
  for (int t = 0; t < 16; ++t) {
    ts.emplace_back([&] {
      const int fd = Connect(port_);
      std::string pending;
      for (int i = 0; i < 200; ++i) {
        Send(fd, i % 50 == 0 ? "GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n" : "GET /a.txt HTTP/1.1\r\nHost: x\r\n\r\n");
        Response r;
        if (!ReadResponse(fd, &pending, &r) || r.status != 200) ++bad;
      }
      close(fd);
    });
  }
  for (auto& t : ts) t.join();
  EXPECT_EQ(bad.load(), 0);
}

TEST_F(ServerTest, StopDrainsInFlightResponses) {
  const int fd = Connect(port_);
  Send(fd, "GET /big.bin HTTP/1.1\r\nHost: x\r\n\r\n");
  char first[1024];
  const ssize_t got = read(fd, first, sizeof first);
  ASSERT_GT(got, 0);  // the response has started; most of the 3 MB is still queued
  std::thread stopper([&] { server_->Stop(5000); });
  std::string pending(first, static_cast<std::size_t>(got));
  std::string rest;
  char buf[65536];
  ssize_t n;
  while ((n = read(fd, buf, sizeof buf)) > 0) rest.append(buf, static_cast<std::size_t>(n));
  stopper.join();
  pending += rest;
  const auto e = pending.find("\r\n\r\n");
  ASSERT_NE(e, std::string::npos);
  EXPECT_TRUE(pending.substr(e + 4) == big_);  // the whole body arrived, then the server closed
  close(fd);
}

}  // namespace
