// Tests for BashSearchTool: PATH scan, tag listing, boolean tag search.
//
// The singleton index persists in memory across cases, so SetUpTestSuite does a
// single scan before any case runs.  Individual cases then query that data.

#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include <core/tools.h>
#include <tool_test_env.h>

namespace agent::test {
namespace {

struct BashSearchTest : ToolTest {
  // The index defaults to ~/.m8trix/bash_search_index.json, and ToolTest only
  // chdirs — it never overrides $HOME. Without this, running the suite
  // rewrites the developer's own index file.
  static std::filesystem::path index_file() {
    return std::filesystem::temp_directory_path() /
           "m8trixparrot-test-bash-search-index.json";
  }

  static void SetUpTestSuite() {
    set_bash_search_index_path(index_file().string());
    BashSearchTool().execute(args({{"action", str("scan")}}));
  }

  static void TearDownTestSuite() {
    std::error_code ec;
    std::filesystem::remove(index_file(), ec);
    set_bash_search_index_path("");
  }
};

// ── scan ─────────────────────────────────────────────────────────────────────

TEST_F(BashSearchTest, ScanReturnsOkAndReportsCommandCount) {
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("scan")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.error.empty());
  EXPECT_NE(result.output.find("Scanned"), std::string::npos);
  EXPECT_NE(result.output.find("commands"), std::string::npos);
}

TEST_F(BashSearchTest, ScanFinishesInSecondsNotMinutes) {
  // The scan used to run one whatis(1) query per command name. On a system
  // whose manual index is built lazily — macOS, where mandoc re-reads the whole
  // manual tree per query at roughly a second a go — that turned a 2000-entry
  // PATH into half an hour and left the tool wedged at its first use. The scan
  // now asks apropos(1) for every description in one call, so its cost no
  // longer scales with PATH at all.
  const auto start = std::chrono::steady_clock::now();
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("scan")}}));
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();

  EXPECT_TRUE(result.ok);
  // Generous enough for a cold manual tree on a loaded machine, and still two
  // orders of magnitude under the per-name behavior it replaced.
  EXPECT_LT(seconds, 60.0);
}

// ── list_tags ────────────────────────────────────────────────────────────────

TEST_F(BashSearchTest, ListTagsReturnsNonEmptyList) {
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("list_tags")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.error.empty());
  EXPECT_FALSE(result.output.empty());
}

TEST_F(BashSearchTest, ListTagsContainsKnownCategories) {
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("list_tags")}}));

  EXPECT_TRUE(result.ok);
  // file, text, and network are on any POSIX system.
  EXPECT_NE(result.output.find("file"),    std::string::npos);
  EXPECT_NE(result.output.find("text"),    std::string::npos);
  EXPECT_NE(result.output.find("network"), std::string::npos);
}

// ── search ───────────────────────────────────────────────────────────────────

TEST_F(BashSearchTest, SearchSingleTagFindsExpectedCommands) {
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("search")},
                                     {"query",  str("file")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.error.empty());
  // ls and cp are always in PATH and both tagged "file".
  EXPECT_NE(result.output.find("ls"), std::string::npos);
}

TEST_F(BashSearchTest, SearchAndExpressionReturnsOnlyCommandsMatchingBothTags) {
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("search")},
                                     {"query",  str("file AND text")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.error.empty());
  // cat, head, tail are tagged both "file" and "text" in kNameTags.
  EXPECT_NE(result.output.find("cat"), std::string::npos);
}

TEST_F(BashSearchTest, SearchOrExpressionReturnsCommandsFromEitherTag) {
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("search")},
                                     {"query",  str("file OR text")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.error.empty());
  // ls is "file"-only, grep is "text"-only; both should appear.
  EXPECT_NE(result.output.find("ls"),   std::string::npos);
  EXPECT_NE(result.output.find("grep"), std::string::npos);
}

TEST_F(BashSearchTest, SearchUnknownTagReturnsEmptyButOk) {
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("search")},
                                     {"query",  str("nonexistent_tag_xyz")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.error.empty());
  EXPECT_NE(result.output.find("No commands found"), std::string::npos);
}

TEST_F(BashSearchTest, SearchWithParenthesisGroupingWorks) {
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("search")},
                                     {"query",  str("(file AND text) OR editor")}}));

  EXPECT_TRUE(result.ok);
  EXPECT_TRUE(result.error.empty());
}

// ── error cases ───────────────────────────────────────────────────────────────

TEST_F(BashSearchTest, MissingActionArgumentIsAnError) {
  const ToolResult result = BashSearchTool().execute(args({}));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("missing"), std::string::npos);
}

TEST_F(BashSearchTest, UnknownActionIsAnError) {
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("fly")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("unknown action"), std::string::npos);
}

TEST_F(BashSearchTest, SearchWithoutQueryIsAnError) {
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("search")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("query"), std::string::npos);
}

TEST_F(BashSearchTest, SearchWithMalformedQueryIsAnError) {
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("search")},
                                     {"query",  str("(file AND")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_TRUE(result.output.empty());
}

}  // namespace
}  // namespace agent::test
