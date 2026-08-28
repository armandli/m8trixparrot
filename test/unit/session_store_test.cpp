// SessionStore round-trips an AgentResult tree: the objective/conclusion of the
// root plus every nested subagent result, one file, read back identically.

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>

#include <gtest/gtest.h>

#include <core/agent_result.h>
#include <core/session_store.h>

namespace agent {
namespace {

struct SessionStoreTest : ::testing::Test {
  std::filesystem::path dir;

  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("m8trix-session-" + generate_uuid_v4());
    std::filesystem::create_directories(dir);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }
};

void expect_tree_eq(const AgentResult& want, const AgentResult& got) {
  EXPECT_EQ(want.ok, got.ok);
  EXPECT_EQ(want.objective, got.objective);
  EXPECT_EQ(want.conclusion, got.conclusion);
  EXPECT_EQ(want.error, got.error);
  EXPECT_EQ(want.steps, got.steps);
  EXPECT_EQ(want.hit_step_limit, got.hit_step_limit);
  ASSERT_EQ(want.children.size(), got.children.size());
  for (std::size_t i = 0; i < want.children.size(); ++i) {
    expect_tree_eq(want.children[i], got.children[i]);
  }
}

AgentResult sample_tree() {
  AgentResult root;
  root.ok = true;
  root.objective = "build the thing";
  root.conclusion = "built it: see notes";
  root.steps = 4;

  AgentResult analyze;
  analyze.ok = true;
  analyze.objective = "analyze the parser";
  analyze.conclusion = "handles X, not Y";
  analyze.steps = 7;

  AgentResult fuzz;
  fuzz.ok = false;
  fuzz.objective = "run the fuzzer";
  fuzz.error = "gave up after 12 steps without a final answer";
  fuzz.steps = 12;
  fuzz.hit_step_limit = true;
  analyze.children.push_back(fuzz);

  AgentResult docs;
  docs.ok = true;
  docs.objective = "write the docs";
  docs.conclusion = "docs written";
  docs.steps = 2;

  root.children.push_back(analyze);
  root.children.push_back(docs);
  return root;
}

TEST_F(SessionStoreTest, RoundTripsANestedResult) {
  SessionStore store(dir.string());
  const AgentResult tree = sample_tree();

  const SessionStoreResult saved = store.store(tree, "");
  ASSERT_TRUE(saved.ok) << saved.error;
  ASSERT_FALSE(saved.session_id.empty());

  const SessionResult loaded = store.load(saved.session_id);
  ASSERT_TRUE(loaded.ok) << loaded.error;
  EXPECT_EQ(saved.session_id, loaded.session.session_id);
  expect_tree_eq(tree, loaded.session.result);
}

TEST_F(SessionStoreTest, LeafResultRoundTrips) {
  SessionStore store(dir.string());
  AgentResult leaf;
  leaf.ok = true;
  leaf.objective = "small task";
  leaf.conclusion = "done";
  leaf.steps = 1;

  const std::string id = store.store(leaf, "").session_id;
  const SessionResult loaded = store.load(id);
  ASSERT_TRUE(loaded.ok) << loaded.error;
  EXPECT_TRUE(loaded.session.result.children.empty());
  expect_tree_eq(leaf, loaded.session.result);
}

TEST_F(SessionStoreTest, LatestReturnsTheMostRecentlyWritten) {
  SessionStore store(dir.string());

  AgentResult first;
  first.ok = true;
  first.objective = "first";
  first.conclusion = "one";
  AgentResult second;
  second.ok = true;
  second.objective = "second";
  second.conclusion = "two";

  const std::string id1 = store.store(first, "").session_id;
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const std::string id2 = store.store(second, "").session_id;
  ASSERT_NE(id1, id2);

  const SessionResult latest = store.latest();
  ASSERT_TRUE(latest.ok) << latest.error;
  EXPECT_EQ(id2, latest.session.session_id);
  EXPECT_EQ("second", latest.session.result.objective);
}

TEST_F(SessionStoreTest, MissingFileIsAnError) {
  SessionStore store(dir.string());
  const SessionResult loaded = store.load("does-not-exist");
  EXPECT_FALSE(loaded.ok);
  EXPECT_FALSE(loaded.error.empty());
}

}  // namespace
}  // namespace agent
