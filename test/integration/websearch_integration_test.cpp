// Live end-to-end test for the `websearch` tool against the real Parallel
// Search API (https://api.parallel.ai/v1/search). No Ollama, no Python — just
// WebSearchTool::execute() making a real HTTP call.
//
// Skipped unless a Parallel API key is available: the PARALLEL_API_KEY
// environment variable, or the repo's own .m8trix/parallel_api_key file (found
// via M8_SOURCE_DIR, since ctest's working directory is the build tree). A
// missing key is a SKIP; with a key present, a network or API failure is a
// real failure.
//
// Run with:  PARALLEL_API_KEY=... make integration-test
//        or: (with .m8trix/parallel_api_key in place)  make integration-test

#include <cstdlib>
#include <string>

#include <gtest/gtest.h>

#include <core/tools.h>

#include <parallel_key.h>

namespace agent {
namespace {

TEST(WebSearchIntegrationTest, RealParallelSearchReturnsResultsWithUrls) {
  if (not test::parallel_key_available()) {
    GTEST_SKIP() << "no Parallel API key — set PARALLEL_API_KEY or add "
                    ".m8trix/parallel_api_key to run this test";
  }

  ToolArgs args;
  args["query"] = std::string("Parallel Web Systems company");
  args["limit"] = static_cast<int64_t>(3);

  const ToolResult result = WebSearchTool().execute(args);

  ASSERT_TRUE(result.ok) << result.error;
  EXPECT_NE(result.output.find("http"), std::string::npos) << result.output;
  EXPECT_NE(result.output.find("result"), std::string::npos) << result.output;
}

// The error path against the live API: a rejected key comes back as an HTTP
// error carrying Parallel's own message. Gated the same way — it needs the
// network, and only a live-test environment has that set up.
TEST(WebSearchIntegrationTest, AnInvalidKeyIsRejectedByParallelWithItsMessage) {
  if (not test::parallel_key_available()) {
    GTEST_SKIP() << "no Parallel API key — set PARALLEL_API_KEY or add "
                    ".m8trix/parallel_api_key to run this test";
  }

  const char* prior = std::getenv("PARALLEL_API_KEY");
  const bool had_prior = prior != nullptr;
  const std::string saved = had_prior ? prior : "";
  ::setenv("PARALLEL_API_KEY", "definitely-not-a-real-key", 1);

  ToolArgs args;
  args["query"] = std::string("anything at all");
  const ToolResult result = WebSearchTool().execute(args);

  if (had_prior) {
    ::setenv("PARALLEL_API_KEY", saved.c_str(), 1);
  } else {
    ::unsetenv("PARALLEL_API_KEY");
  }

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("websearch: Parallel API returned HTTP"),
            std::string::npos)
      << result.error;
}

}  // namespace
}  // namespace agent
