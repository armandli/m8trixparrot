// MemoryStore is the agentic layer: what gets remembered comes back ranked by
// a blend of similarity, recency and importance; recall bumps access counts;
// and every failure at the embedding boundary is reported rather than thrown.

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>
#include <simdjson.h>

#include <core/memory_store.h>
#include <core/tools.h>
#include <core/session_store.h>

#include "loopback_server.h"

namespace agent {
namespace {

inline constexpr uint32_t kDim = 64;

// Deterministic and offline. Every test that is not about the embedding
// boundary itself uses this, so the suite never needs a model or a network.
Embedder stub() { return hash_embedder(kDim); }

struct MemoryStoreTest : ::testing::Test {
  std::filesystem::path dir;
  std::string path;

  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("m8trix-memory-" + generate_uuid_v4());
    std::filesystem::create_directories(dir);
    path = (dir / "memory.m8db").string();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  MemoryOptions options() const {
    MemoryOptions out;
    out.path = path;
    out.embedder = stub();
    out.embed_model = "stub";
    out.recency_weight = 0.0;  // Explicit per test; the default is 0.3.
    return out;
  }

  MemoryOpenResult open_store(MemoryOptions opts) const {
    return MemoryStore::open(opts);
  }

  static Memory make(std::string content, std::string type = kMemoryEpisodic,
                     double importance = 0.5,
                     std::vector<std::string> tags = {},
                     std::string context = "default") {
    Memory memory;
    memory.content = std::move(content);
    memory.memory_type = std::move(type);
    memory.importance = importance;
    memory.tags = std::move(tags);
    memory.context_id = std::move(context);
    return memory;
  }
};

TEST_F(MemoryStoreTest, RememberReturnsAnIdAndRecallFindsTheContent) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const RememberResult stored =
      opened.store->remember(make("The Louvre is in Paris"));
  ASSERT_TRUE(stored.ok) << stored.error;
  EXPECT_GT(stored.id, 0u);

  RecallQuery query;
  query.query = "Louvre Paris";
  query.k = 3;
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_FALSE(recalled.memories.empty());
  EXPECT_EQ(recalled.memories[0].memory.content, "The Louvre is in Paris");
  EXPECT_EQ(recalled.memories[0].memory.id, stored.id);
}

TEST_F(MemoryStoreTest, TheFileIsCreatedLazilyOnTheFirstRemember) {
  // An embedding model does not advertise its width, so a brand-new store
  // cannot write a header until something has been embedded.
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  EXPECT_FALSE(std::filesystem::exists(path));
  ASSERT_TRUE(opened.store->remember(make("something")).ok);
  EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(MemoryStoreTest, RecallOnAnUntouchedStoreIsAnEmptyOkResult) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  RecallQuery query;
  query.query = "anything";
  const RecallResult recalled = opened.store->recall(query);
  EXPECT_TRUE(recalled.ok) << recalled.error;
  EXPECT_TRUE(recalled.memories.empty());
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(MemoryStoreTest, RecallRanksTheClosestMemoryFirst) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("impressionist art museums")).ok);
  ASSERT_TRUE(opened.store->remember(make("diesel engine maintenance")).ok);
  ASSERT_TRUE(opened.store->remember(make("sourdough bread starter")).ok);

  RecallQuery query;
  query.query = "impressionist art museums";
  query.k = 1;
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.content, "impressionist art museums");
}

TEST_F(MemoryStoreTest, MemoriesSurviveCloseAndReopen) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const RememberResult stored = opened.store->remember(
      make("user prefers impressionist art", kMemorySemantic, 0.9,
           {"preference", "art"}));
  ASSERT_TRUE(stored.ok) << stored.error;
  opened.store.reset();

  MemoryOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  RecallQuery query;
  query.query = "impressionist art preference";
  const RecallResult recalled = reopened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_FALSE(recalled.memories.empty());
  const Memory& got = recalled.memories[0].memory;
  EXPECT_EQ(got.content, "user prefers impressionist art");
  EXPECT_EQ(got.memory_type, kMemorySemantic);
  EXPECT_DOUBLE_EQ(got.importance, 0.9);
  EXPECT_EQ(got.tags, (std::vector<std::string>{"preference", "art"}));
}

TEST_F(MemoryStoreTest, RecallHonorsTheMemoryTypeFilter) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("paris trip talk", kMemoryEpisodic)).ok);
  ASSERT_TRUE(opened.store->remember(make("paris is in france", kMemorySemantic)).ok);

  RecallQuery query;
  query.query = "paris";
  query.k = 5;
  query.memory_type = kMemorySemantic;
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.memory_type, kMemorySemantic);
}

TEST_F(MemoryStoreTest, RecallHonorsTheContextIdFilter) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(
      make("shared topic", kMemoryEpisodic, 0.5, {}, "conv_001")).ok);
  ASSERT_TRUE(opened.store->remember(
      make("shared topic", kMemoryEpisodic, 0.5, {}, "conv_002")).ok);

  RecallQuery query;
  query.query = "shared topic";
  query.k = 5;
  query.context_id = "conv_002";
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.context_id, "conv_002");
}

TEST_F(MemoryStoreTest, RecallHonorsMinImportance) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("trivial note", kMemoryEpisodic, 0.2)).ok);
  ASSERT_TRUE(opened.store->remember(make("trivial note", kMemoryEpisodic, 0.9)).ok);

  RecallQuery query;
  query.query = "trivial note";
  query.k = 5;
  query.min_importance = 0.5;
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_DOUBLE_EQ(recalled.memories[0].memory.importance, 0.9);
}

TEST_F(MemoryStoreTest, RecallHonorsATagFilter) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(
      make("a note", kMemoryEpisodic, 0.5, {"travel", "paris"})).ok);
  ASSERT_TRUE(opened.store->remember(
      make("a note", kMemoryEpisodic, 0.5, {"cooking"})).ok);

  RecallQuery query;
  query.query = "a note";
  query.k = 5;
  query.tags = {"paris"};
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.tags,
            (std::vector<std::string>{"travel", "paris"}));
}

TEST_F(MemoryStoreTest, StructuredFieldsAndRawFilterJsonAreAnded) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("x", kMemorySemantic, 0.9)).ok);
  ASSERT_TRUE(opened.store->remember(make("x", kMemorySemantic, 0.1)).ok);
  ASSERT_TRUE(opened.store->remember(make("x", kMemoryEpisodic, 0.9)).ok);

  RecallQuery query;
  query.query = "x";
  query.k = 5;
  query.memory_type = kMemorySemantic;
  query.filters = R"({"importance":{"$gte":0.5}})";
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.memory_type, kMemorySemantic);
  EXPECT_DOUBLE_EQ(recalled.memories[0].memory.importance, 0.9);
}

TEST_F(MemoryStoreTest, AMalformedRawFilterIsAnError) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("x")).ok);

  RecallQuery query;
  query.query = "x";
  query.filters = "{not json";
  const RecallResult recalled = opened.store->recall(query);
  EXPECT_FALSE(recalled.ok);
  EXPECT_NE(recalled.error.find("filter parse error"), std::string::npos)
      << recalled.error;
}

TEST_F(MemoryStoreTest, ARecallWithNoQueryIsAnError) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const RecallResult recalled = opened.store->recall(RecallQuery{});
  EXPECT_FALSE(recalled.ok);
  EXPECT_FALSE(recalled.error.empty());
}

TEST_F(MemoryStoreTest, RecencyWeightPromotesTheNewerOfTwoEqualMemories) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;

  // Identical text, so similarity is identical and only the age separates
  // them. Timestamps are supplied rather than stamped, to make the age exact —
  // and taken from the real clock, because the decay is relative to now and a
  // hardcoded epoch would put both memories infinitely far in the past.
  const double now = std::chrono::duration<double>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  Memory old_memory = make("the same words exactly");
  old_memory.timestamp = now - 72.0 * 3600.0;
  Memory new_memory = make("the same words exactly");
  new_memory.timestamp = now;
  const RememberResult stored_old = opened.store->remember(old_memory);
  const RememberResult stored_new = opened.store->remember(new_memory);
  ASSERT_TRUE(stored_old.ok) << stored_old.error;
  ASSERT_TRUE(stored_new.ok) << stored_new.error;

  RecallQuery query;
  query.query = "the same words exactly";
  query.k = 2;
  query.recency_weight = 0.5;
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 2u);
  EXPECT_EQ(recalled.memories[0].memory.id, stored_new.id);
  EXPECT_GT(recalled.memories[0].recency, recalled.memories[1].recency);
}

TEST_F(MemoryStoreTest, TheRecencyFactorDecaysByEAtTheHalfLife) {
  MemoryOptions opts = options();
  opts.recency_half_life_hours = 24.0;  // The python's decay.
  MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;

  Memory memory = make("aged");
  memory.timestamp = 0.0;  // 0 means "stamp it now", so this is age zero.
  ASSERT_TRUE(opened.store->remember(memory).ok);

  RecallQuery query;
  query.query = "aged";
  query.recency_weight = 1.0;
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_NEAR(recalled.memories[0].recency, 1.0, 1e-3);
  EXPECT_NEAR(recalled.memories[0].score, 1.0, 1e-3);
}

TEST_F(MemoryStoreTest, ZeroRecencyWeightRanksPurelyBySimilarity) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  Memory ancient = make("impressionist art museums in paris");
  ancient.timestamp = 1.0;  // Effectively infinitely old.
  const RememberResult stored_old = opened.store->remember(ancient);
  ASSERT_TRUE(stored_old.ok) << stored_old.error;
  ASSERT_TRUE(opened.store->remember(make("diesel engine maintenance")).ok);

  RecallQuery query;
  query.query = "impressionist art museums in paris";
  query.k = 1;
  query.recency_weight = 0.0;
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.id, stored_old.id);
  EXPECT_NEAR(recalled.memories[0].score, recalled.memories[0].similarity, 1e-6);
}

TEST_F(MemoryStoreTest, ImportanceWeightPromotesTheMoreImportantMemory) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const RememberResult dull =
      opened.store->remember(make("equally worded memory", kMemoryEpisodic, 0.1));
  const RememberResult vital =
      opened.store->remember(make("equally worded memory", kMemoryEpisodic, 1.0));
  ASSERT_TRUE(dull.ok) << dull.error;
  ASSERT_TRUE(vital.ok) << vital.error;

  RecallQuery query;
  query.query = "equally worded memory";
  query.k = 2;
  query.recency_weight = 0.0;
  query.importance_weight = 0.5;
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 2u);
  EXPECT_EQ(recalled.memories[0].memory.id, vital.id);
}

TEST_F(MemoryStoreTest, RecallBumpsAccessCountAndTheBumpSurvivesReopen) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("counted memory")).ok);

  RecallQuery query;
  query.query = "counted memory";
  query.k = 1;
  const RecallResult first = opened.store->recall(query);
  ASSERT_TRUE(first.ok) << first.error;
  ASSERT_EQ(first.memories.size(), 1u);
  EXPECT_EQ(first.memories[0].memory.access_count, 1);

  const RecallResult second = opened.store->recall(query);
  ASSERT_TRUE(second.ok) << second.error;
  EXPECT_EQ(second.memories[0].memory.access_count, 2);
  opened.store.reset();

  MemoryOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  const RecallResult third = reopened.store->recall(query);
  ASSERT_TRUE(third.ok) << third.error;
  EXPECT_EQ(third.memories[0].memory.access_count, 3);
}

TEST_F(MemoryStoreTest, ForgetRemovesTheMemoryFromRecall) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const RememberResult stored = opened.store->remember(make("forget me"));
  ASSERT_TRUE(stored.ok) << stored.error;
  ASSERT_TRUE(opened.store->remember(make("keep me")).ok);

  ASSERT_TRUE(opened.store->forget({stored.id}).ok);
  RecallQuery query;
  query.query = "forget me";
  query.k = 5;
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  for (const ScoredMemory& memory : recalled.memories) {
    EXPECT_NE(memory.memory.id, stored.id);
  }
  EXPECT_EQ(opened.store->stats().total, 1u);
}

TEST_F(MemoryStoreTest, ForgettingAnUnknownIdIsHarmless) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  EXPECT_TRUE(opened.store->forget({999999}).ok);
  ASSERT_TRUE(opened.store->remember(make("x")).ok);
  EXPECT_TRUE(opened.store->forget({999999}).ok);
  EXPECT_EQ(opened.store->stats().total, 1u);
}

TEST_F(MemoryStoreTest, StatsCountMemoriesByType) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("a", kMemoryEpisodic)).ok);
  ASSERT_TRUE(opened.store->remember(make("b", kMemoryEpisodic)).ok);
  ASSERT_TRUE(opened.store->remember(make("c", kMemorySemantic)).ok);
  ASSERT_TRUE(opened.store->remember(make("d", kMemoryProcedural)).ok);

  const MemoryStats stats = opened.store->stats();
  EXPECT_EQ(stats.total, 4u);
  EXPECT_EQ(stats.episodic, 2u);
  EXPECT_EQ(stats.semantic, 1u);
  EXPECT_EQ(stats.procedural, 1u);
  EXPECT_EQ(stats.dim, kDim);
  EXPECT_GT(stats.file_bytes, 0u);
}

// ───────────────────────── the embedding boundary ──────────────────────────

TEST_F(MemoryStoreTest, AnEmbedderFailureIsReportedNotThrown) {
  MemoryOptions opts = options();
  opts.embedder = [](std::string_view, std::string& error) {
    error = "the model is on fire";
    return std::vector<float>();
  };
  MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  const RememberResult stored = opened.store->remember(make("x"));
  EXPECT_FALSE(stored.ok);
  EXPECT_NE(stored.error.find("the model is on fire"), std::string::npos)
      << stored.error;
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(MemoryStoreTest, AnEmbedderChangingItsDimensionIsAnError) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("first")).ok);
  opened.store.reset();

  MemoryOptions wider = options();
  wider.embedder = hash_embedder(kDim * 2);
  MemoryOpenResult reopened = open_store(wider);
  ASSERT_TRUE(reopened.ok) << reopened.error;
  const RememberResult stored = reopened.store->remember(make("second"));
  EXPECT_FALSE(stored.ok);
  EXPECT_NE(stored.error.find("dimensions"), std::string::npos) << stored.error;
}

TEST_F(MemoryStoreTest, AConfiguredDimensionThatContradictsTheFileIsAnError) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("first")).ok);
  opened.store.reset();

  MemoryOptions wrong = options();
  wrong.embedding_dim = kDim * 2;
  const MemoryOpenResult reopened = open_store(wrong);
  EXPECT_FALSE(reopened.ok);
  EXPECT_NE(reopened.error.find("dimensional"), std::string::npos)
      << reopened.error;
}

TEST_F(MemoryStoreTest, AZeroVectorDoesNotProduceNaNs) {
  MemoryOptions opts = options();
  opts.embedder = [](std::string_view, std::string&) {
    return std::vector<float>(kDim, 0.0f);
  };
  MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("degenerate")).ok);

  RecallQuery query;
  query.query = "degenerate";
  query.k = 1;
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_FALSE(std::isnan(recalled.memories[0].similarity));
  EXPECT_FALSE(std::isnan(recalled.memories[0].score));
}

TEST_F(MemoryStoreTest, OverlongContentIsRejected) {
  MemoryOptions opts = options();
  opts.max_content_bytes = 32;
  MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  const RememberResult stored = opened.store->remember(make(std::string(64, 'x')));
  EXPECT_FALSE(stored.ok);
  EXPECT_NE(stored.error.find("limit"), std::string::npos) << stored.error;
}

TEST_F(MemoryStoreTest, EmptyContentIsRejected) {
  MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  EXPECT_FALSE(opened.store->remember(make("")).ok);
}

TEST_F(MemoryStoreTest, OllamaEmbedderNarrowsDoublesAndKeepsTheirDirection) {
  const test::LoopbackServer server(
      200, "application/json",
      R"({"model":"stub","embeddings":[[3.0,4.0,0.0,0.0]]})");

  MemoryOptions opts;
  opts.path = path;
  opts.embed_model = "stub";
  opts.embedder = ollama_embedder("stub", server.url());
  opts.recency_weight = 0.0;
  MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;

  const RememberResult stored = opened.store->remember(make("anything"));
  ASSERT_TRUE(stored.ok) << stored.error;
  EXPECT_EQ(opened.store->stats().dim, 4u);

  // Stored normalized: (3,4,0,0) has length 5, so the query (3,4,0,0) must
  // come back at cosine similarity 1.
  RecallQuery query;
  query.query = "anything";
  query.k = 1;
  const RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_NEAR(recalled.memories[0].similarity, 1.0f, 1e-5);
}

TEST_F(MemoryStoreTest, OllamaEmbedderReportsAnHttpErrorAsAString) {
  const test::LoopbackServer server(500, "text/plain", "upstream is down");
  MemoryOptions opts;
  opts.path = path;
  opts.embedder = ollama_embedder("stub", server.url());
  MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  const RememberResult stored = opened.store->remember(make("x"));
  EXPECT_FALSE(stored.ok);
  EXPECT_NE(stored.error.find("embedding failed"), std::string::npos)
      << stored.error;
}

TEST_F(MemoryStoreTest, OllamaEmbedderReportsAnEmptyResponseAsAString) {
  const test::LoopbackServer server(200, "application/json",
                                    R"({"model":"stub","embeddings":[]})");
  MemoryOptions opts;
  opts.path = path;
  opts.embedder = ollama_embedder("stub", server.url());
  MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  const RememberResult stored = opened.store->remember(make("x"));
  EXPECT_FALSE(stored.ok);
  EXPECT_NE(stored.error.find("no embedding"), std::string::npos)
      << stored.error;
}

TEST_F(MemoryStoreTest, TheHashEmbedderIsDeterministicAndDirectional) {
  const Embedder embedder = hash_embedder(kDim);
  std::string error;
  const std::vector<float> first = embedder("paris art museums", error);
  const std::vector<float> again = embedder("paris art museums", error);
  const std::vector<float> other = embedder("diesel engine repair", error);
  ASSERT_EQ(first.size(), kDim);
  EXPECT_EQ(first, again);
  EXPECT_NE(first, other);
  // An empty text has no tokens and must not be a crash or a NaN factory.
  EXPECT_EQ(embedder("", error).size(), kDim);
}

// ──────────────────────────────── MemoryTool ───────────────────────────────

struct MemoryToolTest : MemoryStoreTest {
  void TearDown() override {
    // The registry is process-wide and caches by path, so a case that did not
    // clean up would hand its store to the next one.
    MemoryStoreRegistry::instance().reset();
    MemoryStoreTest::TearDown();
  }

  MemoryTool tool() const {
    MemoryOptions opts = options();
    opts.recency_weight = 0.0;
    return MemoryTool{opts};
  }

  static ToolArgs args_of(std::initializer_list<
                          std::pair<std::string, ToolArgValue>> entries) {
    ToolArgs args;
    for (const auto& [key, value] : entries) args.emplace(key, value);
    return args;
  }
};

TEST_F(MemoryToolTest, TheSchemaIsValidJsonAndNamesTheTool) {
  const std::string schema = MemoryTool::description();
  simdjson::dom::parser parser;
  simdjson::dom::element root;
  ASSERT_FALSE(parser.parse(simdjson::padded_string(schema)).get(root));
  std::string_view name;
  ASSERT_FALSE(root["name"].get_string().get(name));
  EXPECT_EQ(name, "memory");
  simdjson::dom::array required;
  ASSERT_FALSE(root["parameters"]["required"].get_array().get(required));
  ASSERT_EQ(required.size(), 1u);
}

TEST_F(MemoryToolTest, RemembersAndRecallsThroughTheToolInterface) {
  const MemoryTool memory = tool();
  const ToolResult stored = memory.execute(args_of({
      {"action", std::string("remember")},
      {"content", std::string("the user lives in Lisbon")},
      {"type", std::string("semantic")},
      {"importance", 0.9},
      {"tags", std::vector<std::string>{"user", "location"}},
  }));
  ASSERT_TRUE(stored.ok) << stored.error;
  EXPECT_NE(stored.output.find("Remembered as memory"), std::string::npos)
      << stored.output;

  const ToolResult recalled = memory.execute(args_of({
      {"action", std::string("recall")},
      {"query", std::string("where does the user live")},
      {"k", static_cast<int64_t>(3)},
  }));
  ASSERT_TRUE(recalled.ok) << recalled.error;
  EXPECT_NE(recalled.output.find("the user lives in Lisbon"), std::string::npos)
      << recalled.output;
  EXPECT_NE(recalled.output.find("semantic"), std::string::npos)
      << recalled.output;
}

TEST_F(MemoryToolTest, ATagArgumentReachesTheFilter) {
  const MemoryTool memory = tool();
  ASSERT_TRUE(memory
                  .execute(args_of({{"action", std::string("remember")},
                                    {"content", std::string("a tagged note")},
                                    {"tags", std::vector<std::string>{"keep"}}}))
                  .ok);
  ASSERT_TRUE(memory
                  .execute(args_of({{"action", std::string("remember")},
                                    {"content", std::string("a tagged note")},
                                    {"tags", std::vector<std::string>{"drop"}}}))
                  .ok);

  const ToolResult recalled = memory.execute(args_of({
      {"action", std::string("recall")},
      {"query", std::string("a tagged note")},
      {"tags", std::vector<std::string>{"keep"}},
  }));
  ASSERT_TRUE(recalled.ok) << recalled.error;
  EXPECT_NE(recalled.output.find("keep"), std::string::npos) << recalled.output;
  EXPECT_EQ(recalled.output.find("drop"), std::string::npos) << recalled.output;
}

TEST_F(MemoryToolTest, ForgetRemovesTheMemory) {
  const MemoryTool memory = tool();
  const ToolResult stored = memory.execute(
      args_of({{"action", std::string("remember")},
               {"content", std::string("temporary")}}));
  ASSERT_TRUE(stored.ok) << stored.error;
  const size_t open_bracket = stored.output.find_last_of(' ');
  const int64_t id = std::stoll(stored.output.substr(open_bracket + 1));

  const ToolResult forgotten = memory.execute(
      args_of({{"action", std::string("forget")}, {"id", id}}));
  ASSERT_TRUE(forgotten.ok) << forgotten.error;

  const ToolResult recalled = memory.execute(args_of({
      {"action", std::string("recall")},
      {"query", std::string("temporary")},
  }));
  ASSERT_TRUE(recalled.ok) << recalled.error;
  EXPECT_NE(recalled.output.find("No memories matched"), std::string::npos)
      << recalled.output;
}

TEST_F(MemoryToolTest, EveryArgumentErrorNamesTheToolAndTheArgument) {
  const MemoryTool memory = tool();
  const std::vector<std::pair<ToolArgs, std::string>> cases = {
      {args_of({}), "action"},
      {args_of({{"action", std::string("remember")}}), "content"},
      {args_of({{"action", std::string("recall")}}), "query"},
      {args_of({{"action", std::string("forget")}}), "id"},
      {args_of({{"action", std::string("levitate")}}), "unknown action"},
  };
  for (const auto& [args, expected] : cases) {
    const ToolResult result = memory.execute(args);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.rfind("memory: ", 0), 0u) << result.error;
    EXPECT_NE(result.error.find(expected), std::string::npos) << result.error;
  }
}

TEST_F(MemoryToolTest, AMalformedFiltersArgumentIsAnError) {
  const MemoryTool memory = tool();
  ASSERT_TRUE(memory
                  .execute(args_of({{"action", std::string("remember")},
                                    {"content", std::string("x")}}))
                  .ok);
  const ToolResult recalled = memory.execute(args_of({
      {"action", std::string("recall")},
      {"query", std::string("x")},
      {"filters", std::string("{broken")},
  }));
  EXPECT_FALSE(recalled.ok);
  EXPECT_NE(recalled.error.find("filter parse error"), std::string::npos)
      << recalled.error;
}

TEST_F(MemoryToolTest, RecallingBeforeAnythingIsRememberedSaysSo) {
  const ToolResult recalled = tool().execute(args_of({
      {"action", std::string("recall")},
      {"query", std::string("anything at all")},
  }));
  ASSERT_TRUE(recalled.ok) << recalled.error;
  EXPECT_NE(recalled.output.find("No memories matched"), std::string::npos)
      << recalled.output;
}

}  // namespace
}  // namespace agent
