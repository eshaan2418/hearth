// libFuzzer target for the request parser and path sanitiser (build: make fuzz).
#include <cstdint>
#include <cstdlib>
#include <string_view>

#include "hearth/http.h"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  const std::string_view in(reinterpret_cast<const char*>(data), size);
  hearth::Request r;
  int status = 0;
  const auto res = hearth::ParseRequest(in, &r, &status);
  if (res == hearth::Parse::kComplete) {
    if (r.head_bytes > in.size() || r.path.empty()) std::abort();
    if (auto p = hearth::SanitizePath(r.path)) {
      if (p->empty() || (*p)[0] != '/' || p->find("/../") != std::string::npos || p->find("//") != std::string::npos)
        std::abort();
    }
  } else if (res == hearth::Parse::kError && (status < 400 || status > 505)) {
    std::abort();
  }
  return 0;
}
