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

#include <core/vdb/memory_store.h>
#include <core/oc/ollama_client.h>
#include <core/tools/tools.h>
#include <core/util/uuid.h>

#include "loopback_server.h"

namespace agent {
namespace {

inline constexpr uint32_t kDim = 64;

// Deterministic and offline. Every test that is not about the embedding
// boundary itself uses this, so the suite never needs a model or a network.
vdb::Embedder stub() { return vdb::hash_embedder(kDim); }

struct MemoryStoreTest : ::testing::Test {
  std::filesystem::path dir;
  std::string path;

  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("m8trix-memory-" + util::generate_uuid_v4());
    std::filesystem::create_directories(dir);
    path = (dir / "memory.m8db").string();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  vdb::MemoryOptions options() const {
    vdb::MemoryOptions out;
    out.path = path;
    out.embedder = stub();
    out.embed_model = "stub";
    out.recency_weight = 0.0;  // Explicit per test; the default is 0.3.
    return out;
  }

  vdb::MemoryOpenResult open_store(vdb::MemoryOptions opts) const {
    return vdb::MemoryStore::open(opts);
  }

  static vdb::Memory make(std::string content, std::string type = vdb::kMemoryEpisodic,
                     double importance = 0.5,
                     std::vector<std::string> tags = {},
                     std::string context = "default") {
    vdb::Memory memory;
    memory.content = std::move(content);
    memory.memory_type = std::move(type);
    memory.importance = importance;
    memory.tags = std::move(tags);
    memory.context_id = std::move(context);
    return memory;
  }
};

TEST_F(MemoryStoreTest, RememberReturnsAnIdAndRecallFindsTheContent) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::RememberResult stored =
      opened.store->remember(make("The Louvre is in Paris"));
  ASSERT_TRUE(stored.ok) << stored.error;
  EXPECT_GT(stored.id, 0u);

  vdb::RecallQuery query;
  query.query = "Louvre Paris";
  query.k = 3;
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_FALSE(recalled.memories.empty());
  EXPECT_EQ(recalled.memories[0].memory.content, "The Louvre is in Paris");
  EXPECT_EQ(recalled.memories[0].memory.id, stored.id);
}

TEST_F(MemoryStoreTest, TheFileIsCreatedLazilyOnTheFirstRemember) {
  // An embedding model does not advertise its width, so a brand-new store
  // cannot write a header until something has been embedded.
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  EXPECT_FALSE(std::filesystem::exists(path));
  ASSERT_TRUE(opened.store->remember(make("something")).ok);
  EXPECT_TRUE(std::filesystem::exists(path));
}

TEST_F(MemoryStoreTest, RecallOnAnUntouchedStoreIsAnEmptyOkResult) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  vdb::RecallQuery query;
  query.query = "anything";
  const vdb::RecallResult recalled = opened.store->recall(query);
  EXPECT_TRUE(recalled.ok) << recalled.error;
  EXPECT_TRUE(recalled.memories.empty());
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(MemoryStoreTest, RecallRanksTheClosestMemoryFirst) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("impressionist art museums")).ok);
  ASSERT_TRUE(opened.store->remember(make("diesel engine maintenance")).ok);
  ASSERT_TRUE(opened.store->remember(make("sourdough bread starter")).ok);

  vdb::RecallQuery query;
  query.query = "impressionist art museums";
  query.k = 1;
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.content, "impressionist art museums");
}

TEST_F(MemoryStoreTest, MemoriesSurviveCloseAndReopen) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::RememberResult stored = opened.store->remember(
      make("user prefers impressionist art", vdb::kMemorySemantic, 0.9,
           {"preference", "art"}));
  ASSERT_TRUE(stored.ok) << stored.error;
  opened.store.reset();

  vdb::MemoryOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  vdb::RecallQuery query;
  query.query = "impressionist art preference";
  const vdb::RecallResult recalled = reopened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_FALSE(recalled.memories.empty());
  const vdb::Memory& got = recalled.memories[0].memory;
  EXPECT_EQ(got.content, "user prefers impressionist art");
  EXPECT_EQ(got.memory_type, vdb::kMemorySemantic);
  EXPECT_DOUBLE_EQ(got.importance, 0.9);
  EXPECT_EQ(got.tags, (std::vector<std::string>{"preference", "art"}));
}

TEST_F(MemoryStoreTest, RecallHonorsTheMemoryTypeFilter) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("paris trip talk", vdb::kMemoryEpisodic)).ok);
  ASSERT_TRUE(opened.store->remember(make("paris is in france", vdb::kMemorySemantic)).ok);

  vdb::RecallQuery query;
  query.query = "paris";
  query.k = 5;
  query.memory_type = vdb::kMemorySemantic;
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.memory_type, vdb::kMemorySemantic);
}

TEST_F(MemoryStoreTest, RecallHonorsTheContextIdFilter) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(
      make("shared topic", vdb::kMemoryEpisodic, 0.5, {}, "conv_001")).ok);
  ASSERT_TRUE(opened.store->remember(
      make("shared topic", vdb::kMemoryEpisodic, 0.5, {}, "conv_002")).ok);

  vdb::RecallQuery query;
  query.query = "shared topic";
  query.k = 5;
  query.context_id = "conv_002";
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.context_id, "conv_002");
}

TEST_F(MemoryStoreTest, RecallHonorsMinImportance) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("trivial note", vdb::kMemoryEpisodic, 0.2)).ok);
  ASSERT_TRUE(opened.store->remember(make("trivial note", vdb::kMemoryEpisodic, 0.9)).ok);

  vdb::RecallQuery query;
  query.query = "trivial note";
  query.k = 5;
  query.min_importance = 0.5;
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_DOUBLE_EQ(recalled.memories[0].memory.importance, 0.9);
}

TEST_F(MemoryStoreTest, RecallHonorsATagFilter) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(
      make("a note", vdb::kMemoryEpisodic, 0.5, {"travel", "paris"})).ok);
  ASSERT_TRUE(opened.store->remember(
      make("a note", vdb::kMemoryEpisodic, 0.5, {"cooking"})).ok);

  vdb::RecallQuery query;
  query.query = "a note";
  query.k = 5;
  query.tags = {"paris"};
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.tags,
            (std::vector<std::string>{"travel", "paris"}));
}

TEST_F(MemoryStoreTest, StructuredFieldsAndRawFilterJsonAreAnded) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("x", vdb::kMemorySemantic, 0.9)).ok);
  ASSERT_TRUE(opened.store->remember(make("x", vdb::kMemorySemantic, 0.1)).ok);
  ASSERT_TRUE(opened.store->remember(make("x", vdb::kMemoryEpisodic, 0.9)).ok);

  vdb::RecallQuery query;
  query.query = "x";
  query.k = 5;
  query.memory_type = vdb::kMemorySemantic;
  query.filters = R"({"importance":{"$gte":0.5}})";
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.memory_type, vdb::kMemorySemantic);
  EXPECT_DOUBLE_EQ(recalled.memories[0].memory.importance, 0.9);
}

TEST_F(MemoryStoreTest, AMalformedRawFilterIsAnError) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("x")).ok);

  vdb::RecallQuery query;
  query.query = "x";
  query.filters = "{not json";
  const vdb::RecallResult recalled = opened.store->recall(query);
  EXPECT_FALSE(recalled.ok);
  EXPECT_NE(recalled.error.find("filter parse error"), std::string::npos)
      << recalled.error;
}

TEST_F(MemoryStoreTest, ARecallWithNoQueryIsAnError) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::RecallResult recalled = opened.store->recall(vdb::RecallQuery{});
  EXPECT_FALSE(recalled.ok);
  EXPECT_FALSE(recalled.error.empty());
}

TEST_F(MemoryStoreTest, RecencyWeightPromotesTheNewerOfTwoEqualMemories) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;

  // Identical text, so similarity is identical and only the age separates
  // them. Timestamps are supplied rather than stamped, to make the age exact —
  // and taken from the real clock, because the decay is relative to now and a
  // hardcoded epoch would put both memories infinitely far in the past.
  const double now = std::chrono::duration<double>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  vdb::Memory old_memory = make("the same words exactly");
  old_memory.timestamp = now - 72.0 * 3600.0;
  vdb::Memory new_memory = make("the same words exactly");
  new_memory.timestamp = now;
  const vdb::RememberResult stored_old = opened.store->remember(old_memory);
  const vdb::RememberResult stored_new = opened.store->remember(new_memory);
  ASSERT_TRUE(stored_old.ok) << stored_old.error;
  ASSERT_TRUE(stored_new.ok) << stored_new.error;

  vdb::RecallQuery query;
  query.query = "the same words exactly";
  query.k = 2;
  query.recency_weight = 0.5;
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 2u);
  EXPECT_EQ(recalled.memories[0].memory.id, stored_new.id);
  EXPECT_GT(recalled.memories[0].recency, recalled.memories[1].recency);
}

TEST_F(MemoryStoreTest, TheRecencyFactorDecaysByEAtTheHalfLife) {
  vdb::MemoryOptions opts = options();
  opts.recency_half_life_hours = 24.0;  // The python's decay.
  vdb::MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;

  vdb::Memory memory = make("aged");
  memory.timestamp = 0.0;  // 0 means "stamp it now", so this is age zero.
  ASSERT_TRUE(opened.store->remember(memory).ok);

  vdb::RecallQuery query;
  query.query = "aged";
  query.recency_weight = 1.0;
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_NEAR(recalled.memories[0].recency, 1.0, 1e-3);
  EXPECT_NEAR(recalled.memories[0].score, 1.0, 1e-3);
}

TEST_F(MemoryStoreTest, ZeroRecencyWeightRanksPurelyBySimilarity) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  vdb::Memory ancient = make("impressionist art museums in paris");
  ancient.timestamp = 1.0;  // Effectively infinitely old.
  const vdb::RememberResult stored_old = opened.store->remember(ancient);
  ASSERT_TRUE(stored_old.ok) << stored_old.error;
  ASSERT_TRUE(opened.store->remember(make("diesel engine maintenance")).ok);

  vdb::RecallQuery query;
  query.query = "impressionist art museums in paris";
  query.k = 1;
  query.recency_weight = 0.0;
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_EQ(recalled.memories[0].memory.id, stored_old.id);
  EXPECT_NEAR(recalled.memories[0].score, recalled.memories[0].similarity, 1e-6);
}

TEST_F(MemoryStoreTest, ImportanceWeightPromotesTheMoreImportantMemory) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::RememberResult dull =
      opened.store->remember(make("equally worded memory", vdb::kMemoryEpisodic, 0.1));
  const vdb::RememberResult vital =
      opened.store->remember(make("equally worded memory", vdb::kMemoryEpisodic, 1.0));
  ASSERT_TRUE(dull.ok) << dull.error;
  ASSERT_TRUE(vital.ok) << vital.error;

  vdb::RecallQuery query;
  query.query = "equally worded memory";
  query.k = 2;
  query.recency_weight = 0.0;
  query.importance_weight = 0.5;
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 2u);
  EXPECT_EQ(recalled.memories[0].memory.id, vital.id);
}

TEST_F(MemoryStoreTest, RecallBumpsAccessCountAndTheBumpSurvivesReopen) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("counted memory")).ok);

  vdb::RecallQuery query;
  query.query = "counted memory";
  query.k = 1;
  const vdb::RecallResult first = opened.store->recall(query);
  ASSERT_TRUE(first.ok) << first.error;
  ASSERT_EQ(first.memories.size(), 1u);
  EXPECT_EQ(first.memories[0].memory.access_count, 1);

  const vdb::RecallResult second = opened.store->recall(query);
  ASSERT_TRUE(second.ok) << second.error;
  EXPECT_EQ(second.memories[0].memory.access_count, 2);
  opened.store.reset();

  vdb::MemoryOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  const vdb::RecallResult third = reopened.store->recall(query);
  ASSERT_TRUE(third.ok) << third.error;
  EXPECT_EQ(third.memories[0].memory.access_count, 3);
}

TEST_F(MemoryStoreTest, ForgetRemovesTheMemoryFromRecall) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::RememberResult stored = opened.store->remember(make("forget me"));
  ASSERT_TRUE(stored.ok) << stored.error;
  ASSERT_TRUE(opened.store->remember(make("keep me")).ok);

  ASSERT_TRUE(opened.store->forget({stored.id}).ok);
  vdb::RecallQuery query;
  query.query = "forget me";
  query.k = 5;
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  for (const vdb::ScoredMemory& memory : recalled.memories) {
    EXPECT_NE(memory.memory.id, stored.id);
  }
  EXPECT_EQ(opened.store->stats().total, 1u);
}

TEST_F(MemoryStoreTest, ForgettingAnUnknownIdIsHarmless) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  EXPECT_TRUE(opened.store->forget({999999}).ok);
  ASSERT_TRUE(opened.store->remember(make("x")).ok);
  EXPECT_TRUE(opened.store->forget({999999}).ok);
  EXPECT_EQ(opened.store->stats().total, 1u);
}

TEST_F(MemoryStoreTest, StatsCountMemoriesByType) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("a", vdb::kMemoryEpisodic)).ok);
  ASSERT_TRUE(opened.store->remember(make("b", vdb::kMemoryEpisodic)).ok);
  ASSERT_TRUE(opened.store->remember(make("c", vdb::kMemorySemantic)).ok);
  ASSERT_TRUE(opened.store->remember(make("d", vdb::kMemoryProcedural)).ok);

  const vdb::MemoryStats stats = opened.store->stats();
  EXPECT_EQ(stats.total, 4u);
  EXPECT_EQ(stats.episodic, 2u);
  EXPECT_EQ(stats.semantic, 1u);
  EXPECT_EQ(stats.procedural, 1u);
  EXPECT_EQ(stats.dim, kDim);
  EXPECT_GT(stats.file_bytes, 0u);
}

// ───────────────────────── the embedding boundary ──────────────────────────

TEST_F(MemoryStoreTest, AnEmbedderFailureIsReportedNotThrown) {
  vdb::MemoryOptions opts = options();
  opts.embedder = [](std::string_view, std::string& error) {
    error = "the model is on fire";
    return std::vector<float>();
  };
  vdb::MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::RememberResult stored = opened.store->remember(make("x"));
  EXPECT_FALSE(stored.ok);
  EXPECT_NE(stored.error.find("the model is on fire"), std::string::npos)
      << stored.error;
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST_F(MemoryStoreTest, AnEmbedderChangingItsDimensionIsAnError) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("first")).ok);
  opened.store.reset();

  vdb::MemoryOptions wider = options();
  wider.embedder = vdb::hash_embedder(kDim * 2);
  vdb::MemoryOpenResult reopened = open_store(wider);
  ASSERT_TRUE(reopened.ok) << reopened.error;
  const vdb::RememberResult stored = reopened.store->remember(make("second"));
  EXPECT_FALSE(stored.ok);
  EXPECT_NE(stored.error.find("dimensions"), std::string::npos) << stored.error;
}

TEST_F(MemoryStoreTest, AConfiguredDimensionThatContradictsTheFileIsAnError) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("first")).ok);
  opened.store.reset();

  vdb::MemoryOptions wrong = options();
  wrong.embedding_dim = kDim * 2;
  const vdb::MemoryOpenResult reopened = open_store(wrong);
  EXPECT_FALSE(reopened.ok);
  EXPECT_NE(reopened.error.find("dimensional"), std::string::npos)
      << reopened.error;
}

TEST_F(MemoryStoreTest, AZeroVectorDoesNotProduceNaNs) {
  vdb::MemoryOptions opts = options();
  opts.embedder = [](std::string_view, std::string&) {
    return std::vector<float>(kDim, 0.0f);
  };
  vdb::MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(opened.store->remember(make("degenerate")).ok);

  vdb::RecallQuery query;
  query.query = "degenerate";
  query.k = 1;
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_FALSE(std::isnan(recalled.memories[0].similarity));
  EXPECT_FALSE(std::isnan(recalled.memories[0].score));
}

TEST_F(MemoryStoreTest, OverlongContentIsRejected) {
  vdb::MemoryOptions opts = options();
  opts.max_content_bytes = 32;
  vdb::MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::RememberResult stored = opened.store->remember(make(std::string(64, 'x')));
  EXPECT_FALSE(stored.ok);
  EXPECT_NE(stored.error.find("limit"), std::string::npos) << stored.error;
}

TEST_F(MemoryStoreTest, EmptyContentIsRejected) {
  vdb::MemoryOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  EXPECT_FALSE(opened.store->remember(make("")).ok);
}

// The case that matters is two models of the same width: nothing downstream
// fails, so without this check recall just quietly gets worse.
TEST_F(MemoryStoreTest, AChangeOfEmbeddingModelIsCaughtAtTheSameWidth) {
  {
    vdb::MemoryOpenResult opened = open_store(options());
    ASSERT_TRUE(opened.ok) << opened.error;
    ASSERT_TRUE(opened.store->remember(make("something")).ok);
    ASSERT_TRUE(opened.store->flush().ok);
  }

  vdb::MemoryOptions other = options();
  other.embed_model = "a-different-model";  // Same hash embedder, same width.

  const std::string mismatch = vdb::memory_model_mismatch(path, other.embed_model);
  EXPECT_NE(mismatch.find("stub"), std::string::npos) << mismatch;
  EXPECT_NE(mismatch.find("a-different-model"), std::string::npos) << mismatch;

  // And open() refuses rather than leaving it to a later tool call.
  vdb::MemoryOpenResult reopened = open_store(other);
  EXPECT_FALSE(reopened.ok);
  EXPECT_NE(reopened.error.find("a-different-model"), std::string::npos)
      << reopened.error;

  // The model it was built with still opens.
  EXPECT_TRUE(vdb::memory_model_mismatch(path, "stub").empty());
  EXPECT_TRUE(open_store(options()).ok);
}

TEST_F(MemoryStoreTest, AMissingFileHasNoModelToDisagreeWith) {
  EXPECT_TRUE(vdb::memory_model_mismatch(path, "stub").empty());
}

TEST_F(MemoryStoreTest, OllamaEmbedderNarrowsDoublesAndKeepsTheirDirection) {
  const test::LoopbackServer server(
      200, "application/json",
      R"({"model":"stub","embeddings":[[3.0,4.0,0.0,0.0]]})");

  vdb::MemoryOptions opts;
  opts.path = path;
  opts.embed_model = "stub";
  oc::OllamaClient::configure_embed("stub", server.url());
  opts.embedder = vdb::ollama_embedder("stub");
  opts.recency_weight = 0.0;
  vdb::MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;

  const vdb::RememberResult stored = opened.store->remember(make("anything"));
  ASSERT_TRUE(stored.ok) << stored.error;
  EXPECT_EQ(opened.store->stats().dim, 4u);

  // Stored normalized: (3,4,0,0) has length 5, so the query (3,4,0,0) must
  // come back at cosine similarity 1.
  vdb::RecallQuery query;
  query.query = "anything";
  query.k = 1;
  const vdb::RecallResult recalled = opened.store->recall(query);
  ASSERT_TRUE(recalled.ok) << recalled.error;
  ASSERT_EQ(recalled.memories.size(), 1u);
  EXPECT_NEAR(recalled.memories[0].similarity, 1.0f, 1e-5);
}

TEST_F(MemoryStoreTest, OllamaEmbedderReportsAnHttpErrorAsAString) {
  const test::LoopbackServer server(500, "text/plain", "upstream is down");
  vdb::MemoryOptions opts;
  opts.path = path;
  oc::OllamaClient::configure_embed("stub", server.url());
  opts.embedder = vdb::ollama_embedder("stub");
  vdb::MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::RememberResult stored = opened.store->remember(make("x"));
  EXPECT_FALSE(stored.ok);
  EXPECT_NE(stored.error.find("embedding failed"), std::string::npos)
      << stored.error;
}

TEST_F(MemoryStoreTest, OllamaEmbedderReportsAnEmptyResponseAsAString) {
  const test::LoopbackServer server(200, "application/json",
                                    R"({"model":"stub","embeddings":[]})");
  vdb::MemoryOptions opts;
  opts.path = path;
  oc::OllamaClient::configure_embed("stub", server.url());
  opts.embedder = vdb::ollama_embedder("stub");
  vdb::MemoryOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::RememberResult stored = opened.store->remember(make("x"));
  EXPECT_FALSE(stored.ok);
  EXPECT_NE(stored.error.find("no embedding"), std::string::npos)
      << stored.error;
}

TEST_F(MemoryStoreTest, TheHashEmbedderIsDeterministicAndDirectional) {
  const vdb::Embedder embedder = vdb::hash_embedder(kDim);
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
    vdb::MemoryStoreRegistry::instance().reset();
    MemoryStoreTest::TearDown();
  }

  vdb::MemoryTool tool() const {
    vdb::MemoryOptions opts = options();
    opts.recency_weight = 0.0;
    return vdb::MemoryTool{opts};
  }

  static tools::ToolArgs args_of(std::initializer_list<
                          std::pair<std::string, tools::ToolArgValue>> entries) {
    tools::ToolArgs args;
    for (const auto& [key, value] : entries) args.emplace(key, value);
    return args;
  }
};

TEST_F(MemoryToolTest, TheSchemaIsValidJsonAndNamesTheTool) {
  const std::string schema = vdb::MemoryTool::description();
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
  const vdb::MemoryTool memory = tool();
  const tools::ToolResult stored = memory.execute(args_of({
      {"action", std::string("remember")},
      {"content", std::string("the user lives in Lisbon")},
      {"type", std::string("semantic")},
      {"importance", 0.9},
      {"tags", std::vector<std::string>{"user", "location"}},
  }));
  ASSERT_TRUE(stored.ok) << stored.error;
  EXPECT_NE(stored.output.find("Remembered as memory"), std::string::npos)
      << stored.output;

  const tools::ToolResult recalled = memory.execute(args_of({
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
  const vdb::MemoryTool memory = tool();
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

  const tools::ToolResult recalled = memory.execute(args_of({
      {"action", std::string("recall")},
      {"query", std::string("a tagged note")},
      {"tags", std::vector<std::string>{"keep"}},
  }));
  ASSERT_TRUE(recalled.ok) << recalled.error;
  EXPECT_NE(recalled.output.find("keep"), std::string::npos) << recalled.output;
  EXPECT_EQ(recalled.output.find("drop"), std::string::npos) << recalled.output;
}

TEST_F(MemoryToolTest, ForgetRemovesTheMemory) {
  const vdb::MemoryTool memory = tool();
  const tools::ToolResult stored = memory.execute(
      args_of({{"action", std::string("remember")},
               {"content", std::string("temporary")}}));
  ASSERT_TRUE(stored.ok) << stored.error;
  const size_t open_bracket = stored.output.find_last_of(' ');
  const int64_t id = std::stoll(stored.output.substr(open_bracket + 1));

  const tools::ToolResult forgotten = memory.execute(
      args_of({{"action", std::string("forget")}, {"id", id}}));
  ASSERT_TRUE(forgotten.ok) << forgotten.error;

  const tools::ToolResult recalled = memory.execute(args_of({
      {"action", std::string("recall")},
      {"query", std::string("temporary")},
  }));
  ASSERT_TRUE(recalled.ok) << recalled.error;
  EXPECT_NE(recalled.output.find("No memories matched"), std::string::npos)
      << recalled.output;
}

TEST_F(MemoryToolTest, EveryArgumentErrorNamesTheToolAndTheArgument) {
  const vdb::MemoryTool memory = tool();
  const std::vector<std::pair<tools::ToolArgs, std::string>> cases = {
      {args_of({}), "action"},
      {args_of({{"action", std::string("remember")}}), "content"},
      {args_of({{"action", std::string("recall")}}), "query"},
      {args_of({{"action", std::string("forget")}}), "id"},
      {args_of({{"action", std::string("levitate")}}), "unknown action"},
  };
  for (const auto& [args, expected] : cases) {
    const tools::ToolResult result = memory.execute(args);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.error.rfind("memory: ", 0), 0u) << result.error;
    EXPECT_NE(result.error.find(expected), std::string::npos) << result.error;
  }
}

TEST_F(MemoryToolTest, AMalformedFiltersArgumentIsAnError) {
  const vdb::MemoryTool memory = tool();
  ASSERT_TRUE(memory
                  .execute(args_of({{"action", std::string("remember")},
                                    {"content", std::string("x")}}))
                  .ok);
  const tools::ToolResult recalled = memory.execute(args_of({
      {"action", std::string("recall")},
      {"query", std::string("x")},
      {"filters", std::string("{broken")},
  }));
  EXPECT_FALSE(recalled.ok);
  EXPECT_NE(recalled.error.find("filter parse error"), std::string::npos)
      << recalled.error;
}

TEST_F(MemoryToolTest, RecallingBeforeAnythingIsRememberedSaysSo) {
  const tools::ToolResult recalled = tool().execute(args_of({
      {"action", std::string("recall")},
      {"query", std::string("anything at all")},
  }));
  ASSERT_TRUE(recalled.ok) << recalled.error;
  EXPECT_NE(recalled.output.find("No memories matched"), std::string::npos)
      << recalled.output;
}

}  // namespace
}  // namespace agent
