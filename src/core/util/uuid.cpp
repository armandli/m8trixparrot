#include <core/util/uuid.h>

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>

namespace util {

std::string generate_uuid_v4() {
  static thread_local std::mt19937_64 engine(std::random_device{}());
  std::uniform_int_distribution<uint64_t> dist;

  uint64_t hi = dist(engine);
  uint64_t lo = dist(engine);

  hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

  char buf[37];
  std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%04x-%012llx",
                static_cast<unsigned>(hi >> 32),
                static_cast<unsigned>((hi >> 16) & 0xFFFFu),
                static_cast<unsigned>(hi & 0xFFFFu),
                static_cast<unsigned>(lo >> 48),
                static_cast<unsigned long long>(lo & 0xFFFFFFFFFFFFULL));
  return std::string(buf);
}

}  // namespace util
