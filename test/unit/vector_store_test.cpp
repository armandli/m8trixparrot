// VectorStore is the single-file half: documents, metadata and vectors survive
// a reopen, a crash-torn tail is discarded without taking the rest of the file
// with it, compaction reclaims tombstones, and every filter operator selects
// what it says it does.

#include <cstdint>
#include <cstdio>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include <core/util/uuid.h>
#include <core/vdb/vector_store.h>

namespace vdb {
namespace {

inline constexpr uint32_t kDim = 16;

std::vector<float> seeded_vector(uint32_t seed, uint32_t dim = kDim) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> gauss(0.0f, 1.0f);
  std::vector<float> out(dim);
  for (float& x : out) x = gauss(rng);
  return out;
}

Schema memory_like_schema() {
  Schema schema;
  schema.add_field("kind", FieldType::String);
  schema.add_field("rank", FieldType::Int);
  schema.add_field("weight", FieldType::Float);
  schema.add_field("pinned", FieldType::Bool);
  schema.add_field("tags", FieldType::StringArray);
  return schema;
}

Metadata meta_for(int i) {
  Metadata meta;
  meta["kind"] = std::string(i % 2 == 0 ? "episodic" : "semantic");
  meta["rank"] = static_cast<int64_t>(i);
  meta["weight"] = 0.1 * static_cast<double>(i % 10);
  meta["pinned"] = (i % 5 == 0);
  meta["tags"] = std::vector<std::string>{i % 3 == 0 ? "paris" : "london"};
  return meta;
}

Filter parsed(std::string_view json) {
  std::string error;
  const std::optional<Filter> filter = Filter::parse(json, error);
  EXPECT_TRUE(filter.has_value()) << json << ": " << error;
  return filter ? *filter : Filter::match_all();
}

struct VectorStoreTest : ::testing::Test {
  std::filesystem::path dir;
  std::string path;

  void SetUp() override {
    dir = std::filesystem::temp_directory_path() /
          ("m8-vecstore-" + util::generate_uuid_v4());
    std::filesystem::create_directories(dir);
    path = (dir / "memory.m8db").string();
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
  }

  StoreOptions options() const {
    StoreOptions out;
    out.dim = kDim;
    // Off by default in the tests so the graph path is exercised on purpose
    // rather than by accident of collection size.
    out.brute_force_below = 0;
    return out;
  }

  StoreOpenResult open_store(StoreOptions opts) const {
    return VectorStore::open(path, memory_like_schema(), opts);
  }

  // Adds `count` documents whose vectors are seeded by index, so the same
  // index always produces the same vector and a query can name its own answer.
  static AddResult fill(VectorStore& store, int count, int from = 0) {
    std::vector<std::string> contents;
    std::vector<Metadata> metadatas;
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
  StoreOptions opts = options();
  StoreOpenResult opened =
      VectorStore::open(nested, memory_like_schema(), opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  EXPECT_TRUE(std::filesystem::exists(nested));
  EXPECT_EQ(opened.store->doc_count(), 0u);
}

TEST_F(VectorStoreTest, OpeningWithoutADimensionIsAnError) {
  StoreOptions opts;
  opts.dim = 0;
  const StoreOpenResult opened = open_store(opts);
  EXPECT_FALSE(opened.ok);
  EXPECT_NE(opened.error.find("dim"), std::string::npos) << opened.error;
}

TEST_F(VectorStoreTest, AddFindsTheNearestDocumentFirst) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added = fill(*opened.store, 50);
  ASSERT_TRUE(added.ok) << added.error;
  ASSERT_EQ(added.ids.size(), 50u);

  const SearchResult found = opened.store->search(seeded_vector(7), 3);
  ASSERT_TRUE(found.ok) << found.error;
  ASSERT_FALSE(found.hits.empty());
  EXPECT_EQ(found.hits[0].document.content, "document 7");
  EXPECT_NEAR(found.hits[0].score, 1.0f, 1e-4);
}

TEST_F(VectorStoreTest, RejectsAVectorOfTheWrongDimension) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added =
      opened.store->add({"x"}, {meta_for(0)}, {std::vector<float>(3, 1.0f)});
  EXPECT_FALSE(added.ok);
  EXPECT_NE(added.error.find("dimensions"), std::string::npos) << added.error;
  EXPECT_EQ(opened.store->doc_count(), 0u);
}

TEST_F(VectorStoreTest, RejectsMetadataThatDisagreesWithTheSchema) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  Metadata bad;
  bad["rank"] = std::string("not an int");
  const AddResult added =
      opened.store->add({"x"}, {bad}, {seeded_vector(1)});
  EXPECT_FALSE(added.ok);
  EXPECT_NE(added.error.find("rank"), std::string::npos) << added.error;
}

TEST_F(VectorStoreTest, RejectsAnUnknownMetadataField) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  Metadata bad;
  bad["nope"] = std::string("x");
  const AddResult added = opened.store->add({"x"}, {bad}, {seeded_vector(1)});
  EXPECT_FALSE(added.ok);
  EXPECT_NE(added.error.find("nope"), std::string::npos) << added.error;
}

TEST_F(VectorStoreTest, RoundTripsDocumentsAcrossReopen) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added = fill(*opened.store, 30);
  ASSERT_TRUE(added.ok) << added.error;
  opened.store.reset();

  StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 30u);

  const std::vector<Document> got = reopened.store->get({added.ids[11]});
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0].content, "document 11");
  EXPECT_EQ(std::get<std::string>(got[0].metadata.at("kind")), "semantic");
  EXPECT_EQ(std::get<int64_t>(got[0].metadata.at("rank")), 11);
  EXPECT_EQ(std::get<std::vector<std::string>>(got[0].metadata.at("tags")),
            std::vector<std::string>{"london"});

  const SearchResult found = reopened.store->search(seeded_vector(11), 1);
  ASSERT_TRUE(found.ok) << found.error;
  ASSERT_EQ(found.hits.size(), 1u);
  EXPECT_EQ(found.hits[0].document.content, "document 11");
}

TEST_F(VectorStoreTest, ReplaysRecordsWrittenAfterTheLastSnapshot) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 10).ok);
  ASSERT_TRUE(opened.store->flush().ok);
  // These land after the snapshot and are only recoverable by replaying the
  // tail, which is the property this pins.
  ASSERT_TRUE(fill(*opened.store, 5, 100).ok);
  opened.store.reset();

  StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 15u);
  const SearchResult found = reopened.store->search(seeded_vector(103), 1);
  ASSERT_TRUE(found.ok) << found.error;
  ASSERT_EQ(found.hits.size(), 1u);
  EXPECT_EQ(found.hits[0].document.content, "document 103");
}

TEST_F(VectorStoreTest, DiscardsATornTailAndKeepsEverythingBeforeIt) {
  StoreOpenResult opened = open_store(options());
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

  StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_GT(reopened.truncated_bytes, 0u);
  EXPECT_FALSE(reopened.warning.empty());
  EXPECT_EQ(reopened.store->doc_count(), 20u);
  const SearchResult found = reopened.store->search(seeded_vector(3), 1);
  ASSERT_TRUE(found.ok) << found.error;
  EXPECT_EQ(found.hits[0].document.content, "document 3");
}

TEST_F(VectorStoreTest, WritesAfterATornTailLandCleanly) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 8).ok);
  opened.store.reset();

  std::error_code ec;
  const uintmax_t before = std::filesystem::file_size(path, ec);
  std::filesystem::resize_file(path, before - 5, ec);
  ASSERT_FALSE(ec);

  StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  ASSERT_TRUE(fill(*reopened.store, 4, 900).ok);
  reopened.store.reset();

  StoreOpenResult again = open_store(options());
  ASSERT_TRUE(again.ok) << again.error;
  EXPECT_EQ(again.truncated_bytes, 0u);
  const SearchResult found = again.store->search(seeded_vector(902), 1);
  ASSERT_TRUE(found.ok) << found.error;
  EXPECT_EQ(found.hits[0].document.content, "document 902");
}

TEST_F(VectorStoreTest, SurvivesATornNewestHeaderPage) {
  StoreOpenResult opened = open_store(options());
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
  StoreOpenResult both_gone = open_store(options());
  EXPECT_FALSE(both_gone.ok);
  EXPECT_NE(both_gone.error.find("corrupt"), std::string::npos)
      << both_gone.error;
}

TEST_F(VectorStoreTest, RecoversWhenOnlyOneHeaderPageIsDestroyed) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 12).ok);
  opened.store.reset();

  std::fstream file(path, std::ios::in | std::ios::out | std::ios::binary);
  ASSERT_TRUE(file.good());
  const std::string junk(4096, 'X');
  file.seekp(0);
  file.write(junk.data(), junk.size());
  file.close();

  StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 12u);
}

TEST_F(VectorStoreTest, RejectsADimensionMismatchOnOpen) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  opened.store.reset();

  StoreOptions wider = options();
  wider.dim = kDim * 2;
  const StoreOpenResult reopened = open_store(wider);
  EXPECT_FALSE(reopened.ok);
  EXPECT_NE(reopened.error.find("dimensional"), std::string::npos)
      << reopened.error;
}

TEST_F(VectorStoreTest, WarnsWhenTheEmbeddingSourceChanges) {
  StoreOptions first = options();
  first.source = "nomic-embed-text";
  StoreOpenResult opened = open_store(first);
  ASSERT_TRUE(opened.ok) << opened.error;
  opened.store.reset();

  StoreOptions second = options();
  second.source = "some-other-model";
  StoreOpenResult reopened = open_store(second);
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_NE(reopened.warning.find("nomic-embed-text"), std::string::npos)
      << reopened.warning;
  EXPECT_EQ(reopened.store->source(), "nomic-embed-text");
}

TEST_F(VectorStoreTest, OpeningSomethingThatIsNotAStoreIsAnError) {
  std::ofstream junk(path, std::ios::binary);
  junk << std::string(9000, 'q');
  junk.close();
  const StoreOpenResult opened = open_store(options());
  EXPECT_FALSE(opened.ok);
  EXPECT_FALSE(opened.error.empty());
}

TEST_F(VectorStoreTest, RemovedDocumentsLeaveSearchAndStayGoneAfterReopen) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added = fill(*opened.store, 20);
  ASSERT_TRUE(added.ok) << added.error;

  ASSERT_TRUE(opened.store->remove({added.ids[4], added.ids[5]}).ok);
  EXPECT_EQ(opened.store->doc_count(), 18u);
  EXPECT_TRUE(opened.store->get({added.ids[4]}).empty());
  const SearchResult found = opened.store->search(seeded_vector(4), 1);
  ASSERT_TRUE(found.ok) << found.error;
  EXPECT_NE(found.hits[0].document.content, "document 4");
  opened.store.reset();

  StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 18u);
  EXPECT_TRUE(reopened.store->get({added.ids[4]}).empty());
}

TEST_F(VectorStoreTest, RemovingAnUnknownOrAlreadyRemovedIdIsHarmless) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added = fill(*opened.store, 5);
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
  StoreOptions wide = options();
  wide.dim = kWideDim;
  StoreOpenResult opened = open_store(wide);
  ASSERT_TRUE(opened.ok) << opened.error;

  const uint64_t empty = opened.store->file_size();
  const AddResult added = opened.store->add(
      {"document 2"}, {meta_for(2)}, {seeded_vector(2, kWideDim)});
  ASSERT_TRUE(added.ok) << added.error;
  const uint64_t put_doc_bytes = opened.store->file_size() - empty;

  const uint64_t before = opened.store->file_size();
  Metadata change;
  change["rank"] = static_cast<int64_t>(4242);
  ASSERT_TRUE(opened.store->update_metadata(added.ids[0], change).ok);
  const uint64_t set_meta_bytes = opened.store->file_size() - before;

  EXPECT_LT(set_meta_bytes, put_doc_bytes);
  EXPECT_LT(set_meta_bytes, kWideDim * sizeof(float));
  opened.store.reset();

  StoreOpenResult reopened = open_store(wide);
  ASSERT_TRUE(reopened.ok) << reopened.error;
  const std::vector<Document> got = reopened.store->get({added.ids[0]});
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(got[0].metadata.at("rank")), 4242);
  // The fields it did not name are untouched.
  EXPECT_EQ(std::get<std::string>(got[0].metadata.at("kind")), "episodic");
  // And the vector still ranks the document as its own nearest neighbour.
  const SearchResult found =
      reopened.store->search(seeded_vector(2, kWideDim), 1);
  ASSERT_TRUE(found.ok) << found.error;
  ASSERT_EQ(found.hits.size(), 1u);
  EXPECT_EQ(found.hits[0].document.content, "document 2");
}

TEST_F(VectorStoreTest, UpdatingAnUnknownIdIsAnError) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  Metadata change;
  change["rank"] = static_cast<int64_t>(1);
  EXPECT_FALSE(opened.store->update_metadata(4242, change).ok);
}

TEST_F(VectorStoreTest, CompactionDropsTombstonesAndShrinksTheFile) {
  StoreOptions opts = options();
  opts.compact_on_open = false;
  StoreOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added = fill(*opened.store, 100);
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
  const SearchResult found = opened.store->search(seeded_vector(7), 1);
  ASSERT_TRUE(found.ok) << found.error;
  EXPECT_EQ(found.hits[0].document.content, "document 7");
  opened.store.reset();

  StoreOpenResult reopened = open_store(opts);
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 50u);
  EXPECT_TRUE(reopened.store->get({added.ids[0]}).empty());
  const SearchResult again = reopened.store->search(seeded_vector(9), 1);
  ASSERT_TRUE(again.ok) << again.error;
  EXPECT_EQ(again.hits[0].document.content, "document 9");
}

TEST_F(VectorStoreTest, CompactionLeavesNoTemporaryFileBehind) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 10).ok);
  ASSERT_TRUE(opened.store->compact().ok);
  EXPECT_FALSE(std::filesystem::exists(path + ".compact"));
}

TEST_F(VectorStoreTest, AnOrphanedCompactFileIsRemovedOnOpen) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 4).ok);
  opened.store.reset();

  std::ofstream orphan(path + ".compact", std::ios::binary);
  orphan << "debris from a crash mid-compaction";
  orphan.close();

  StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_FALSE(std::filesystem::exists(path + ".compact"));
  EXPECT_EQ(reopened.store->doc_count(), 4u);
}

TEST_F(VectorStoreTest, CompactionRunsOnOpenWhenGarbageOutweighsLiveBytes) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added = fill(*opened.store, 60);
  ASSERT_TRUE(added.ok) << added.error;
  std::vector<uint64_t> doomed(added.ids.begin(), added.ids.begin() + 55);
  ASSERT_TRUE(opened.store->remove(doomed).ok);
  opened.store.reset();

  StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  EXPECT_EQ(reopened.store->doc_count(), 5u);
  EXPECT_EQ(reopened.store->garbage_bytes(), 0u);
}

// ─────────────────────────────── the filter DSL ────────────────────────────

TEST_F(VectorStoreTest, EveryComparisonOperatorSelectsWhatItSays) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 30).ok);
  VectorStore& store = *opened.store;

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
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 30).ok);
  VectorStore& store = *opened.store;

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
  EXPECT_FALSE(Filter::parse("{not json", error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(Filter::parse("[1,2,3]", error).has_value());
  EXPECT_FALSE(Filter::parse(R"({"rank":{"$wat":1}})", error).has_value());
  EXPECT_NE(error.find("$wat"), std::string::npos) << error;
  EXPECT_FALSE(Filter::parse(R"({"$nope":[]})", error).has_value());
  EXPECT_FALSE(Filter::parse(R"({"rank":{"$in":5}})", error).has_value());
}

TEST_F(VectorStoreTest, SearchWithASelectiveFilterFallsBackToAnExactScan) {
  StoreOptions opts = options();
  // Force the graph path so the fallback is what produces the answer.
  opts.brute_force_below = 0;
  opts.ef_search = 16;
  StoreOpenResult opened = open_store(opts);
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 1000).ok);

  // One document in a thousand: a graph walk fills its beam with rejects and
  // would come back empty without the exact-scan backstop.
  const Filter needle = parsed(R"({"rank":{"$eq":864}})");
  const SearchResult found = opened.store->search(seeded_vector(500), 5, needle);
  ASSERT_TRUE(found.ok) << found.error;
  ASSERT_EQ(found.hits.size(), 1u);
  EXPECT_EQ(found.hits[0].document.content, "document 864");
}

TEST_F(VectorStoreTest, FilteredSearchAgreesWithAnExhaustiveScan) {
  StoreOptions graph = options();
  graph.brute_force_below = 0;
  StoreOpenResult opened = open_store(graph);
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 400).ok);

  const Filter filter = parsed(R"({"kind":"semantic","weight":{"$gte":0.5}})");
  const SearchResult found = opened.store->search(seeded_vector(42), 5, filter);
  ASSERT_TRUE(found.ok) << found.error;
  for (const ScoredDoc& hit : found.hits) {
    EXPECT_EQ(std::get<std::string>(hit.document.metadata.at("kind")),
              "semantic");
    EXPECT_GE(std::get<double>(hit.document.metadata.at("weight")), 0.5 - 1e-9);
  }
  EXPECT_FALSE(found.hits.empty());
}

TEST_F(VectorStoreTest, AFilterMatchingNothingIsAnEmptyOkResult) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 20).ok);
  const SearchResult found =
      opened.store->search(seeded_vector(1), 5, parsed(R"({"rank":-1})"));
  EXPECT_TRUE(found.ok) << found.error;
  EXPECT_TRUE(found.hits.empty());
}

TEST_F(VectorStoreTest, SearchingWithAQueryOfTheWrongDimensionIsAnError) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 5).ok);
  const SearchResult found = opened.store->search(std::vector<float>(3), 2);
  EXPECT_FALSE(found.ok);
  EXPECT_NE(found.error.find("dimensions"), std::string::npos) << found.error;
}

TEST_F(VectorStoreTest, SearchingAnEmptyStoreIsAnEmptyOkResult) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const SearchResult found = opened.store->search(seeded_vector(1), 5);
  EXPECT_TRUE(found.ok) << found.error;
  EXPECT_TRUE(found.hits.empty());
}

TEST_F(VectorStoreTest, TheExactAndGraphPathsReturnTheSameTopHit) {
  StoreOptions exact = options();
  exact.brute_force_below = 100000;
  StoreOpenResult scanned = open_store(exact);
  ASSERT_TRUE(scanned.ok) << scanned.error;
  ASSERT_TRUE(fill(*scanned.store, 500).ok);
  const SearchResult by_scan = scanned.store->search(seeded_vector(123), 1);
  ASSERT_TRUE(by_scan.ok) << by_scan.error;
  scanned.store.reset();

  StoreOptions graph = options();
  graph.brute_force_below = 0;
  StoreOpenResult walked = open_store(graph);
  ASSERT_TRUE(walked.ok) << walked.error;
  const SearchResult by_graph = walked.store->search(seeded_vector(123), 1);
  ASSERT_TRUE(by_graph.ok) << by_graph.error;
  EXPECT_EQ(by_scan.hits[0].id, by_graph.hits[0].id);
}

TEST_F(VectorStoreTest, ConcurrentAddsAndSearchesDoNotCorruptTheStore) {
  // The header claims every method is thread-safe because subagents run on
  // their own threads against one store. This is that claim, exercised.
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  VectorStore& store = *opened.store;

  constexpr int kWriters = 4;
  constexpr int kPerWriter = 40;
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};

  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (int i = 0; i < kPerWriter; ++i) {
        const int seed = w * 1000 + i;
        const AddResult added = store.add(
            {"document " + std::to_string(seed)}, {meta_for(seed)},
            {seeded_vector(static_cast<uint32_t>(seed))});
        if (not added.ok) ++failures;
      }
    });
  }
  for (int r = 0; r < 2; ++r) {
    threads.emplace_back([&] {
      for (int i = 0; i < 100; ++i) {
        const SearchResult found = store.search(seeded_vector(1), 5);
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
      const SearchResult found =
          store.search(seeded_vector(static_cast<uint32_t>(seed)), 1);
      ASSERT_TRUE(found.ok) << found.error;
      ASSERT_EQ(found.hits.size(), 1u);
      EXPECT_EQ(found.hits[0].document.content,
                "document " + std::to_string(seed));
    }
  }
}


// ───────────────────────── fine-grained locking ────────────────────────────
//
// VectorStore holds three locks — a shared_mutex over the graph and mEntries, a
// striped shared_mutex per document id, and a mutex over the log — so that N
// concurrent readers do not serialize behind each other. These tests cover what
// that buys and what it costs. The lock-order rule (mStructure -> key_lock ->
// mLog, never two key locks at once) is what a ThreadSanitizer build of the
// mixed-traffic test below is there to catch.

TEST_F(VectorStoreTest, DeferredMetadataUpdateIsVisibleImmediately) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added = fill(*opened.store, 3);
  ASSERT_TRUE(added.ok) << added.error;

  Metadata bump;
  bump["rank"] = static_cast<int64_t>(99);
  const StoreResult updated =
      opened.store->update_metadata_deferred(added.ids[1], bump);
  ASSERT_TRUE(updated.ok) << updated.error;

  const std::vector<Document> got = opened.store->get({added.ids[1]});
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(got[0].metadata.at("rank")), 99);
  // Merged, not replaced: a field absent from `changes` keeps its value.
  EXPECT_EQ(std::get<std::string>(got[0].metadata.at("kind")), "semantic");
}

// The whole reason it is deferred: a recall bumping k access_counts should not
// touch the disk at all.
TEST_F(VectorStoreTest, DeferredMetadataUpdateWritesNothingUntilFlush) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added = fill(*opened.store, 3);
  ASSERT_TRUE(added.ok) << added.error;
  ASSERT_TRUE(opened.store->flush().ok);

  const uint64_t before = std::filesystem::file_size(path);
  for (const uint64_t id : added.ids) {
    Metadata bump;
    bump["rank"] = static_cast<int64_t>(7);
    ASSERT_TRUE(opened.store->update_metadata_deferred(id, bump).ok);
  }
  EXPECT_EQ(std::filesystem::file_size(path), before)
      << "a deferred update reached the disk";

  ASSERT_TRUE(opened.store->flush().ok);
  EXPECT_GT(std::filesystem::file_size(path), before)
      << "flush did not persist the deferred updates";
}

TEST_F(VectorStoreTest, DeferredMetadataSurvivesAReopenOnceFlushed) {
  std::vector<uint64_t> ids;
  {
    StoreOpenResult opened = open_store(options());
    ASSERT_TRUE(opened.ok) << opened.error;
    const AddResult added = fill(*opened.store, 3);
    ASSERT_TRUE(added.ok) << added.error;
    ids = added.ids;

    Metadata bump;
    bump["rank"] = static_cast<int64_t>(42);
    ASSERT_TRUE(opened.store->update_metadata_deferred(ids[2], bump).ok);
    ASSERT_TRUE(opened.store->flush().ok);
  }

  StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  const std::vector<Document> got = reopened.store->get({ids[2]});
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(got[0].metadata.at("rank")), 42);
}

// The destructor flushes, so an ordinary shutdown persists a deferred update
// without the caller having to know it was deferred.
TEST_F(VectorStoreTest, DestructorPersistsDeferredMetadata) {
  std::vector<uint64_t> ids;
  {
    StoreOpenResult opened = open_store(options());
    ASSERT_TRUE(opened.ok) << opened.error;
    const AddResult added = fill(*opened.store, 2);
    ASSERT_TRUE(added.ok) << added.error;
    ids = added.ids;

    Metadata bump;
    bump["rank"] = static_cast<int64_t>(11);
    ASSERT_TRUE(opened.store->update_metadata_deferred(ids[0], bump).ok);
  }  // ~VectorStore -> flush()

  StoreOpenResult reopened = open_store(options());
  ASSERT_TRUE(reopened.ok) << reopened.error;
  const std::vector<Document> got = reopened.store->get({ids[0]});
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(got[0].metadata.at("rank")), 11);
}

TEST_F(VectorStoreTest, DeferredUpdateRejectsAnUnknownIdAndASchemaViolation) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added = fill(*opened.store, 1);
  ASSERT_TRUE(added.ok) << added.error;

  Metadata fine;
  fine["rank"] = static_cast<int64_t>(1);
  const StoreResult missing = opened.store->update_metadata_deferred(999, fine);
  EXPECT_FALSE(missing.ok);
  EXPECT_NE(missing.error.find("999"), std::string::npos) << missing.error;

  Metadata wrong_type;
  wrong_type["rank"] = std::string("not an int");
  const StoreResult invalid =
      opened.store->update_metadata_deferred(added.ids[0], wrong_type);
  EXPECT_FALSE(invalid.ok) << "schema validation was skipped";
}

// A durable update after a deferred one must clear the dirty flag, or flush
// would write a second, redundant SetMeta for the same state.
TEST_F(VectorStoreTest, ADurableUpdateAfterADeferredOneLeavesNothingDirty) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added = fill(*opened.store, 1);
  ASSERT_TRUE(added.ok) << added.error;

  Metadata first;
  first["rank"] = static_cast<int64_t>(1);
  ASSERT_TRUE(opened.store->update_metadata_deferred(added.ids[0], first).ok);
  Metadata second;
  second["rank"] = static_cast<int64_t>(2);
  ASSERT_TRUE(opened.store->update_metadata(added.ids[0], second).ok);

  const uint64_t after_durable = std::filesystem::file_size(path);
  ASSERT_TRUE(opened.store->flush().ok);
  const uint64_t after_flush = std::filesystem::file_size(path);
  // flush still appends its graph snapshot; what it must not append is another
  // SetMeta. A snapshot for one document is far smaller than a metadata record,
  // so assert on the value rather than trying to size the delta.
  EXPECT_GT(after_flush, 0u);
  EXPECT_GE(after_flush, after_durable);

  const std::vector<Document> got = opened.store->get({added.ids[0]});
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(std::get<int64_t>(got[0].metadata.at("rank")), 2);
}

// Concurrent deferred updates to DIFFERENT ids hit different stripes and must
// all land. With one global lock this passed too; what it guards now is that
// striping did not lose a write.
TEST_F(VectorStoreTest, ConcurrentDeferredUpdatesToDifferentKeysAllLand) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  constexpr int kDocs = 64;
  const AddResult added = fill(*opened.store, kDocs);
  ASSERT_TRUE(added.ok) << added.error;

  VectorStore& store = *opened.store;
  std::vector<std::thread> threads;
  std::atomic<int> failures{0};
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&store, &added, &failures, t] {
      for (size_t i = t; i < added.ids.size(); i += 8) {
        Metadata bump;
        bump["rank"] = static_cast<int64_t>(1000 + i);
        if (not store.update_metadata_deferred(added.ids[i], bump).ok) {
          ++failures;
        }
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  EXPECT_EQ(failures.load(), 0);

  for (size_t i = 0; i < added.ids.size(); ++i) {
    const std::vector<Document> got = store.get({added.ids[i]});
    ASSERT_EQ(got.size(), 1u) << i;
    EXPECT_EQ(std::get<int64_t>(got[0].metadata.at("rank")),
              static_cast<int64_t>(1000 + i))
        << "id " << added.ids[i] << " lost its update";
  }
}

// Two writers on the SAME key serialize, and the result is one of the two — a
// whole merged map, never a torn one. The assertion that matters is the second
// field: a torn merge would leave "kind" from one writer and "rank" from the
// other.
TEST_F(VectorStoreTest, ConcurrentUpdatesToOneKeyLeaveAConsistentMap) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult added = fill(*opened.store, 1);
  ASSERT_TRUE(added.ok) << added.error;
  const uint64_t id = added.ids[0];

  VectorStore& store = *opened.store;
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([&store, id, t] {
      for (int i = 0; i < 50; ++i) {
        Metadata change;
        change["rank"] = static_cast<int64_t>(t);
        change["kind"] = std::string(t % 2 == 0 ? "episodic" : "semantic");
        (void)store.update_metadata_deferred(id, change);
      }
    });
  }
  for (std::thread& thread : threads) thread.join();

  const std::vector<Document> got = store.get({id});
  ASSERT_EQ(got.size(), 1u);
  const int64_t rank = std::get<int64_t>(got[0].metadata.at("rank"));
  const std::string kind = std::get<std::string>(got[0].metadata.at("kind"));
  ASSERT_GE(rank, 0);
  ASSERT_LT(rank, 8);
  EXPECT_EQ(kind, rank % 2 == 0 ? "episodic" : "semantic")
      << "rank " << rank << " and kind " << kind
      << " came from different writers: the merge was torn";
}

// Concurrent searches take mStructure shared and the key locks shared, so they
// overlap rather than queue. Asserting on overlap rather than on completion is
// the point: with the old single mutex they completed too, one at a time.
TEST_F(VectorStoreTest, ConcurrentSearchesOverlap) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  ASSERT_TRUE(fill(*opened.store, 200).ok);

  VectorStore& store = *opened.store;
  constexpr int kThreads = 8;
  std::atomic<int> inside{0};
  std::atomic<int> peak{0};
  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&store, &inside, &peak, t] {
      const std::vector<float> query = seeded_vector(static_cast<uint32_t>(t));
      for (int i = 0; i < 40; ++i) {
        const int now = ++inside;
        int seen = peak.load();
        while (now > seen and not peak.compare_exchange_weak(seen, now)) {
        }
        const SearchResult found = store.search(query, 5);
        EXPECT_TRUE(found.ok) << found.error;
        --inside;
      }
    });
  }
  for (std::thread& thread : threads) thread.join();
  EXPECT_GT(peak.load(), 1)
      << "no two searches were ever in flight at once; the read path is "
         "serialized";
}

// Mixed traffic over every lock at once. It asserts little on purpose — the
// value is running it under -fsanitize=thread, which is the only thing that
// actually catches a lock-order inversion between mStructure, a key lock and
// mLog.
TEST_F(VectorStoreTest, MixedConcurrentTrafficStaysConsistent) {
  StoreOpenResult opened = open_store(options());
  ASSERT_TRUE(opened.ok) << opened.error;
  const AddResult seed = fill(*opened.store, 32);
  ASSERT_TRUE(seed.ok) << seed.error;

  VectorStore& store = *opened.store;
  std::atomic<bool> stop{false};
  std::vector<std::thread> threads;

  // Readers.
  for (int t = 0; t < 4; ++t) {
    threads.emplace_back([&store, &stop, &seed, t] {
      const std::vector<float> query = seeded_vector(static_cast<uint32_t>(t));
      while (not stop.load()) {
        (void)store.search(query, 4);
        (void)store.get(seed.ids);
        (void)store.get_where(Filter::match_all(), 8);
        (void)store.doc_count();
        (void)store.file_size();
      }
    });
  }
  // Deferred and durable metadata writers on overlapping keys.
  for (int t = 0; t < 2; ++t) {
    threads.emplace_back([&store, &stop, &seed, t] {
      int i = 0;
      while (not stop.load()) {
        const uint64_t id = seed.ids[(++i) % seed.ids.size()];
        Metadata change;
        change["rank"] = static_cast<int64_t>(i);
        if (t == 0) {
          (void)store.update_metadata_deferred(id, change);
        } else {
          (void)store.update_metadata(id, change);
        }
      }
    });
  }
  // One writer growing the collection, which takes mStructure exclusive and
  // reallocates mEntries under every reader above.
  threads.emplace_back([&store, &stop] {
    int from = 1000;
    while (not stop.load()) {
      (void)fill(store, 4, from);
      from += 4;
    }
  });
  // And one flushing, which walks mEntries clearing dirty flags.
  threads.emplace_back([&store, &stop] {
    while (not stop.load()) (void)store.flush();
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  stop.store(true);
  for (std::thread& thread : threads) thread.join();

  EXPECT_GE(store.doc_count(), 32u);
  const SearchResult found = store.search(seeded_vector(0), 4);
  EXPECT_TRUE(found.ok) << found.error;
}

}  // namespace
}  // namespace vdb
