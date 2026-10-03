#include <core/util/secure_random.h>

#include <unistd.h>

#if defined(__APPLE__)
#include <sys/random.h>
#endif

#include <algorithm>
#include <fstream>

#include <core/util/base64.h>

namespace util {

std::optional<std::string> secure_random_bytes(size_t count) {
  std::string out(count, '\0');

  // getentropy() is capped at 256 bytes per call on both macOS and Linux.
  size_t filled = 0;
  bool ok = true;
  while (filled < count) {
    const size_t chunk = std::min<size_t>(256, count - filled);
    if (::getentropy(out.data() + filled, chunk) != 0) {
      ok = false;
      break;
    }
    filled += chunk;
  }
  if (ok) return out;

  std::ifstream urandom("/dev/urandom", std::ios::binary);
  if (not urandom) return std::nullopt;
  urandom.read(out.data(), static_cast<std::streamsize>(count));
  if (static_cast<size_t>(urandom.gcount()) != count) return std::nullopt;
  return out;
}

std::string secure_random_token(size_t count) {
  const std::optional<std::string> bytes = secure_random_bytes(count);
  if (not bytes) return std::string();
  return base64_encode(*bytes, /*url_safe=*/true, /*pad=*/false);
}

}  // namespace util
