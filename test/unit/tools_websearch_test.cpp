// What `websearch` sends, how it renders the Parallel Search API's response,
// and what it refuses.
//
// Every test here talks to a loopback HTTP server standing in for
// api.parallel.ai (PARALLEL_API_BASE points the tool at it), so the suite is
// offline and deterministic. The loopback server never parses the request, so
// these tests cover response handling, not the exact bytes we send.

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include <core/tools.h>
#include <loopback_server.h>
#include <tool_test_env.h>

namespace agent::test {
namespace {

// Sets an environment variable for the lifetime of the object and restores the
// previous state (value or absence) on destruction. gtest runs cases
// sequentially in one process, so this is safe.
struct ScopedEnv {
  ScopedEnv(const std::string& name, const std::string& value) : mName(name) {
    capture();
    ::setenv(mName.c_str(), value.c_str(), 1);
  }

  // The one-argument form clears the variable instead of setting it.
  explicit ScopedEnv(const std::string& name) : mName(name) {
    capture();
    ::unsetenv(mName.c_str());
  }

  ~ScopedEnv() {
    if (mHadPrior) {
      ::setenv(mName.c_str(), mPrior.c_str(), 1);
    } else {
      ::unsetenv(mName.c_str());
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 protected:
  void capture() {
    if (const char* prior = std::getenv(mName.c_str())) {
      mHadPrior = true;
      mPrior = prior;
    }
  }

  std::string mName;
  std::string mPrior;
  bool mHadPrior = false;
};

struct WebSearchTest : ToolTest {};

const char* kTwoResults = R"json({
  "search_id": "search_x",
  "results": [
    {"url": "https://example.com/alpha", "title": "Alpha Page",
     "publish_date": "2024-01-15",
     "excerpts": ["First snippet about alpha.", "Second snippet."]},
    {"url": "https://example.com/beta", "title": null, "publish_date": null,
     "excerpts": ["Beta snippet only."]}
  ],
  "session_id": "session_x"
})json";

const char* kThreeResults = R"json({
  "results": [
    {"url": "https://example.com/1", "title": "One", "excerpts": ["first"]},
    {"url": "https://example.com/2", "title": "Two", "excerpts": ["second"]},
    {"url": "https://example.com/3", "title": "Three", "excerpts": ["third"]}
  ]
})json";

TEST_F(WebSearchTest, FormatsResultsAsANumberedListWithTitlesUrlsAndExcerpts) {
  LoopbackServer server(200, "application/json", kTwoResults);
  ScopedEnv base("PARALLEL_API_BASE", server.url(""));
  ScopedEnv key("PARALLEL_API_KEY", "test-key");

  const ToolResult result =
      WebSearchTool().execute(args({{"query", str("cats")}}));

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_NE(result.output.find("2 results for \"cats\":"), std::string::npos);
  EXPECT_NE(result.output.find("1. Alpha Page"), std::string::npos);
  EXPECT_NE(result.output.find("   https://example.com/alpha  (2024-01-15)"),
            std::string::npos);
  EXPECT_NE(result.output.find("First snippet about alpha."), std::string::npos);
  EXPECT_NE(result.output.find("Second snippet."), std::string::npos);
  // A null title falls back to the URL as the entry heading.
  EXPECT_NE(result.output.find("2. https://example.com/beta"), std::string::npos);
  EXPECT_NE(result.output.find("Beta snippet only."), std::string::npos);
}

// A null publish_date is simply left off the URL line, not rendered as "(null)"
// or an empty "()".
TEST_F(WebSearchTest, PublishDateIsOmittedWhenTheFieldIsNull) {
  LoopbackServer server(200, "application/json", kTwoResults);
  ScopedEnv base("PARALLEL_API_BASE", server.url(""));
  ScopedEnv key("PARALLEL_API_KEY", "test-key");

  const ToolResult result =
      WebSearchTool().execute(args({{"query", str("cats")}}));

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.output.find("https://example.com/beta  ("), std::string::npos);
  EXPECT_EQ(result.output.find("()"), std::string::npos);
}

TEST_F(WebSearchTest, LimitCapsHowManyResultsAreFormatted) {
  LoopbackServer server(200, "application/json", kThreeResults);
  ScopedEnv base("PARALLEL_API_BASE", server.url(""));
  ScopedEnv key("PARALLEL_API_KEY", "test-key");

  const ToolResult result = WebSearchTool().execute(
      args({{"query", str("counting")}, {"limit", num(1)}}));

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_NE(result.output.find("1 result for \"counting\":"), std::string::npos);
  EXPECT_NE(result.output.find("1. One"), std::string::npos);
  EXPECT_EQ(result.output.find("2. Two"), std::string::npos);
}

TEST_F(WebSearchTest, AnEmptyResultSetIsReportedAsNoResultsRatherThanAnError) {
  LoopbackServer server(200, "application/json", R"json({"results": []})json");
  ScopedEnv base("PARALLEL_API_BASE", server.url(""));
  ScopedEnv key("PARALLEL_API_KEY", "test-key");

  const ToolResult result =
      WebSearchTool().execute(args({{"query", str("ghosts")}}));

  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_EQ(result.output, "[no results for \"ghosts\"]");
}

// A non-2xx response is an error, and Parallel's own message is pulled out of
// the body and appended so the caller can tell a bad key from a bad request.
TEST_F(WebSearchTest, ANonSuccessStatusIsAnErrorNamingTheStatusAndParallelsMessage) {
  LoopbackServer server(401, "application/json",
                        R"json({"code": 16, "message": "Invalid API key (C.1)"})json");
  ScopedEnv base("PARALLEL_API_BASE", server.url(""));
  ScopedEnv key("PARALLEL_API_KEY", "test-key");

  const ToolResult result =
      WebSearchTool().execute(args({{"query", str("anything")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error,
            "websearch: Parallel API returned HTTP 401: Invalid API key (C.1)");
  EXPECT_TRUE(result.output.empty());
}

TEST_F(WebSearchTest, AResponseBodyThatIsNotJsonIsAnError) {
  LoopbackServer server(200, "application/json", "not json at all");
  ScopedEnv base("PARALLEL_API_BASE", server.url(""));
  ScopedEnv key("PARALLEL_API_KEY", "test-key");

  const ToolResult result =
      WebSearchTool().execute(args({{"query", str("anything")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error.rfind("websearch: ", 0), 0u);
  EXPECT_NE(result.error.find("Parallel response"), std::string::npos);
}

TEST_F(WebSearchTest, MissingQueryIsAnError) {
  const ToolResult result = WebSearchTool().execute(args({}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "websearch: missing required string argument 'query'");
}

TEST_F(WebSearchTest, AMissingApiKeyIsAnErrorNamingTheEnvVarAndTheKeyFile) {
  ScopedEnv key("PARALLEL_API_KEY");  // ensure it is unset; the temp cwd has no key file

  const ToolResult result =
      WebSearchTool().execute(args({{"query", str("anything")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("PARALLEL_API_KEY"), std::string::npos);
  EXPECT_NE(result.error.find(".m8trix/parallel_api_key"), std::string::npos);
}

// With no env var, the key is read from .m8trix/parallel_api_key relative to the
// working directory. Reaching ok == true proves a key was found and sent.
TEST_F(WebSearchTest, TheKeyFileIsUsedWhenTheEnvVarIsAbsent) {
  LoopbackServer server(200, "application/json", kThreeResults);
  ScopedEnv base("PARALLEL_API_BASE", server.url(""));
  ScopedEnv key("PARALLEL_API_KEY");  // unset
  write_file(".m8trix/parallel_api_key", "  file-key-123\n");

  const ToolResult result =
      WebSearchTool().execute(args({{"query", str("counting")}}));

  EXPECT_TRUE(result.ok) << result.error;
  EXPECT_NE(result.output.find("1. One"), std::string::npos);
}

}  // namespace
}  // namespace agent::test
