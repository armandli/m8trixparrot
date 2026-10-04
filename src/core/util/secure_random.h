#ifndef M8_UTIL_SECURE_RANDOM_H
#define M8_UTIL_SECURE_RANDOM_H

#include <cstddef>
#include <optional>
#include <string>

namespace util {

// `count` bytes from the operating system's CSPRNG (getentropy, falling back to
// /dev/urandom). nullopt only if both are unavailable, which a caller treats as
// fatal for whatever needed the randomness — never as a reason to fall back to
// a predictable generator. generate_uuid_v4() is mt19937 and must not be used
// for OAuth `state` or a PKCE verifier.
std::optional<std::string> secure_random_bytes(size_t count);

// base64url (unpadded) of `count` random bytes: the shape PKCE verifiers and
// OAuth `state` values take. Empty string on failure.
std::string secure_random_token(size_t count = 32);

}  // namespace util

#endif  // M8_UTIL_SECURE_RANDOM_H
