// The byte-level helpers MCP and OAuth stand on: base64, SHA-256, the CSPRNG,
// and UTF-8 repair. All checked against published vectors rather than against
// themselves, since a round trip through a wrong implementation passes.

#include <set>
#include <string>

#include <gtest/gtest.h>

#include <core/util/base64.h>
#include <core/util/secure_random.h>
#include <core/util/sha256.h>
#include <core/util/text.h>

namespace util {
namespace {

// ─────────────────────────────── base64 ─────────────────────────────────────

// RFC 4648 section 10.
TEST(Base64Test, EncodesTheRfcVectors) {
  EXPECT_EQ("", base64_encode(""));
  EXPECT_EQ("Zg==", base64_encode("f"));
  EXPECT_EQ("Zm8=", base64_encode("fo"));
  EXPECT_EQ("Zm9v", base64_encode("foo"));
  EXPECT_EQ("Zm9vYg==", base64_encode("foob"));
  EXPECT_EQ("Zm9vYmE=", base64_encode("fooba"));
  EXPECT_EQ("Zm9vYmFy", base64_encode("foobar"));
}

TEST(Base64Test, UrlAlphabetWithoutPadding) {
  const std::string bytes("\xfb\xff\xbf", 3);
  EXPECT_EQ("+/+/", base64_encode(bytes));
  EXPECT_EQ("-_-_", base64_encode(bytes, /*url_safe=*/true));
  EXPECT_EQ("Zm8", base64_encode("fo", /*url_safe=*/true, /*pad=*/false));
}

TEST(Base64Test, DecodesBothAlphabetsPaddedOrNot) {
  EXPECT_EQ("foobar", base64_decode("Zm9vYmFy").value_or("?"));
  EXPECT_EQ("fo", base64_decode("Zm8=").value_or("?"));
  EXPECT_EQ("fo", base64_decode("Zm8").value_or("?"));
  EXPECT_EQ(std::string("\xfb\xff\xbf", 3), base64_decode("-_-_").value_or("?"));
  EXPECT_EQ(std::string("\xfb\xff\xbf", 3), base64_decode("+/+/").value_or("?"));
  // Line-wrapped payloads decode.
  EXPECT_EQ("foobar", base64_decode("Zm9v\nYmFy\n").value_or("?"));
}

TEST(Base64Test, RejectsWhatIsNotBase64) {
  EXPECT_FALSE(base64_decode("Zm9v!").has_value());
  EXPECT_FALSE(base64_decode("Zg==Zg").has_value());
  EXPECT_FALSE(base64_decode("Z").has_value());
}

// ─────────────────────────────── SHA-256 ────────────────────────────────────

// FIPS 180-4 / NIST CSRC example vectors.
TEST(Sha256Test, MatchesTheNistVectors) {
  EXPECT_EQ("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            sha256_hex(""));
  EXPECT_EQ("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
            sha256_hex("abc"));
  EXPECT_EQ("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1",
            sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"));
  EXPECT_EQ("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0",
            sha256_hex(std::string(1000000, 'a')));
}

// Feeding the same bytes in awkward pieces must not change the digest — the
// buffering across block boundaries is where a hand-written hash goes wrong.
TEST(Sha256Test, IncrementalUpdatesAgreeWithOneShot) {
  const std::string text(1000, 'x');
  Sha256 hash;
  for (size_t at = 0; at < text.size(); at += 37) {
    hash.update(std::string_view(text).substr(at, 37));
  }
  const auto digest = hash.finish();
  EXPECT_EQ(sha256_bytes(text),
            std::string(reinterpret_cast<const char*>(digest.data()), 32));
}

// RFC 7636 appendix B: the PKCE S256 challenge is base64url(SHA-256(verifier)).
TEST(Sha256Test, ProducesThePkceExampleChallenge) {
  const std::string verifier = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
  EXPECT_EQ("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM",
            base64_encode(sha256_bytes(verifier), /*url_safe=*/true,
                          /*pad=*/false));
}

// ──────────────────────────── secure random ─────────────────────────────────

TEST(SecureRandomTest, ReturnsTheRequestedLengthAndDiffersEachCall) {
  ASSERT_EQ(600u, secure_random_bytes(600).value_or("").size());  // > 256
  std::set<std::string> seen;
  for (int i = 0; i < 8; ++i) seen.insert(secure_random_token(32));
  EXPECT_EQ(8u, seen.size());
  // 32 bytes -> 43 unpadded base64url characters, the RFC 7636 minimum.
  EXPECT_EQ(43u, secure_random_token(32).size());
}

// ──────────────────────────────── UTF-8 ─────────────────────────────────────

TEST(Utf8Test, ValidTextIsReturnedUnchanged) {
  const std::string text = "plain, caf\xc3\xa9, \xe4\xb8\x96\xe7\x95\x8c, \xf0\x9f\x98\x80";
  EXPECT_TRUE(is_valid_utf8(text));
  EXPECT_EQ(text, sanitize_utf8(text));
}

TEST(Utf8Test, IllFormedBytesBecomeReplacementCharacters) {
  const std::string replacement = "\xef\xbf\xbd";
  // A stray continuation byte, a truncated sequence, an overlong slash, a
  // UTF-16 surrogate, and a byte that never appears in UTF-8.
  EXPECT_EQ("a" + replacement + "b", sanitize_utf8("a\x80" "b"));
  EXPECT_EQ("a" + replacement, sanitize_utf8("a\xc3"));
  EXPECT_EQ(replacement + replacement, sanitize_utf8("\xc0\xaf"));
  EXPECT_EQ(replacement + replacement + replacement,
            sanitize_utf8("\xed\xa0\x80"));
  EXPECT_EQ(replacement, sanitize_utf8("\xff"));
  EXPECT_TRUE(is_valid_utf8(sanitize_utf8("x\xfe\xfe\x80y")));
}

}  // namespace
}  // namespace util
