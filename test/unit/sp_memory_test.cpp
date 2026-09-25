// sp's own logic: the slash command grammar its TUI accepts and the three
// memory operations the user can drive by hand. Everything here runs against
// a hash embedder, so the suite never needs an embedding model or a network.
// sp's system prompt is covered by sp_prompt_test.cpp.

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

#include <gtest/gtest.h>

#include <core/memory_store.h>
#include <core/util/uuid.h>

#include <sp_memory.h>

namespace sp {
namespace {

inline constexpr uint32_t kDim = 64;

bool has(const std::string& haystack, const std::string& needle) {
  return haystack.find(needle) != std::string::npos;
}

// ─────────────────────────────── the grammar ───────────────────────────────

TEST(SpCommandTest, RecognisesTheCommandsThatTakeNoArgument) {
  EXPECT_EQ(parse_command("/quit").kind, Command::Kind::Quit);
  EXPECT_EQ(parse_command("/help").kind, Command::Kind::Help);
  EXPECT_EQ(parse_command("/reset").kind, Command::Kind::Reset);
  EXPECT_EQ(parse_command("  /reset  ").kind, Command::Kind::Reset);
}

TEST(SpCommandTest, SplitsTheArgumentOffAndTrimsIt) {
  const Command remember = parse_command("/remember  I prefer rg  ");
  EXPECT_EQ(remember.kind, Command::Kind::Remember);
  EXPECT_EQ(remember.args, "I prefer rg");

  const Command search = parse_command("/memories git log");
  EXPECT_EQ(search.kind, Command::Kind::Memories);
  EXPECT_EQ(search.args, "git log");
}

// An empty argument is the command's problem, not the parser's: /memories with
// no query reports statistics, and /remember with no text prints its usage.
TEST(SpCommandTest, AllowsAnEmptyArgument) {
  EXPECT_EQ(parse_command("/memories").kind, Command::Kind::Memories);
  EXPECT_EQ(parse_command("/memories").args, "");
  EXPECT_EQ(parse_command("/remember").kind, Command::Kind::Remember);
  EXPECT_EQ(parse_command("/remember").args, "");
}

TEST(SpCommandTest, ForgetTakesADigitStringAndNothingElse) {
  const Command forget = parse_command("/forget 42");
  EXPECT_EQ(forget.kind, Command::Kind::Forget);
  EXPECT_EQ(forget.id, 42u);

  for (const char* bad : {"/forget", "/forget abc", "/forget 12abc",
                          "/forget -1", "/forget 0"}) {
    EXPECT_EQ(parse_command(bad).kind, Command::Kind::BadForget) << bad;
  }
}

// An unknown /word, and anything not starting with a slash, is a task for the
// agent — which is what sp did before these commands existed.
TEST(SpCommandTest, LeavesEverythingElseToTheAgent) {
  for (const char* task : {"", "list my files", "/rese", "/resetx",
                           "/rememberX", "remember to buy milk"}) {
    EXPECT_EQ(parse_command(task).kind, Command::Kind::None) << task;
  }
}

// ────────────────────────────── the operations ─────────────────────────────

struct SpMemoryTest : ::testing::Test {
  std::filesystem::path dir;
  std::unique_ptr<agent::MemoryStore> store;

  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("m8trix-sp-" + util::generate_uuid_v4());
    agent::MemoryOptions options;
    options.path = (dir / "memory.m8db").string();
    options.embedder = agent::hash_embedder(kDim);
    options.embed_model = "stub";
    agent::MemoryOpenResult opened = agent::MemoryStore::open(options);
    ASSERT_TRUE(opened.ok) << opened.error;
    store = std::move(opened.store);
  }

  void TearDown() override {
    store.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

TEST_F(SpMemoryTest, RememberingNothingStoresNothing) {
  EXPECT_TRUE(has(do_remember(*store, ""), "usage:"));
  EXPECT_EQ(store->stats().total, 0u);
}

TEST_F(SpMemoryTest, WhatAPersonTypesIsStoredAsAPreference) {
  const std::string said = do_remember(*store, "I prefer rg over grep");
  EXPECT_TRUE(has(said, "remembered as memory")) << said;

  const agent::MemoryStats stats = store->stats();
  EXPECT_EQ(stats.total, 1u);
  EXPECT_EQ(stats.semantic, 1u);  // Not episodic: it is a fact about the user.
  EXPECT_EQ(stats.episodic, 0u);
}

// The virgin-install path: nothing remembered yet, and asking must not fail or
// bring a database file into existence.
TEST_F(SpMemoryTest, ReportsStatisticsWhenAskedWithNoQuery) {
  const std::string said = do_search(*store, "");
  EXPECT_TRUE(has(said, "0 memories")) << said;
  EXPECT_TRUE(has(said, "nothing remembered yet")) << said;
  EXPECT_FALSE(std::filesystem::exists(dir / "memory.m8db"));
}

TEST_F(SpMemoryTest, SearchingFindsWhatWasRememberedAndShowsItsId) {
  do_remember(*store, "I prefer rg over grep");
  const std::string said = do_search(*store, "grep");
  EXPECT_TRUE(has(said, "[1]")) << said;
  EXPECT_TRUE(has(said, "semantic")) << said;
  EXPECT_TRUE(has(said, "I prefer rg over grep")) << said;
}

// Vector search has no similarity floor, so a store with anything in it always
// answers with its nearest memory however unrelated the query. "Nothing
// matched" therefore means the store is empty, and that is the case to cover.
TEST_F(SpMemoryTest, SaysSoWhenThereIsNothingToMatch) {
  EXPECT_TRUE(has(do_search(*store, "zzzz"), "no memories matched"));
  do_remember(*store, "I prefer rg over grep");
  EXPECT_TRUE(has(do_search(*store, "zzzz"), "[1]"));
}

// A memory's content is several lines by design; a listing is one row each.
TEST_F(SpMemoryTest, ListsOnlyTheFirstLineOfAMultiLineMemory) {
  do_remember(*store, "LESSON (tar): forgot -z\nDetect: not gzipped\nInstead: "
                      "tar -czf\nAvoid: check the extension");
  const std::string said = do_search(*store, "tar");
  EXPECT_TRUE(has(said, "LESSON (tar): forgot -z")) << said;
  EXPECT_FALSE(has(said, "Avoid: check the extension")) << said;
}

TEST_F(SpMemoryTest, ForgettingRemovesItFromLaterSearches) {
  do_remember(*store, "I prefer rg over grep");
  EXPECT_TRUE(has(do_forget(*store, 1), "forgot memory 1"));
  EXPECT_TRUE(has(do_search(*store, "grep"), "no memories matched"));
}

// Every failure comes back as text the TUI can show. Nothing throws.
TEST(SpMemoryEmbedderTest, ReportsAnEmbeddingFailureAsAnOrdinaryLine) {
  const std::filesystem::path dir =
      std::filesystem::temp_directory_path() /
      ("m8trix-sp-" + util::generate_uuid_v4());
  agent::MemoryOptions options;
  options.path = (dir / "memory.m8db").string();
  options.embed_model = "stub";
  options.embedder = [](std::string_view, std::string& error) {
    error = "no embedding model";
    return std::vector<float>();
  };
  agent::MemoryOpenResult opened = agent::MemoryStore::open(options);
  ASSERT_TRUE(opened.ok) << opened.error;

  EXPECT_TRUE(has(do_remember(*opened.store, "anything"), "no embedding model"));
  // A search cannot reach the embedder here: recall short-circuits to an empty
  // answer while no database file exists, precisely so that asking a question
  // never creates one.
  EXPECT_TRUE(has(do_search(*opened.store, "anything"), "no memories matched"));

  std::error_code ec;
  std::filesystem::remove_all(dir, ec);
}

}  // namespace
}  // namespace sp
