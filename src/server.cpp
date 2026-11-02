// Event loop, connection state machine, static file handling.
#include "hearth/server.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <sys/event.h>
#else
#include <sys/epoll.h>
#include <sys/sendfile.h>
#endif

#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <list>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include "hearth/http.h"

namespace hearth {

namespace {

using Clock = std::chrono::steady_clock;

void SetNonBlocking(int fd) {
  const int fl = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, fl | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}

// --- poller -------------------------------------------------------------------------

struct PollEvent {
  int fd;
  bool readable;
  bool writable;
  bool hangup;
};

class Poller {
 public:
  Poller() {
#if defined(__APPLE__)
    fd_ = kqueue();
#else
    fd_ = epoll_create1(EPOLL_CLOEXEC);
#endif
    if (fd_ < 0) throw std::runtime_error("poller create failed");
  }
  ~Poller() { close(fd_); }
  Poller(const Poller&) = delete;
  Poller& operator=(const Poller&) = delete;

  // Level-triggered read interest is always on; write interest is toggled.
  void Add(int fd) {
#if defined(__APPLE__)
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    kevent(fd_, &ev, 1, nullptr, 0, nullptr);
#else
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP;
    ev.data.fd = fd;
    epoll_ctl(fd_, EPOLL_CTL_ADD, fd, &ev);
#endif
  }

  void WantWrite(int fd, bool on) {
#if defined(__APPLE__)
    struct kevent ev;
    EV_SET(&ev, fd, EVFILT_WRITE, on ? EV_ADD : EV_DELETE, 0, 0, nullptr);
    kevent(fd_, &ev, 1, nullptr, 0, nullptr);
#else
    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLRDHUP | (on ? EPOLLOUT : 0);
    ev.data.fd = fd;
    epoll_ctl(fd_, EPOLL_CTL_MOD, fd, &ev);
#endif
  }

  // Closing the fd removes it from kqueue/epoll automatically.

  int Wait(std::vector<PollEvent>* out, int timeout_ms) {
    out->clear();
#if defined(__APPLE__)
    struct kevent evs[256];
    timespec ts{timeout_ms / 1000, static_cast<long>(timeout_ms % 1000) * 1000000L};
    const int n = kevent(fd_, nullptr, 0, evs, 256, &ts);
    for (int i = 0; i < n; ++i) {
      out->push_back({static_cast<int>(evs[i].ident), evs[i].filter == EVFILT_READ, evs[i].filter == EVFILT_WRITE,
                      (evs[i].flags & EV_EOF) != 0});
    }
#else
    epoll_event evs[256];
    const int n = epoll_wait(fd_, evs, 256, timeout_ms);
    for (int i = 0; i < n; ++i) {
      out->push_back({evs[i].data.fd, (evs[i].events & EPOLLIN) != 0, (evs[i].events & EPOLLOUT) != 0,
                      (evs[i].events & (EPOLLHUP | EPOLLERR)) != 0});
    }
#endif
    return n;
  }

 private:
  int fd_;
};

// --- file cache -----------------------------------------------------------------------

struct OpenFile {
  int fd = -1;
  off_t size = 0;
  time_t mtime = 0;
  std::string etag;
  std::string last_modified;
  std::string_view mime;
  Clock::time_point checked;
  ~OpenFile() {
    if (fd >= 0) close(fd);
  }
};

std::string HttpDate(time_t t) {
  tm g{};
  gmtime_r(&t, &g);
  char buf[64];
  strftime(buf, sizeof buf, "%a, %d %b %Y %H:%M:%S GMT", &g);
  return buf;
}

// --- connection -------------------------------------------------------------------------

struct Chunk {
  std::string data;                // headers or small bodies
  std::shared_ptr<OpenFile> file;  // or a file range sent with sendfile
  off_t offset = 0;
  off_t length = 0;
};

struct Conn {
  int fd;
  std::string in;
  std::deque<Chunk> out;
  Clock::time_point last_activity;
  Clock::time_point head_started;
  bool head_pending = false;  // bytes of an incomplete head are buffered
  bool want_write = false;
  bool close_after = false;
  std::size_t body_to_discard = 0;
};

}  // namespace

struct Server::Worker {
  const Options& opts;
  const std::string& root_real;
  int listen_fd;
  std::atomic<bool>& stopping;
  std::atomic<long long>& stop_deadline_ns;
  Poller poller;
  std::unordered_map<int, std::unique_ptr<Conn>> conns;
  // LRU file cache: path -> entry
  std::list<std::pair<std::string, std::shared_ptr<OpenFile>>> lru;
  std::unordered_map<std::string, decltype(lru)::iterator> cache;
  std::string date;
  time_t date_sec = 0;
  std::atomic<std::uint64_t> accepted{0}, requests{0}, bytes_sent{0}, timeouts{0}, open_conns{0};

  Worker(const Options& o, const std::string& rr, int lfd, std::atomic<bool>& s, std::atomic<long long>& dl)
      : opts(o), root_real(rr), listen_fd(lfd), stopping(s), stop_deadline_ns(dl) {
    poller.Add(listen_fd);
  }

  const std::string& Date() {
    const time_t now = time(nullptr);
    if (now != date_sec) {
      date_sec = now;
      date = HttpDate(now);
    }
    return date;
  }

  void Run() {
    std::vector<PollEvent> events;
    auto last_sweep = Clock::now();
    bool accepting = true;
    while (true) {
      if (stopping.load(std::memory_order_relaxed) && accepting) {
        accepting = false;  // the listening fd is closed by Stop(); stop polling it
      }
      if (!accepting && (AllIdle() || Clock::now().time_since_epoch().count() > stop_deadline_ns.load())) break;
      poller.Wait(&events, 250);
      for (const PollEvent& ev : events) {
        if (ev.fd == listen_fd) {
          if (accepting) Accept();
          continue;
        }
        auto it = conns.find(ev.fd);
        if (it == conns.end()) continue;
        Conn* c = it->second.get();
        if (ev.readable && !OnReadable(c)) continue;
        if (ev.writable && !Flush(c)) continue;
        if (ev.hangup && c->out.empty()) Close(c);
      }
      const auto now = Clock::now();
      if (now - last_sweep >= std::chrono::milliseconds(500)) {
        last_sweep = now;
        Sweep(now, !accepting);
      }
    }
    for (auto& [fd, c] : conns) close(fd);
    conns.clear();
  }

  bool AllIdle() const {
    for (const auto& [fd, c] : conns) {
      if (!c->out.empty()) return false;
    }
    return true;
  }

  void Accept() {
    while (true) {
      sockaddr_storage ss{};
      socklen_t len = sizeof ss;
      const int fd = accept(listen_fd, reinterpret_cast<sockaddr*>(&ss), &len);
      if (fd < 0) return;  // EAGAIN (another worker took it) or a transient error
      if (open_conns.load() >= static_cast<std::uint64_t>(opts.max_connections)) {
        close(fd);
        continue;
      }
      SetNonBlocking(fd);
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
#if defined(__APPLE__)
      setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
      auto c = std::make_unique<Conn>();
      c->fd = fd;
      c->last_activity = Clock::now();
      poller.Add(fd);
      conns.emplace(fd, std::move(c));
      accepted.fetch_add(1, std::memory_order_relaxed);
      open_conns.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void Close(Conn* c) {
    close(c->fd);
    open_conns.fetch_sub(1, std::memory_order_relaxed);
    conns.erase(c->fd);  // destroys *c
  }

  // Returns false if the connection was closed.
  bool OnReadable(Conn* c) {
    char buf[16384];
    while (true) {
      const ssize_t n = read(c->fd, buf, sizeof buf);
      if (n > 0) {
        c->last_activity = Clock::now();
        std::size_t skip = 0;
        if (c->body_to_discard > 0) {
          skip = std::min<std::size_t>(c->body_to_discard, static_cast<std::size_t>(n));
          c->body_to_discard -= skip;
        }
        c->in.append(buf + skip, static_cast<std::size_t>(n) - skip);
        if (c->in.size() > 64 * 1024) break;  // backpressure: parse what we have first
        continue;
      }
      if (n == 0) {  // peer closed its sending side
        if (c->out.empty()) {
          Close(c);
          return false;
        }
        c->close_after = true;
        break;
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      Close(c);
      return false;
    }
    return Process(c);
  }

  bool Process(Conn* c) {
    while (!c->close_after && c->body_to_discard == 0 && !c->in.empty()) {
      Request req;
      int status = 0;
      const Parse r = ParseRequest(c->in, &req, &status);
      if (r == Parse::kIncomplete) {
        if (!c->head_pending) {
          c->head_pending = true;
          c->head_started = Clock::now();
        }
        break;
      }
      c->head_pending = false;
      if (r == Parse::kError) {
        Error(c, status);
        c->close_after = true;
        c->in.clear();
        break;
      }
      requests.fetch_add(1, std::memory_order_relaxed);
      if (req.content_length > opts.max_body_bytes) {
        Error(c, 413);
        c->close_after = true;
        c->in.clear();
        break;
      }
      const std::size_t consumed = req.head_bytes;
      Handle(c, req);
      // Request bodies are not used by a static server: discard them.
      const std::size_t available = c->in.size() - consumed;
      const std::size_t body_now = std::min(available, req.content_length);
      c->in.erase(0, consumed + body_now);
      c->body_to_discard = req.content_length - body_now;
      if (!req.keep_alive) {
        c->close_after = true;
        c->in.clear();
      }
    }
    return Flush(c);
  }

  void Respond(Conn* c, int status, std::string_view extra_headers, std::string_view body, bool head_only = false) {
    std::string h;
    h.reserve(256 + body.size());
    h += "HTTP/1.1 ";
    h += std::to_string(status);
    h += ' ';
    h += ReasonPhrase(status);
    h += "\r\nServer: hearth\r\nDate: ";
    h += Date();
    h += "\r\nContent-Length: ";
    h += std::to_string(body.size());
    h += "\r\n";
    h += extra_headers;
    if (c->close_after) h += "Connection: close\r\n";
    h += "\r\n";
    if (!head_only) h += body;
    c->out.push_back(Chunk{std::move(h), nullptr, 0, 0});
  }

  void Error(Conn* c, int status) {
    c->close_after = c->close_after || status >= 400;
    std::string body = std::to_string(status) + " " + ReasonPhrase(status) + "\n";
    Respond(c, status, "Content-Type: text/plain; charset=utf-8\r\n", body);
  }

  std::shared_ptr<OpenFile> Lookup(const std::string& rel, bool* is_dir) {
    *is_dir = false;
    const auto now = Clock::now();
    if (auto it = cache.find(rel); it != cache.end()) {
      auto f = it->second->second;
      if (now - f->checked < std::chrono::seconds(1)) {
        lru.splice(lru.begin(), lru, it->second);
        return f;
      }
      // Revalidate at most once per second: reopen if the file changed or disappeared.
      struct stat st{};
      const std::string full = root_real + rel;
      if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size == f->size && st.st_mtime == f->mtime) {
        f->checked = now;
        lru.splice(lru.begin(), lru, it->second);
        return f;
      }
      lru.erase(it->second);
      cache.erase(it);
    }
    const std::string full = root_real + rel;
    char resolved[PATH_MAX];
    if (realpath(full.c_str(), resolved) == nullptr) return nullptr;
    // Symlinks must not lead outside the root.
    const std::string_view r(resolved);
    if (r.substr(0, root_real.size()) != root_real || (r.size() > root_real.size() && r[root_real.size()] != '/'))
      return nullptr;
    struct stat st{};
    if (stat(resolved, &st) != 0) return nullptr;
    if (S_ISDIR(st.st_mode)) {
      *is_dir = true;
      return nullptr;
    }
    if (!S_ISREG(st.st_mode)) return nullptr;
    const int fd = open(resolved, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    auto f = std::make_shared<OpenFile>();
    f->fd = fd;
    f->size = st.st_size;
    f->mtime = st.st_mtime;
    char etag[64];
    std::snprintf(etag, sizeof etag, "\"%llx-%llx\"", static_cast<unsigned long long>(st.st_mtime),
                  static_cast<unsigned long long>(st.st_size));
    f->etag = etag;
    f->last_modified = HttpDate(st.st_mtime);
    f->mime = MimeType(rel);
    f->checked = now;
    lru.emplace_front(rel, f);
    cache[rel] = lru.begin();
    if (lru.size() > 4096) {  // in-flight sendfiles keep their entry alive via shared_ptr
      cache.erase(lru.back().first);
      lru.pop_back();
    }
    return f;
  }

  void Handle(Conn* c, const Request& req) {
    const bool head = req.method == "HEAD";
    if (req.method != "GET" && !head) {
      Respond(c, 405, "Allow: GET, HEAD\r\nContent-Type: text/plain; charset=utf-8\r\n", "405 Method Not Allowed\n");
      return;
    }
    if (!req.keep_alive) c->close_after = true;
    auto clean = SanitizePath(req.path);
    if (!clean) {
      Respond(c, 400, "Content-Type: text/plain; charset=utf-8\r\n", "400 Bad Request\n");
      return;
    }
    std::string rel = *clean;
    if (rel.back() == '/') rel += "index.html";
    bool is_dir = false;
    auto f = Lookup(rel, &is_dir);
    if (!f && is_dir) {
      std::string loc = "Location: " + std::string(req.path) + "/\r\n";
      Respond(c, 301, loc, "");
      return;
    }
    if (!f) {
      Respond(c, 404, "Content-Type: text/plain; charset=utf-8\r\n", "404 Not Found\n");
      return;
    }
    // Conditional requests: If-None-Match takes precedence over If-Modified-Since.
    if (const auto inm = req.header("if-none-match")) {
      if (*inm == f->etag || *inm == "*") {
        std::string h = "ETag: " + f->etag + "\r\n";
        RespondNotModified(c, h);
        return;
      }
    } else if (const auto ims = req.header("if-modified-since")) {
      if (*ims == f->last_modified) {
        RespondNotModified(c, "ETag: " + f->etag + "\r\n");
        return;
      }
    }
    off_t start = 0, len = f->size;
    int status = 200;
    if (const auto range = req.header("range")) {
      if (!ParseRange(*range, f->size, &start, &len)) {
        std::string h = "Content-Range: bytes */" + std::to_string(f->size) + "\r\n";
        Respond(c, 416, h, "");
        return;
      }
      status = 206;
    }
    std::string h;
    h.reserve(320);
    h += status == 200 ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 206 Partial Content\r\n";
    h += "Server: hearth\r\nDate: ";
    h += Date();
    h += "\r\nContent-Type: ";
    h += f->mime;
    h += "\r\nContent-Length: ";
    h += std::to_string(len);
    h += "\r\nLast-Modified: ";
    h += f->last_modified;
    h += "\r\nETag: ";
    h += f->etag;
    h += "\r\nAccept-Ranges: bytes\r\n";
    if (status == 206) {
      h += "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(start + len - 1) + "/" +
           std::to_string(f->size) + "\r\n";
    }
    if (c->close_after) h += "Connection: close\r\n";
    h += "\r\n";
    c->out.push_back(Chunk{std::move(h), nullptr, 0, 0});
    if (!head && len > 0) c->out.push_back(Chunk{std::string(), f, start, len});
  }

  void RespondNotModified(Conn* c, const std::string& extra) {
    std::string h = "HTTP/1.1 304 Not Modified\r\nServer: hearth\r\nDate: " + Date() + "\r\n" + extra;
    if (c->close_after) h += "Connection: close\r\n";
    h += "\r\n";
    c->out.push_back(Chunk{std::move(h), nullptr, 0, 0});
  }

  // A single "bytes=a-b", "bytes=a-", or "bytes=-n" range; multi-range requests are served whole
  // by returning the full range (RFC 9110 allows ignoring Range).
  static bool ParseRange(std::string_view v, off_t size, off_t* start, off_t* len) {
    if (v.substr(0, 6) != "bytes=" || v.find(',') != std::string_view::npos) {
      *start = 0;
      *len = size;
      return true;
    }
    v.remove_prefix(6);
    const std::size_t dash = v.find('-');
    if (dash == std::string_view::npos) return false;
    auto num = [](std::string_view s, off_t* out) {
      if (s.empty() || s.size() > 18) return false;
      off_t n = 0;
      for (char ch : s) {
        if (ch < '0' || ch > '9') return false;
        n = n * 10 + (ch - '0');
      }
      *out = n;
      return true;
    };
    off_t a = 0, b = 0;
    const bool has_a = num(v.substr(0, dash), &a);
    const bool has_b = num(v.substr(dash + 1), &b);
    if (!has_a && has_b) {  // suffix range: last b bytes
      if (b == 0) return false;
      *start = b >= size ? 0 : size - b;
      *len = size - *start;
      return size > 0;
    }
    if (!has_a || a >= size) return false;
    if (!has_b || b >= size) b = size - 1;
    if (b < a) return false;
    *start = a;
    *len = b - a + 1;
    return true;
  }

  // Write queued output until done or EAGAIN. Returns false if the connection was closed.
  bool Flush(Conn* c) {
    while (!c->out.empty()) {
      Chunk& ch = c->out.front();
      if (!ch.file && c->out.size() >= 2 && c->out[1].file && !ch.data.empty()) {
        // Response headers followed by a file: send both in one go, so the headers do not
        // leave as a separate small packet (macOS: sendfile header vector; Linux: MSG_MORE).
#if defined(__APPLE__)
        Chunk& fch = c->out[1];
        iovec hv{ch.data.data(), ch.data.size()};
        sf_hdtr hdtr{&hv, 1, nullptr, 0};
        off_t sent = static_cast<off_t>(ch.data.size()) + fch.length;
        const int rc = sendfile(fch.file->fd, c->fd, fch.offset, &sent, &hdtr, 0);
        const bool again = rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
        if (rc < 0 && !again && errno != EINTR) {
          Close(c);
          return false;
        }
        bytes_sent.fetch_add(static_cast<std::uint64_t>(sent), std::memory_order_relaxed);
        const off_t hdr = static_cast<off_t>(ch.data.size());
        if (sent < hdr) {
          ch.data.erase(0, static_cast<std::size_t>(sent));
          if (again || sent == 0) return WaitWritable(c);
          continue;
        }
        c->out.pop_front();
        Chunk& f2 = c->out.front();
        f2.offset += sent - hdr;
        f2.length -= sent - hdr;
        if (f2.length == 0) {
          c->out.pop_front();
          continue;
        }
        if (again || sent == hdr) return WaitWritable(c);
        continue;
#else
        const ssize_t w = send(c->fd, ch.data.data(), ch.data.size(), MSG_MORE | MSG_NOSIGNAL);
        if (w < 0) {
          if (errno == EINTR) continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK) return WaitWritable(c);
          Close(c);
          return false;
        }
        bytes_sent.fetch_add(static_cast<std::uint64_t>(w), std::memory_order_relaxed);
        if (static_cast<std::size_t>(w) < ch.data.size()) {
          ch.data.erase(0, static_cast<std::size_t>(w));
        } else {
          c->out.pop_front();
        }
        continue;
#endif
      }
      if (!ch.file) {
        // Gather consecutive memory chunks into one writev.
        iovec iov[16];
        int n = 0;
        for (auto it = c->out.begin(); it != c->out.end() && !it->file && n < 16; ++it, ++n) {
          iov[n].iov_base = it->data.data();
          iov[n].iov_len = it->data.size();
        }
        const ssize_t w = writev(c->fd, iov, n);
        if (w < 0) {
          if (errno == EINTR) continue;
          if (errno == EAGAIN || errno == EWOULDBLOCK) return WaitWritable(c);
          Close(c);
          return false;
        }
        bytes_sent.fetch_add(static_cast<std::uint64_t>(w), std::memory_order_relaxed);
        std::size_t left = static_cast<std::size_t>(w);
        while (left > 0) {
          Chunk& f = c->out.front();
          if (left >= f.data.size()) {
            left -= f.data.size();
            c->out.pop_front();
          } else {
            f.data.erase(0, left);
            left = 0;
          }
        }
        continue;
      }
      off_t sent = 0;
#if defined(__APPLE__)
      sent = ch.length;
      const int rc = sendfile(ch.file->fd, c->fd, ch.offset, &sent, nullptr, 0);
      const bool again = rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
      if (rc < 0 && !again && errno != EINTR) {
        Close(c);
        return false;
      }
#else
      off_t off = ch.offset;
      const ssize_t rc = sendfile(c->fd, ch.file->fd, &off, static_cast<std::size_t>(ch.length));
      const bool again = rc < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
      if (rc < 0 && !again && errno != EINTR) {
        Close(c);
        return false;
      }
      sent = rc > 0 ? rc : 0;
#endif
      bytes_sent.fetch_add(static_cast<std::uint64_t>(sent), std::memory_order_relaxed);
      ch.offset += sent;
      ch.length -= sent;
      if (ch.length == 0) {
        c->out.pop_front();
        continue;
      }
      if (again || sent == 0) return WaitWritable(c);
    }
    if (c->want_write) {
      c->want_write = false;
      poller.WantWrite(c->fd, false);
    }
    if (c->close_after) {
      Close(c);
      return false;
    }
    // Pipelined requests may be waiting behind the response we just finished.
    if (!c->in.empty() && !c->head_pending) return Process(c);
    return true;
  }

  bool WaitWritable(Conn* c) {
    if (!c->want_write) {
      c->want_write = true;
      poller.WantWrite(c->fd, true);
    }
    c->last_activity = Clock::now();
    return true;
  }

  void Sweep(Clock::time_point now, bool shutting_down) {
    std::vector<Conn*> doomed;
    for (auto& [fd, c] : conns) {
      const bool busy = !c->out.empty();
      if (c->head_pending && now - c->head_started > std::chrono::milliseconds(opts.header_timeout_ms)) {
        doomed.push_back(c.get());  // slowloris: head not completed in time
      } else if (!busy && (shutting_down || now - c->last_activity > std::chrono::milliseconds(opts.idle_timeout_ms))) {
        doomed.push_back(c.get());
      } else if (busy && now - c->last_activity > std::chrono::milliseconds(opts.idle_timeout_ms)) {
        doomed.push_back(c.get());  // client stopped reading
      }
    }
    for (Conn* c : doomed) {
      timeouts.fetch_add(1, std::memory_order_relaxed);
      Close(c);
    }
  }
};

Server::Server(Options opts) : opts_(std::move(opts)) {}

Server::~Server() { Stop(0); }

void Server::Start() {
  char resolved[PATH_MAX];
  if (realpath(opts_.root.c_str(), resolved) == nullptr) throw std::runtime_error("root does not exist: " + opts_.root);
  root_real_ = resolved;
  if (root_real_.size() > 1 && root_real_.back() == '/') root_real_.pop_back();

  listen_fd_ = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) throw std::runtime_error("socket failed");
  int one = 1;
  setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(opts_.port));
  if (inet_pton(AF_INET, opts_.host.c_str(), &addr.sin_addr) != 1) throw std::runtime_error("bad host " + opts_.host);
  if (bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0)
    throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
  if (listen(listen_fd_, 4096) != 0) throw std::runtime_error("listen failed");
  SetNonBlocking(listen_fd_);
  socklen_t len = sizeof addr;
  getsockname(listen_fd_, reinterpret_cast<sockaddr*>(&addr), &len);
  port_ = ntohs(addr.sin_port);

  int n = opts_.threads > 0 ? opts_.threads : static_cast<int>(std::max(2u, std::thread::hardware_concurrency() / 2));
  for (int i = 0; i < n; ++i) {
    workers_.push_back(std::make_unique<Worker>(opts_, root_real_, listen_fd_, stopping_, stop_deadline_ns_));
  }
  for (auto& w : workers_) threads_.emplace_back([&w] { w->Run(); });
}

void Server::Stop(int grace_ms) {
  if (threads_.empty()) return;
  // Workers stop accepting, close idle connections, and exit once queued responses are
  // flushed or the grace period ends.
  stop_deadline_ns_.store((Clock::now() + std::chrono::milliseconds(grace_ms)).time_since_epoch().count());
  stopping_.store(true);
  for (auto& t : threads_) t.join();
  threads_.clear();
  workers_.clear();
  if (listen_fd_ >= 0) close(listen_fd_);
  listen_fd_ = -1;
}

ServerStats Server::stats() const {
  ServerStats s;
  for (const auto& w : workers_) {
    s.accepted += w->accepted.load();
    s.requests += w->requests.load();
    s.bytes_sent += w->bytes_sent.load();
    s.timeouts += w->timeouts.load();
    s.open_connections += w->open_conns.load();
  }
  return s;
}

}  // namespace hearth
