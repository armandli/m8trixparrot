// URL parsing and encoding for MCP endpoints and, later, OAuth. Parsing goes
// through libcurl's own parser, so these pin what m8 believes about a URL —
// which is what the library that fetches it will believe too.

#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
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

// Puts PATH and the working directory back however the test ends.
struct RestorePathAndCwd {
  std::optional<std::string> path;
  std::filesystem::path cwd = std::filesystem::current_path();
  RestorePathAndCwd() {
    if (const char* value = std::getenv("PATH")) path = value;
  }
  ~RestorePathAndCwd() {
    std::error_code ec;
    std::filesystem::current_path(cwd, ec);
    if (path) {
      ::setenv("PATH", path->c_str(), 1);
    } else {
      ::unsetenv("PATH");
    }
  }
};

// posix_spawn searches nothing: a bare `open` is whatever file of that name
// sits in the current directory, which is the workspace. The opener has to come
// from PATH. Fakes stand in for both platforms' openers, and PATH holds only the
// temp directory, so no real browser is involved.
TEST(OpenUrlTest, OpensTheOpenerOnThePathNotOneInTheWorkspace) {
  const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                     ("m8-open-url-" + std::to_string(::getpid()));
  const std::filesystem::path bin = root / "bin";
  const std::filesystem::path workspace = root / "workspace";
  std::filesystem::remove_all(root);
  std::filesystem::create_directories(bin);
  std::filesystem::create_directories(workspace);
  const auto script = [](const std::filesystem::path& path, const std::string& body) {
    std::ofstream(path) << "#!/bin/sh\n" << body << "\n";
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
  };
  for (const char* name : {"open", "xdg-open"}) {
    script(bin / name, "printf '%s' \"$1\" > '" + (root / "opened").string() + "'");
    script(workspace / name, ": > '" + (root / "ran-from-workspace").string() + "'");
  }

  std::string error;
  bool opened = false;
  {
    RestorePathAndCwd restore;
    ::setenv("PATH", bin.c_str(), 1);
    std::filesystem::current_path(workspace);
    opened = open_url("https://example.com/x", error);
  }

  EXPECT_TRUE(opened) << error;
  std::ifstream in(root / "opened");
  const std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  EXPECT_EQ(got, "https://example.com/x");
  EXPECT_FALSE(std::filesystem::exists(root / "ran-from-workspace"));
  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace util
