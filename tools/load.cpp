// hearth-load: closed-loop HTTP/1.1 keep-alive load generator, one thread per connection.
//   hearth-load HOST PORT PATH CONNECTIONS SECONDS
// Prints one JSON line: requests/s, MB/s, latency percentiles, non-200 count.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using Clock = std::chrono::steady_clock;

int main(int argc, char** argv) {
  if (argc != 6) {
    std::fprintf(stderr, "usage: hearth-load HOST PORT PATH CONNECTIONS SECONDS\n");
    return 2;
  }
  const std::string host = argv[1], path = argv[3];
  const int port = std::atoi(argv[2]), conns = std::atoi(argv[4]);
  const double secs = std::atof(argv[5]);
  const std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + "\r\n\r\n";
  std::atomic<bool> stop{false};
  std::vector<std::vector<std::uint32_t>> lat(conns);
  std::vector<std::uint64_t> bytes(conns, 0), bad(conns, 0);
  std::vector<std::thread> ts;
  const auto warm_until = Clock::now() + std::chrono::milliseconds(500);
  for (int c = 0; c < conns; ++c) {
    ts.emplace_back([&, c] {
      const int fd = socket(AF_INET, SOCK_STREAM, 0);
      int one = 1;
      setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
      sockaddr_in a{};
      a.sin_family = AF_INET;
      a.sin_port = htons(static_cast<uint16_t>(port));
      inet_pton(AF_INET, host.c_str(), &a.sin_addr);
      if (connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        std::perror("connect");
        std::exit(1);
      }
      std::vector<char> buf(1 << 20);
      std::string acc;
      while (!stop.load(std::memory_order_relaxed)) {
        const auto t0 = Clock::now();
        if (write(fd, req.data(), req.size()) != static_cast<ssize_t>(req.size())) return;
        // Read one response: headers, then Content-Length bytes.
        acc.clear();
        std::size_t head_end = std::string::npos, need = 0;
        std::size_t body_got = 0;
        int status = 0;
        while (true) {
          const ssize_t n = read(fd, buf.data(), buf.size());
          if (n <= 0) return;
          if (head_end == std::string::npos) {
            acc.append(buf.data(), static_cast<std::size_t>(n));
            head_end = acc.find("\r\n\r\n");
            if (head_end == std::string::npos) continue;
            status = std::atoi(acc.c_str() + 9);
            const auto cl = acc.find("Content-Length: ");
            need = cl == std::string::npos ? 0 : std::strtoull(acc.c_str() + cl + 16, nullptr, 10);
            body_got = acc.size() - head_end - 4;
          } else {
            body_got += static_cast<std::size_t>(n);
          }
          if (body_got >= need) break;
        }
        if (t0 > warm_until) {
          lat[c].push_back(static_cast<std::uint32_t>(
              std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count()));
          bytes[c] += need;
          if (status != 200) ++bad[c];
        }
      }
      close(fd);
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(500) + std::chrono::duration<double>(secs));
  stop = true;
  for (auto& t : ts) t.join();
  std::vector<std::uint32_t> all;
  std::uint64_t total_bytes = 0, non200 = 0;
  for (int c = 0; c < conns; ++c) {
    all.insert(all.end(), lat[c].begin(), lat[c].end());
    total_bytes += bytes[c];
    non200 += bad[c];
  }
  std::sort(all.begin(), all.end());
  auto pct = [&](double q) { return all.empty() ? 0u : all[static_cast<std::size_t>((all.size() - 1) * q)]; };
  std::printf(
      "{\"connections\":%d,\"path\":\"%s\",\"requests\":%zu,\"rps\":%.0f,\"mb_per_s\":%.1f,\"p50_us\":%u,\"p99_us\":%u,"
      "\"p999_us\":%u,\"non200\":%llu}\n",
      conns, path.c_str(), all.size(), all.size() / secs, total_bytes / secs / 1e6, pct(0.5), pct(0.99), pct(0.999),
      static_cast<unsigned long long>(non200));
}
