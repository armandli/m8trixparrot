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
    // A rescan still in flight would write through index_path() *after* the
    // override is cleared — i.e. into the developer's real
    // ~/.m8trix/bash_search_index.json.
    wait_for_bash_search_rescan();
    std::error_code ec;
    std::filesystem::remove(index_file(), ec);
    std::filesystem::remove(index_file().string() + ".tmp", ec);
    set_bash_search_index_path("");
  }
};

// ── the cache and its background refresh ────────────────────────────────────

// The point of the whole design: with an index file present, answering a query
// must not pay for the ~1.4s `apropos` the scan costs. The rescan that follows
// runs on a worker thread, so it must not show up in the caller's time either.
TEST_F(BashSearchTest, ServingFromCacheDoesNotWaitForAScan) {
  wait_for_bash_search_rescan();
  ASSERT_TRUE(std::filesystem::exists(index_file()));

  const auto start = std::chrono::steady_clock::now();
  const ToolResult result =
      BashSearchTool().execute(args({{"action", str("list_tags")}}));
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
          .count();

  EXPECT_TRUE(result.ok);
  EXPECT_FALSE(result.output.empty());
  // A full scan is ~1.4s here; a cache hit is milliseconds. Slack enough for a
  // loaded machine while still failing if the scan crept back onto this path.
  EXPECT_LT(seconds, 0.5);
}

// The rescan has to actually finish and rewrite the file, or "refresh in the
// background" is just a slower way of never refreshing.
TEST_F(BashSearchTest, TheBackgroundRescanRewritesTheIndexFile) {
  wait_for_bash_search_rescan();
  ASSERT_TRUE(std::filesystem::exists(index_file()));

  // Make the file obviously old, then drop the loaded index so the next call
  // goes back through the cache-load path — which is the one that starts a
  // rescan. After an explicit scan the index is already fresh, so
  // ensure_loaded() rightly does nothing.
  std::filesystem::last_write_time(
      index_file(),
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
  const auto stale = std::filesystem::last_write_time(index_file());

  reset_bash_search_index_for_test();
  BashSearchTool().execute(args({{"action", str("list_tags")}}));
  wait_for_bash_search_rescan();

  EXPECT_GT(std::filesystem::last_write_time(index_file()), stale);
}

// The write goes through a temp file and a rename, so nothing is left behind
// and a reader never sees half a file.
TEST_F(BashSearchTest, WritingTheIndexLeavesNoTempFile) {
  wait_for_bash_search_rescan();
  EXPECT_FALSE(std::filesystem::exists(index_file().string() + ".tmp"));
}

// A truncated or corrupt cache used to mean an empty index *permanently*: the
// parse failed, but the index was still marked loaded, so every later query
// answered "no tags found" until someone ran a scan by hand.
TEST_F(BashSearchTest, ACorruptIndexFileIsRecoveredFrom) {
  wait_for_bash_search_rescan();

  // A fresh singleton is not reachable from a test, so this exercises the
  // recovery through the file: truncate it, then scan, and confirm what lands
  // on disk parses and holds entries again.
  {
    std::ofstream out(index_file(), std::ios::trunc);
    out << "{\"commands\": [{\"name\": \"tru";  // cut mid-value
  }
  ASSERT_TRUE(std::filesystem::file_size(index_file()) > 0);

  const ToolResult rebuilt =
      BashSearchTool().execute(args({{"action", str("scan")}}));
  wait_for_bash_search_rescan();

  EXPECT_TRUE(rebuilt.ok);
  EXPECT_NE(rebuilt.output.find("Scanned"), std::string::npos);

  const ToolResult tags =
      BashSearchTool().execute(args({{"action", str("list_tags")}}));
  EXPECT_TRUE(tags.ok);
  EXPECT_FALSE(tags.output.empty());
  EXPECT_EQ(tags.output.find("No tags found"), std::string::npos)
      << "a corrupt cache left the index permanently empty";
}

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
