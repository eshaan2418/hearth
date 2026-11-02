// hearthd: serve a directory over HTTP/1.1.
//   hearthd [-root DIR] [-host 127.0.0.1] [-port 8080] [-threads N]
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "hearth/server.h"

static volatile std::sig_atomic_t g_stop = 0;

int main(int argc, char** argv) {
  hearth::Options o;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string k = argv[i], v = argv[i + 1];
    if (k == "-root")
      o.root = v;
    else if (k == "-host")
      o.host = v;
    else if (k == "-port")
      o.port = std::atoi(v.c_str());
    else if (k == "-threads")
      o.threads = std::atoi(v.c_str());
    else if (k == "-idle-timeout-ms")
      o.idle_timeout_ms = std::atoi(v.c_str());
    else {
      std::fprintf(stderr, "usage: hearthd [-root DIR] [-host H] [-port P] [-threads N] [-idle-timeout-ms MS]\n");
      return 2;
    }
  }
  std::signal(SIGPIPE, SIG_IGN);
  std::signal(SIGINT, [](int) { g_stop = 1; });
  std::signal(SIGTERM, [](int) { g_stop = 1; });
  hearth::Server s(o);
  try {
    s.Start();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "hearthd: %s\n", e.what());
    return 1;
  }
  std::fprintf(stderr, "hearthd: serving %s on http://%s:%d\n", o.root.c_str(), o.host.c_str(), s.port());
  while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(100));
  s.Stop(5000);
  const auto st = s.stats();
  std::fprintf(stderr, "hearthd: %llu connections, %llu requests, %llu bytes\n",
               static_cast<unsigned long long>(st.accepted), static_cast<unsigned long long>(st.requests),
               static_cast<unsigned long long>(st.bytes_sent));
}
