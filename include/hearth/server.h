// hearth: an event-loop HTTP/1.1 static file server.
#ifndef HEARTH_SERVER_H
#define HEARTH_SERVER_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hearth {

struct Options {
  std::string root = ".";  // directory to serve
  std::string host = "127.0.0.1";
  int port = 8080;                // 0 = pick a free port (see Server::port())
  int threads = 0;                // 0 = hardware_concurrency / 2
  int header_timeout_ms = 10000;  // time to receive a complete request head (slowloris)
  int idle_timeout_ms = 60000;    // keep-alive idle timeout
  std::size_t max_body_bytes = 1 << 20;
  int max_connections = 100000;
};

struct ServerStats {
  std::uint64_t accepted = 0;
  std::uint64_t requests = 0;
  std::uint64_t bytes_sent = 0;
  std::uint64_t timeouts = 0;
  std::uint64_t open_connections = 0;
};

class Server {
 public:
  explicit Server(Options opts);
  ~Server();
  Server(const Server&) = delete;
  Server& operator=(const Server&) = delete;

  // Bind, listen, and start the worker threads. Throws std::runtime_error on failure.
  void Start();
  // Stop accepting, finish queued responses (up to `grace_ms`), close everything, join workers.
  void Stop(int grace_ms = 5000);

  int port() const { return port_; }
  ServerStats stats() const;

  struct Worker;

 private:
  Options opts_;
  int listen_fd_ = -1;
  int port_ = 0;
  std::string root_real_;
  std::vector<std::unique_ptr<Worker>> workers_;
  std::vector<std::thread> threads_;
  std::atomic<bool> stopping_{false};
  std::atomic<long long> stop_deadline_ns_{0};  // steady_clock deadline for draining
};

}  // namespace hearth

#endif  // HEARTH_SERVER_H
