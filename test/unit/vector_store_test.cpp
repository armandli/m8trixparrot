// VectorStore is the single-file half: documents, metadata and vectors survive
// a reopen, a crash-torn tail is discarded without taking the rest of the file
// with it, compaction reclaims tombstones, and every filter operator selects
// what it says it does.

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <random>
#include <string>
#include <thread>
#include <system_error>
#include <vector>

#include <gtest/gtest.h>

#include <core/util/uuid.h>
#include <core/vdb/vector_store.h>

namespace agent {
namespace {

inline constexpr uint32_t kDim = 16;

std::vector<float> seeded_vector(uint32_t seed, uint32_t dim = kDim) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> gauss(0.0f, 1.0f);
  std::vector<float> out(dim);
  for (float& x : out) x = gauss(rng);
  return out;
}

vdb::Schema memory_like_schema() {
  vdb::Schema schema;
  schema.add_field("kind", vdb::FieldType::String);
  schema.add_field("rank", vdb::FieldType::Int);
  schema.add_field("weight", vdb::FieldType::Float);
  schema.add_field("pinned", vdb::FieldType::Bool);
  schema.add_field("tags", vdb::FieldType::StringArray);
  return schema;
}

vdb::Metadata meta_for(int i) {
  vdb::Metadata meta;
  meta["kind"] = std::string(i % 2 == 0 ? "episodic" : "semantic");
  meta["rank"] = static_cast<int64_t>(i);
  meta["weight"] = 0.1 * static_cast<double>(i % 10);
  meta["pinned"] = (i % 5 == 0);
  meta["tags"] = std::vector<std::string>{i % 3 == 0 ? "paris" : "london"};
  return meta;
}

vdb::Filter parsed(std::string_view json) {
  std::string error;
  const std::optional<vdb::Filter> filter = vdb::Filter::parse(json, error);
  EXPECT_TRUE(filter.has_value()) << json << ": " << error;
  return filter ? *filter : vdb::Filter::match_all();
}

struct VectorStoreTest : ::testing::Test {
  std::filesystem::path dir;
  std::string path;

  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("m8trix-vecstore-" + util::generate_uuid_v4());
    std::filesystem::create_directories(dir);
    path = (dir / "memory.m8db").string();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  vdb::StoreOptions options() const {
    vdb::StoreOptions out;
    out.dim = kDim;
    // Off by default in the tests so the graph path is exercised on purpose
    // rather than by accident of collection size.
    out.brute_force_below = 0;
    return out;
  }

  vdb::StoreOpenResult open_store(vdb::StoreOptions opts) const {
    return vdb::VectorStore::open(path, memory_like_schema(), opts);
  }

  // Adds `count` documents whose vectors are seeded by index, so the same
  // index always produces the same vector and a query can name its own answer.
  static vdb::AddResult fill(vdb::VectorStore& store, int count, int from = 0) {
    std::vector<std::string> contents;
    std::vector<vdb::Metadata> metadatas;
    std::vector<std::vector<float>> vectors;
    for (int i = from; i < from + count; ++i) {
      contents.push_back("document " + std::to_string(i));
      metadatas.push_back(meta_for(i));
      vectors.push_back(seeded_vector(static_cast<uint32_t>(i)));
    }
    return store.add(contents, metadatas, vectors);
  }
};

TEST_F(VectorStoreTest, CreatesTheFileAndItsParentDirectory) {
  const std::string nested = (dir / "a" / "b" / "memory.m8db").string();
  vdb::StoreOptions opts = options();
  vdb::StoreOpenResult opened =
      vdb::VectorStore::open(nested, memory_like_schema(), opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  EXPECT_TRUE(std::filesystem::exists(nested));
  EXPECT_EQ(opened.store->doc_count(), 0u);
}

TEST_F(VectorStoreTest, OpeningWithoutADimensionIsAnError) {
  vdb::StoreOptions opts;
  opts.dim = 0;
  const vdb::StoreOpenResult opened = open_store(opts);
  EXPECT_FALSE(opened.ok);
  EXPECT_NE(opened.error.find("dim"), std::string::npos) << opened.error;
}

TEST_F(VectorStoreTest, AddFindsTheNearestDocumentFirst) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::AddResult added = fill(*opened.store, 50);
  ASSERT_TRUE(added.ok) << added.error;
  ASSERT_EQ(added.ids.size(), 50u);

  const vdb::SearchResult found = opened.store->search(seeded_vector(7), 3);
  ASSERT_TRUE(found.ok) << found.error;
  ASSERT_FALSE(found.hits.empty());
  EXPECT_EQ(found.hits[0].document.content, "document 7");
  EXPECT_NEAR(found.hits[0].score, 1.0f, 1e-4);
}

TEST_F(VectorStoreTest, RejectsAVectorOfTheWrongDimension) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::AddResult added =
      opened.store->add({"x"}, {meta_for(0)}, {std::vector<float>(3, 1.0f)});
  EXPECT_FALSE(added.ok);
  EXPECT_NE(added.error.find("dimensions"), std::string::npos) << added.error;
  EXPECT_EQ(opened.store->doc_count(), 0u);
}

TEST_F(VectorStoreTest, RejectsMetadataThatDisagreesWithTheSchema) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  vdb::Metadata bad;
  bad["rank"] = std::string("not an int");
  const vdb::AddResult added =
      opened.store->add({"x"}, {bad}, {seeded_vector(1)});
  EXPECT_FALSE(added.ok);
  EXPECT_NE(added.error.find("rank"), std::string::npos) << added.error;
}

TEST_F(VectorStoreTest, RejectsAnUnknownMetadataField) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  vdb::Metadata bad;
  bad["nope"] = std::string("x");
  const vdb::AddResult added = opened.store->add({"x"}, {bad}, {seeded_vector(1)});
  EXPECT_FALSE(added.ok);
  EXPECT_NE(added.error.find("nope"), std::string::npos) << added.error;
}

TEST_F(VectorStoreTest, RoundTripsDocumentsAcrossReopen) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::AddResult added = fill(*opened.store, 30);
  ASSERT_TRUE(added.ok) << added.error;
  opened.store.reset();

  vdb::StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 30u);

  const std::vector<vdb::Document> got = reopened.store->get({added.ids[11]});
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].content, "document 11");
  EXPECT_EQ(std::get<std::string>(got[0].metadata.at("kind")), "semantic");
  EXPECT_EQ(std::get<int64_t>(got[0].metadata.at("rank")), 11);
  EXPECT_EQ(std::get<std::vector<std::string>>(got[0].metadata.at("tags")),
            std::vector<std::string>{"london"});

  const vdb::SearchResult found = reopened.store->search(seeded_vector(11), 1);
  ASSERT_TRUE(found.ok) << found.error;
  ASSERT_EQ(found.hits.size(), 1u);
  EXPECT_EQ(found.hits[0].document.content, "document 11");
}

TEST_F(VectorStoreTest, ReplaysRecordsWrittenAfterTheLastSnapshot) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 10).ok);
  ASSERT_TRUE(opened.store->flush().ok);
  // These land after the snapshot and are only recoverable by replaying the
  // tail, which is the property this pins.
  ASSERT_TRUE(fill(*opened.store, 5, 100).ok);
  opened.store.reset();

  vdb::StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 15u);
  const vdb::SearchResult found = reopened.store->search(seeded_vector(103), 1);
  ASSERT_TRUE(found.ok) << found.error;
  ASSERT_EQ(found.hits.size(), 1u);
  EXPECT_EQ(found.hits[0].document.content, "document 103");
}

TEST_F(VectorStoreTest, DiscardsATornTailAndKeepsEverythingBeforeIt) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 20).ok);
  ASSERT_TRUE(opened.store->flush().ok);
  const uint64_t durable = opened.store->file_size();
  ASSERT_TRUE(fill(*opened.store, 1, 500).ok);
  opened.store.reset();

  // Chop five bytes into document 500's record — a crash between the write
  // starting and its checksum landing looks exactly like this. Cutting at a
  // known offset rather than a fixed distance from the end keeps the test
  // honest about which record it is destroying: the destructor appends a
  // snapshot on close, so "the last few bytes" would only hit that.
  std::error_code ec;
  std::filesystem::resize_file(path, durable + 5, ec);
  ASSERT_FALSE(ec);

  vdb::StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_GT(reopened.truncated_bytes, 0u);
  EXPECT_FALSE(reopened.warning.empty());
  EXPECT_EQ(reopened.store->doc_count(), 20u);
  const vdb::SearchResult found = reopened.store->search(seeded_vector(3), 1);
  ASSERT_TRUE(found.ok) << found.error;
  EXPECT_EQ(found.hits[0].document.content, "document 3");
}

TEST_F(VectorStoreTest, WritesAfterATornTailLandCleanly) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 8).ok);
  opened.store.reset();

  std::error_code ec;
  const uintmax_t before = std::filesystem::file_size(path, ec);
  std::filesystem::resize_file(path, before - 5, ec);
  ASSERT_FALSE(ec);

  vdb::StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  ASSERT_TRUE(fill(*reopened.store, 4, 900).ok);
  reopened.store.reset();

  vdb::StoreOpenResult again = open_store(options());
  ASSERT_TRUE(again.ok) << again.error;
  EXPECT_EQ(again.truncated_bytes, 0u);
  const vdb::SearchResult found = again.store->search(seeded_vector(902), 1);
  ASSERT_TRUE(found.ok) << found.error;
  EXPECT_EQ(found.hits[0].document.content, "document 902");
}

TEST_F(VectorStoreTest, SurvivesATornNewestHeaderPage) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 12).ok);
  opened.store.reset();

  // Scribble over whichever header page is newest. The duplicate is the whole
  // reason two of them exist.
  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.good());
  const std::string junk(4096, 'X');
  file.seekp(0);
  file.write(junk.data(), junk.size());
  file.seekp(4096);
  file.write(junk.data(), junk.size());
  file.close();

  // Both pages gone is unrecoverable and must say so rather than guess.
  vdb::StoreOpenResult both_gone = open_store(options());
  EXPECT_FALSE(both_gone.ok);
  EXPECT_NE(both_gone.error.find("corrupt"), std::string::npos)
      << both_gone.error;
}

TEST_F(VectorStoreTest, RecoversWhenOnlyOneHeaderPageIsDestroyed) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 12).ok);
  opened.store.reset();

  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.good());
  const std::string junk(4096, 'X');
  file.seekp(0);
  file.write(junk.data(), junk.size());
  file.close();

  vdb::StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 12u);
}

TEST_F(VectorStoreTest, RejectsADimensionMismatchOnOpen) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  opened.store.reset();

  vdb::StoreOptions wider = options();
  wider.dim = kDim * 2;
  const vdb::StoreOpenResult reopened = open_store(wider);
  EXPECT_FALSE(reopened.ok);
  EXPECT_NE(reopened.error.find("dimensional"), std::string::npos)
      << reopened.error;
}

TEST_F(VectorStoreTest, WarnsWhenTheEmbeddingSourceChanges) {
  vdb::StoreOptions first = options();
  first.source = "nomic-embed-text";
  vdb::StoreOpenResult opened = open_store(first);
  ASSERT_TRUE(opened.ok) << opened.error;
  opened.store.reset();

  vdb::StoreOptions second = options();
  second.source = "some-other-model";
  vdb::StoreOpenResult reopened = open_store(second);
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_NE(reopened.warning.find("nomic-embed-text"), std::string::npos)
      << reopened.warning;
  EXPECT_EQ(reopened.store->source(), "nomic-embed-text");
}

TEST_F(VectorStoreTest, OpeningSomethingThatIsNotAStoreIsAnError) {
  std::ofstream junk(path, std::ios::binary);
  junk << std::string(9000, 'q');
  junk.close();
  const vdb::StoreOpenResult opened = open_store(options());
  EXPECT_FALSE(opened.ok);
  EXPECT_FALSE(opened.error.empty());
}

TEST_F(VectorStoreTest, RemovedDocumentsLeaveSearchAndStayGoneAfterReopen) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::AddResult added = fill(*opened.store, 20);
  ASSERT_TRUE(added.ok) << added.error;

  ASSERT_TRUE(opened.store->remove({added.ids[4], added.ids[5]}).ok);
  EXPECT_EQ(opened.store->doc_count(), 18u);
  EXPECT_TRUE(opened.store->get({added.ids[4]}).empty());
  const vdb::SearchResult found = opened.store->search(seeded_vector(4), 1);
  ASSERT_TRUE(found.ok) << found.error;
  EXPECT_NE(found.hits[0].document.content, "document 4");
  opened.store.reset();

  vdb::StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 18u);
  EXPECT_TRUE(reopened.store->get({added.ids[4]}).empty());
}

TEST_F(VectorStoreTest, RemovingAnUnknownOrAlreadyRemovedIdIsHarmless) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::AddResult added = fill(*opened.store, 5);
  ASSERT_TRUE(opened.store->remove({added.ids[0]}).ok);
  ASSERT_TRUE(opened.store->remove({added.ids[0], 999999}).ok);
  EXPECT_EQ(opened.store->doc_count(), 4u);
}

TEST_F(VectorStoreTest, UpdateMetadataSurvivesReopenWithoutRewritingTheVector) {
  // A realistic embedding width, because that is the whole claim: a metadata
  // record must not carry the vector. At kDim the vector is 64 bytes and the
  // metadata JSON is larger than it, so the difference only shows at a width
  // anyone would actually embed at.
  constexpr uint32_t kWideDim = 256;
  vdb::StoreOptions wide = options();
  wide.dim = kWideDim;
  vdb::StoreOpenResult opened = open_store(wide);
  ASSERT_TRUE(opened.ok) << opened.error;

  const uint64_t empty = opened.store->file_size();
  const vdb::AddResult added = opened.store->add(
      {"document 2"}, {meta_for(2)}, {seeded_vector(2, kWideDim)});
  ASSERT_TRUE(added.ok) << added.error;
  const uint64_t put_doc_bytes = opened.store->file_size() - empty;

  const uint64_t before = opened.store->file_size();
  vdb::Metadata change;
  change["rank"] = static_cast<int64_t>(4242);
  ASSERT_TRUE(opened.store->update_metadata(added.ids[0], change).ok);
  const uint64_t set_meta_bytes = opened.store->file_size() - before;

  EXPECT_LT(set_meta_bytes, put_doc_bytes);
  EXPECT_LT(set_meta_bytes, kWideDim * sizeof(float));
  opened.store.reset();

  vdb::StoreOpenResult reopened = open_store(wide);
  ASSERT_TRUE(reopened.ok) << reopened.error;
  const std::vector<vdb::Document> got = reopened.store->get({added.ids[0]});
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(got[0].metadata.at("rank")), 4242);
  // The fields it did not name are untouched.
  EXPECT_EQ(std::get<std::string>(got[0].metadata.at("kind")), "episodic");
  // And the vector still ranks the document as its own nearest neighbour.
  const vdb::SearchResult found =
      reopened.store->search(seeded_vector(2, kWideDim), 1);
  ASSERT_TRUE(found.ok) << found.error;
  ASSERT_EQ(found.hits.size(), 1u);
  EXPECT_EQ(found.hits[0].document.content, "document 2");
}

TEST_F(VectorStoreTest, UpdatingAnUnknownIdIsAnError) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  vdb::Metadata change;
  change["rank"] = static_cast<int64_t>(1);
  EXPECT_FALSE(opened.store->update_metadata(4242, change).ok);
}

TEST_F(VectorStoreTest, CompactionDropsTombstonesAndShrinksTheFile) {
  vdb::StoreOptions opts = options();
  opts.compact_on_open = false;
  vdb::StoreOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::AddResult added = fill(*opened.store, 100);
  ASSERT_TRUE(added.ok) << added.error;

  std::vector<uint64_t> doomed;
  for (size_t i = 0; i < added.ids.size(); ++i) {
    if (i % 2 == 0) doomed.push_back(added.ids[i]);
  }
  ASSERT_TRUE(opened.store->remove(doomed).ok);
  ASSERT_TRUE(opened.store->flush().ok);

  const uint64_t before = opened.store->file_size();
  ASSERT_TRUE(opened.store->compact().ok);
  const uint64_t after = opened.store->file_size();
  EXPECT_LT(after, before);
  EXPECT_EQ(opened.store->garbage_bytes(), 0u);
  EXPECT_EQ(opened.store->doc_count(), 50u);

  // Survivors are still searchable, and by their own vectors.
  const vdb::SearchResult found = opened.store->search(seeded_vector(7), 1);
  ASSERT_TRUE(found.ok) << found.error;
  EXPECT_EQ(found.hits[0].document.content, "document 7");
  opened.store.reset();

  vdb::StoreOpenResult reopened = open_store(opts);
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 50u);
  EXPECT_TRUE(reopened.store->get({added.ids[0]}).empty());
  const vdb::SearchResult again = reopened.store->search(seeded_vector(9), 1);
  ASSERT_TRUE(again.ok) << again.error;
  EXPECT_EQ(again.hits[0].document.content, "document 9");
}

TEST_F(VectorStoreTest, CompactionLeavesNoTemporaryFileBehind) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 10).ok);
  ASSERT_TRUE(opened.store->compact().ok);
  EXPECT_FALSE(std::filesystem::exists(path + ".compact"));
}

TEST_F(VectorStoreTest, AnOrphanedCompactFileIsRemovedOnOpen) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 4).ok);
  opened.store.reset();

  std::ofstream orphan(path + ".compact", std::ios::binary);
  orphan << "debris from a crash mid-compaction";
  orphan.close();

  vdb::StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_FALSE(std::filesystem::exists(path + ".compact"));
  EXPECT_EQ(reopened.store->doc_count(), 4u);
}

TEST_F(VectorStoreTest, CompactionRunsOnOpenWhenGarbageOutweighsLiveBytes) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::AddResult added = fill(*opened.store, 60);
  ASSERT_TRUE(added.ok) << added.error;
  std::vector<uint64_t> doomed(added.ids.begin(), added.ids.begin() + 55);
  ASSERT_TRUE(opened.store->remove(doomed).ok);
  opened.store.reset();

  vdb::StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 5u);
  EXPECT_EQ(reopened.store->garbage_bytes(), 0u);
}

// ─────────────────────────────── the filter DSL ────────────────────────────

TEST_F(VectorStoreTest, EveryComparisonOperatorSelectsWhatItSays) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 30).ok);
  vdb::VectorStore& store = *opened.store;

  const auto count = [&](std::string_view json) {
    return store.get_where(parsed(json), 1000).size();
  };

  EXPECT_EQ(count(R"({"kind":{"$eq":"episodic"}})"), 15u);
  EXPECT_EQ(count(R"({"kind":"episodic"})"), 15u);  // bare value is $eq
  EXPECT_EQ(count(R"({"kind":{"$ne":"episodic"}})"), 15u);
  EXPECT_EQ(count(R"({"rank":{"$lt":5}})"), 5u);
  EXPECT_EQ(count(R"({"rank":{"$lte":5}})"), 6u);
  EXPECT_EQ(count(R"({"rank":{"$gt":25}})"), 4u);
  EXPECT_EQ(count(R"({"rank":{"$gte":25}})"), 5u);
  EXPECT_EQ(count(R"({"rank":{"$in":[1,2,3]}})"), 3u);
  EXPECT_EQ(count(R"({"rank":{"$nin":[1,2,3]}})"), 27u);
  EXPECT_EQ(count(R"({"tags":{"$contains":"paris"}})"), 10u);
  EXPECT_EQ(count(R"({"pinned":true})"), 6u);
  // An int literal must compare against a float field, since JSON writes 1 for
  // 1.0 and the document could have been stored either way.
  EXPECT_EQ(count(R"({"weight":{"$gte":0}})"), 30u);
}

TEST_F(VectorStoreTest, ConditionsCombineWithAndOrAndImplicitAnd) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 30).ok);
  vdb::VectorStore& store = *opened.store;

  const auto count = [&](std::string_view json) {
    return store.get_where(parsed(json), 1000).size();
  };

  EXPECT_EQ(count(R"({"kind":"episodic","rank":{"$lt":10}})"), 5u);
  EXPECT_EQ(count(R"({"$and":[{"kind":"episodic"},{"rank":{"$lt":10}}]})"), 5u);
  EXPECT_EQ(count(R"({"$or":[{"rank":{"$lt":3}},{"rank":{"$gte":28}}]})"), 5u);
  EXPECT_EQ(
      count(R"({"kind":"episodic","$or":[{"rank":0},{"rank":2}]})"), 2u);
  EXPECT_EQ(count("{}"), 30u);
  EXPECT_EQ(count(""), 30u);
}

TEST_F(VectorStoreTest, AMalformedFilterIsAnErrorNotACrash) {
  std::string error;
  EXPECT_FALSE(vdb::Filter::parse("{not json", error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(vdb::Filter::parse("[1,2,3]", error).has_value());
  EXPECT_FALSE(vdb::Filter::parse(R"({"rank":{"$wat":1}})", error).has_value());
  EXPECT_NE(error.find("$wat"), std::string::npos) << error;
  EXPECT_FALSE(vdb::Filter::parse(R"({"$nope":[]})", error).has_value());
  EXPECT_FALSE(vdb::Filter::parse(R"({"rank":{"$in":5}})", error).has_value());
}

TEST_F(VectorStoreTest, SearchWithASelectiveFilterFallsBackToAnExactScan) {
  vdb::StoreOptions opts = options();
  // Force the graph path so the fallback is what produces the answer.
  opts.brute_force_below = 0;
  opts.ef_search = 16;
  vdb::StoreOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 1000).ok);

  // One document in a thousand: a graph walk fills its beam with rejects and
  // would come back empty without the exact-scan backstop.
  const vdb::Filter needle = parsed(R"({"rank":{"$eq":864}})");
  const vdb::SearchResult found = opened.store->search(seeded_vector(500), 5, needle);
  ASSERT_TRUE(found.ok) << found.error;
  ASSERT_EQ(found.hits.size(), 1u);
  EXPECT_EQ(found.hits[0].document.content, "document 864");
}

TEST_F(VectorStoreTest, FilteredSearchAgreesWithAnExhaustiveScan) {
  vdb::StoreOptions graph = options();
  graph.brute_force_below = 0;
  vdb::StoreOpenResult opened = open_store(graph);
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 400).ok);

  const vdb::Filter filter = parsed(R"({"kind":"semantic","weight":{"$gte":0.5}})");
  const vdb::SearchResult found = opened.store->search(seeded_vector(42), 5, filter);
  ASSERT_TRUE(found.ok) << found.error;
  for (const vdb::ScoredDoc& hit : found.hits) {
    EXPECT_EQ(std::get<std::string>(hit.document.metadata.at("kind")),
              "semantic");
    EXPECT_GE(std::get<double>(hit.document.metadata.at("weight")), 0.5 - 1e-9);
  }
  EXPECT_FALSE(found.hits.empty());
}

TEST_F(VectorStoreTest, AFilterMatchingNothingIsAnEmptyOkResult) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 20).ok);
  const vdb::SearchResult found =
      opened.store->search(seeded_vector(1), 5, parsed(R"({"rank":-1})"));
  EXPECT_TRUE(found.ok) << found.error;
  EXPECT_TRUE(found.hits.empty());
}

TEST_F(VectorStoreTest, SearchingWithAQueryOfTheWrongDimensionIsAnError) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 5).ok);
  const vdb::SearchResult found = opened.store->search(std::vector<float>(3), 2);
  EXPECT_FALSE(found.ok);
  EXPECT_NE(found.error.find("dimensions"), std::string::npos) << found.error;
}

TEST_F(VectorStoreTest, SearchingAnEmptyStoreIsAnEmptyOkResult) {
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const vdb::SearchResult found = opened.store->search(seeded_vector(1), 5);
  EXPECT_TRUE(found.ok) << found.error;
  EXPECT_TRUE(found.hits.empty());
}

TEST_F(VectorStoreTest, TheExactAndGraphPathsReturnTheSameTopHit) {
  vdb::StoreOptions exact = options();
  exact.brute_force_below = 100000;
  vdb::StoreOpenResult scanned = open_store(exact);
  ASSERT_TRUE(scanned.ok) << scanned.error;
  ASSERT_TRUE(fill(*scanned.store, 500).ok);
  const vdb::SearchResult by_scan = scanned.store->search(seeded_vector(123), 1);
  ASSERT_TRUE(by_scan.ok) << by_scan.error;
  scanned.store.reset();

  vdb::StoreOptions graph = options();
  graph.brute_force_below = 0;
  vdb::StoreOpenResult walked = open_store(graph);
  ASSERT_TRUE(walked.ok) << walked.error;
  const vdb::SearchResult by_graph = walked.store->search(seeded_vector(123), 1);
  ASSERT_TRUE(by_graph.ok) << by_graph.error;
  EXPECT_EQ(by_scan.hits[0].id, by_graph.hits[0].id);
}

TEST_F(VectorStoreTest, ConcurrentAddsAndSearchesDoNotCorruptTheStore) {
  // The header claims every method is thread-safe because subagents run on
  // their own threads against one store. This is that claim, exercised.
  vdb::StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  vdb::VectorStore& store = *opened.store;

  constexpr int kWriters = 4;
  constexpr int kPerWriter = 40;
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};

  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        const int seed = w * 1000 + i;
        const vdb::AddResult added = store.add(
            {"document " + std::to_string(seed)}, {meta_for(seed)},
            {seeded_vector(static_cast<uint32_t>(seed))});
        if (not added.ok) ++failures;
      }
    });
  }
  for (int r = 0; r < 2; ++r) {
    threads.emplace_back([&] {
      for (int i = 0; i < 100; ++i) {
        const vdb::SearchResult found = store.search(seeded_vector(1), 5);
        if (not found.ok) ++failures;
      }
    });
  }
  for (std::thread& thread : threads) thread.join();

  EXPECT_EQ(failures.load(), 0);
  EXPECT_EQ(store.doc_count(), kWriters * kPerWriter);

  // Every document a writer claimed to have stored must be findable by its own
  // vector, which is what would break if the graph had been raced into an
  // inconsistent state.
  for (int w = 0; w < kWriters; ++w) {
    for (int i = 0; i < kPerWriter; i += 7) {
      const int seed = w * 1000 + i;
      const vdb::SearchResult found =
          store.search(seeded_vector(static_cast<uint32_t>(seed)), 1);
      ASSERT_TRUE(found.ok) << found.error;
      ASSERT_EQ(found.hits.size(), 1u);
      EXPECT_EQ(found.hits[0].document.content,
                "document " + std::to_string(seed));
    }
  }
}

}  // namespace
}  // namespace agent
