// URL parsing and encoding for MCP endpoints and, later, OAuth. Parsing goes
// through libcurl's own parser, so these pin what m8 believes about a URL —
// which is what the library that fetches it will believe too.

#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <core/util/open_url.h>
#include <core/util/url.h>

namespace util {
namespace {

TEST(UrlTest, TakesAUrlApart) {
  const auto url = parse_url("HTTPS://Example.COM:8443/mcp/v1?x=1&y=2#frag");
  ASSERT_TRUE(url.has_value());
  EXPECT_EQ(url->scheme, "https");
  EXPECT_EQ(url->host, "example.com");
  EXPECT_EQ(url->port, "8443");
  EXPECT_EQ(url->path, "/mcp/v1");
  EXPECT_EQ(url->query, "x=1&y=2");
  EXPECT_EQ(url->fragment, "frag");
  EXPECT_FALSE(url->has_userinfo);
}

TEST(UrlTest, FillsInTheRootPathAndNoticesUserinfo) {
  auto url = parse_url("http://localhost:3000");
  ASSERT_TRUE(url.has_value());
  EXPECT_EQ(url->path, "/");
  EXPECT_EQ(url->port, "3000");

  url = parse_url("https://user:secret@example.com/mcp");
  ASSERT_TRUE(url.has_value());
  EXPECT_TRUE(url->has_userinfo);
}

TEST(UrlTest, RefusesWhatIsNotAnAbsoluteUrl) {
  std::string error;
  EXPECT_FALSE(parse_url("example.com/mcp", &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(parse_url("", &error).has_value());
  EXPECT_FALSE(parse_url("http://", &error).has_value());
  EXPECT_FALSE(parse_url(std::string("http://a\0b.com/", 16), &error).has_value());
}

TEST(UrlTest, OriginsDropTheDefaultPort) {
  EXPECT_EQ(url_origin(*parse_url("https://example.com:443/a")), "https://example.com");
  EXPECT_EQ(url_origin(*parse_url("http://example.com:80/a")), "http://example.com");
  EXPECT_EQ(url_origin(*parse_url("https://example.com:8443/a")),
            "https://example.com:8443");
  EXPECT_EQ(url_origin(*parse_url("http://[::1]:9000/")), "http://[::1]:9000");
}

TEST(UrlTest, ResolvesReferencesAgainstABase) {
  EXPECT_EQ(resolve_url("https://a.example/mcp/v1", "/.well-known/x"),
            "https://a.example/.well-known/x");
  EXPECT_EQ(resolve_url("https://a.example/mcp/v1", "other"), "https://a.example/mcp/other");
  EXPECT_EQ(resolve_url("https://a.example/mcp", "https://b.example/y"), "https://b.example/y");
  EXPECT_FALSE(resolve_url("not a url", "/x").has_value());
}

TEST(UrlTest, KnowsTheLoopbackHosts) {
  EXPECT_TRUE(is_loopback_host("localhost"));
  EXPECT_TRUE(is_loopback_host("127.0.0.1"));
  EXPECT_TRUE(is_loopback_host("127.255.0.9"));
  EXPECT_TRUE(is_loopback_host("[::1]"));
  EXPECT_FALSE(is_loopback_host("127.0.0.1.evil.example"));
  EXPECT_FALSE(is_loopback_host("127.1"));
  EXPECT_FALSE(is_loopback_host("128.0.0.1"));
  EXPECT_FALSE(is_loopback_host("localhost.example"));
  EXPECT_FALSE(is_loopback_host("127.0.0.256"));
}

TEST(UrlTest, PercentEncodingLeavesOnlyUnreservedCharacters) {
  EXPECT_EQ(percent_encode("a-b_c.d~e"), "a-b_c.d~e");
  EXPECT_EQ(percent_encode("a b&c=d/é"), "a%20b%26c%3Dd%2F%C3%A9");
  EXPECT_EQ(percent_decode("a%20b%26c%3Dd%2F%C3%A9"), "a b&c=d/é");
  // A malformed escape is kept as written rather than guessed at.
  EXPECT_EQ(percent_decode("100%"), "100%");
  EXPECT_EQ(percent_decode("%zz%4"), "%zz%4");
  EXPECT_EQ(percent_decode("a+b"), "a+b");
  EXPECT_EQ(percent_decode("a+b", /*plus_as_space=*/true), "a b");
}

TEST(UrlTest, FormEncodingRoundTrips) {
  const std::vector<std::pair<std::string, std::string>> fields = {
      {"grant_type", "authorization_code"},
      {"redirect_uri", "http://127.0.0.1:5000/callback"},
      {"scope", "read write"},
      {"empty", ""}};
  const std::string encoded = form_encode(fields);
  EXPECT_EQ(encoded,
            "grant_type=authorization_code&redirect_uri=http%3A%2F%2F127.0.0.1%3A5000%"
            "2Fcallback&scope=read%20write&empty=");
  EXPECT_EQ(form_decode(encoded), fields);
  EXPECT_EQ(form_decode("code=abc+d&&state=x%2By&flag"),
            (std::vector<std::pair<std::string, std::string>>{
                {"code", "abc d"}, {"state", "x+y"}, {"flag", ""}}));
}

// A server chooses the link; only plain http(s) ever reaches the opener.
TEST(OpenUrlTest, OnlyPlainHttpLinksAreEverOpened) {
  EXPECT_EQ(open_url_problem("https://example.com/a?b=c"), "");
  EXPECT_EQ(open_url_problem("http://127.0.0.1:8080/"), "");
  EXPECT_NE(open_url_problem("javascript:alert(1)"), "");
  EXPECT_NE(open_url_problem("file:///etc/passwd"), "");
  EXPECT_NE(open_url_problem("https://example.com/\nX"), "");
  EXPECT_NE(open_url_problem("not a link"), "");
  std::string error;
  EXPECT_FALSE(open_url("file:///etc/passwd", error));
  EXPECT_FALSE(error.empty());
}

}  // namespace
}  // namespace util
