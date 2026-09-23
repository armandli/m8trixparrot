// VectorIndex is the HNSW graph on its own: the distance kernels agree with a
// hand-rolled scalar reference, the graph finds what an exact scan finds, and a
// serialized graph searches identically to the one it was taken from.

#include <cmath>
#include <cstdint>
#include <random>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/vector_index.h>

namespace agent {
namespace {

std::vector<std::vector<float>> random_vectors(size_t count, size_t dim,
                                               uint32_t seed) {
  std::mt19937 rng(seed);
  std::normal_distribution<float> gauss(0.0f, 1.0f);
  std::vector<std::vector<float>> out(count, std::vector<float>(dim));
  for (std::vector<float>& v : out) {
    for (float& x : v) x = gauss(rng);
  }
  return out;
}

IndexParams params_for(size_t dim, Metric metric = Metric::Cosine) {
  IndexParams params;
  params.dim = static_cast<uint32_t>(dim);
  params.metric = metric;
  params.m = 16;
  params.ef_construction = 200;
  params.ef_search = 100;
  return params;
}

struct VectorIndexTest : ::testing::Test {};

TEST_F(VectorIndexTest, DotProductMatchesAScalarReference) {
  // Sized to straddle every unrolled block in the NEON kernel: 16, 4 and 1.
  for (const size_t dim : {1u, 3u, 4u, 7u, 16u, 17u, 33u, 128u, 383u}) {
    const std::vector<std::vector<float>> v = random_vectors(2, dim, 7);
    double want = 0.0;
    for (size_t i = 0; i < dim; ++i) {
      want += static_cast<double>(v[0][i]) * static_cast<double>(v[1][i]);
    }
    EXPECT_NEAR(dot_product(v[0].data(), v[1].data(), dim), want, 1e-3)
        << "dim " << dim;
  }
}

TEST_F(VectorIndexTest, L2SquaredMatchesAScalarReference) {
  for (const size_t dim : {1u, 3u, 4u, 7u, 16u, 17u, 33u, 128u, 383u}) {
    const std::vector<std::vector<float>> v = random_vectors(2, dim, 11);
    double want = 0.0;
    for (size_t i = 0; i < dim; ++i) {
      const double d = static_cast<double>(v[0][i]) - v[1][i];
      want += d * d;
    }
    EXPECT_NEAR(l2_squared(v[0].data(), v[1].data(), dim), want, 1e-3)
        << "dim " << dim;
  }
}

TEST_F(VectorIndexTest, NormalizeLeavesAZeroVectorAlone) {
  std::vector<float> zero(8, 0.0f);
  normalize(zero.data(), zero.size());
  for (const float x : zero) EXPECT_EQ(x, 0.0f);
}

TEST_F(VectorIndexTest, FindsExactNeighborsOnATinyGraph) {
  VectorIndex index(params_for(3));
  const std::vector<std::vector<float>> points = {
      {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f},
      {0.9f, 0.1f, 0.0f}, {-1.0f, 0.0f, 0.0f},
  };
  for (const std::vector<float>& p : points) index.add(p.data());

  const std::vector<float> query = {1.0f, 0.05f, 0.0f};
  const std::vector<Neighbor> hits = index.search(query.data(), 2, {});
  ASSERT_EQ(hits.size(), 2u);
  EXPECT_TRUE(hits[0].label == 0 or hits[0].label == 3);
  EXPECT_TRUE(hits[1].label == 0 or hits[1].label == 3);
  EXPECT_LE(hits[0].distance, hits[1].distance);
}

TEST_F(VectorIndexTest, RecallStaysAboveNinetyPercentAgainstAnExactScan) {
  const size_t dim = 64, count = 3000, k = 10;
  const std::vector<std::vector<float>> data = random_vectors(count, dim, 42);
  VectorIndex index(params_for(dim));
  for (const std::vector<float>& v : data) index.add(v.data());
  ASSERT_EQ(index.size(), count);

  const std::vector<std::vector<float>> queries = random_vectors(40, dim, 43);
  double recall = 0.0;
  for (const std::vector<float>& query : queries) {
    const std::vector<Neighbor> approximate = index.search(query.data(), k, {});
    const std::vector<Neighbor> exact = index.scan(query.data(), k, {});
    std::set<uint32_t> truth;
    for (const Neighbor& n : exact) truth.insert(n.label);
    size_t hit = 0;
    for (const Neighbor& n : approximate) hit += truth.count(n.label);
    recall += static_cast<double>(hit) / static_cast<double>(k);
  }
  EXPECT_GT(recall / static_cast<double>(queries.size()), 0.9);
}

TEST_F(VectorIndexTest, ScanReturnsResultsNearestFirst) {
  const size_t dim = 16;
  const std::vector<std::vector<float>> data = random_vectors(200, dim, 5);
  VectorIndex index(params_for(dim));
  for (const std::vector<float>& v : data) index.add(v.data());

  const std::vector<Neighbor> hits = index.scan(data[0].data(), 10, {});
  ASSERT_EQ(hits.size(), 10u);
  EXPECT_EQ(hits[0].label, 0u);  // A vector is its own nearest neighbour.
  for (size_t i = 1; i < hits.size(); ++i) {
    EXPECT_LE(hits[i - 1].distance, hits[i].distance);
  }
}

TEST_F(VectorIndexTest, APredicateKeepsRejectedLabelsOutOfBothPaths) {
  const size_t dim = 16;
  const std::vector<std::vector<float>> data = random_vectors(500, dim, 9);
  VectorIndex index(params_for(dim));
  for (const std::vector<float>& v : data) index.add(v.data());

  const LabelPredicate every_seventh = [](uint32_t label) {
    return label % 7 == 0;
  };
  for (const std::vector<Neighbor>& hits :
       {index.scan(data[1].data(), 10, every_seventh),
        index.search(data[1].data(), 10, every_seventh)}) {
    EXPECT_FALSE(hits.empty());
    for (const Neighbor& n : hits) EXPECT_EQ(n.label % 7, 0u);
  }
}

TEST_F(VectorIndexTest, SnapshotRoundTripsTheGraph) {
  const size_t dim = 32, count = 800;
  const std::vector<std::vector<float>> data = random_vectors(count, dim, 13);
  VectorIndex index(params_for(dim));
  for (const std::vector<float>& v : data) index.add(v.data());

  const std::string blob = index.serialize_graph();
  ASSERT_FALSE(blob.empty());

  VectorIndex restored(params_for(dim));
  for (const std::vector<float>& v : data) restored.load_vector(v.data());
  std::string error;
  ASSERT_TRUE(restored.deserialize_graph(blob, error)) << error;
  EXPECT_EQ(restored.linked_count(), index.linked_count());

  const std::vector<std::vector<float>> queries = random_vectors(10, dim, 14);
  for (const std::vector<float>& query : queries) {
    const std::vector<Neighbor> want = index.search(query.data(), 10, {});
    const std::vector<Neighbor> got = restored.search(query.data(), 10, {});
    ASSERT_EQ(want.size(), got.size());
    for (size_t i = 0; i < want.size(); ++i) {
      EXPECT_EQ(want[i].label, got[i].label);
    }
  }
}

TEST_F(VectorIndexTest, ASnapshotCoveringAPrefixIsExtendedByLinking) {
  const size_t dim = 24, prefix = 300, total = 500;
  const std::vector<std::vector<float>> data = random_vectors(total, dim, 21);

  VectorIndex partial(params_for(dim));
  for (size_t i = 0; i < prefix; ++i) partial.add(data[i].data());
  const std::string blob = partial.serialize_graph();

  VectorIndex restored(params_for(dim));
  for (const std::vector<float>& v : data) restored.load_vector(v.data());
  std::string error;
  ASSERT_TRUE(restored.deserialize_graph(blob, error)) << error;
  EXPECT_EQ(restored.linked_count(), prefix);
  for (uint32_t label = prefix; label < total; ++label) restored.link(label);
  EXPECT_EQ(restored.linked_count(), total);

  // Every vector must now be findable, including one only added after the
  // snapshot was taken.
  const std::vector<Neighbor> hits =
      restored.search(data[total - 1].data(), 1, {});
  ASSERT_EQ(hits.size(), 1u);
  EXPECT_EQ(hits[0].label, total - 1);
}

TEST_F(VectorIndexTest, ATruncatedSnapshotIsAnErrorNotACrash) {
  const size_t dim = 8;
  const std::vector<std::vector<float>> data = random_vectors(50, dim, 3);
  VectorIndex index(params_for(dim));
  for (const std::vector<float>& v : data) index.add(v.data());
  const std::string blob = index.serialize_graph();

  for (const size_t keep : {size_t{0}, size_t{3}, blob.size() / 2}) {
    VectorIndex restored(params_for(dim));
    for (const std::vector<float>& v : data) restored.load_vector(v.data());
    std::string error;
    EXPECT_FALSE(restored.deserialize_graph(blob.substr(0, keep), error));
    EXPECT_FALSE(error.empty());
  }
}

TEST_F(VectorIndexTest, ASnapshotWithMoreNodesThanVectorsIsRejected) {
  const size_t dim = 8;
  const std::vector<std::vector<float>> data = random_vectors(50, dim, 4);
  VectorIndex index(params_for(dim));
  for (const std::vector<float>& v : data) index.add(v.data());
  const std::string blob = index.serialize_graph();

  VectorIndex restored(params_for(dim));
  for (size_t i = 0; i < 10; ++i) restored.load_vector(data[i].data());
  std::string error;
  EXPECT_FALSE(restored.deserialize_graph(blob, error));
  EXPECT_NE(error.find("50"), std::string::npos) << error;
}

TEST_F(VectorIndexTest, AnEmptyIndexSearchesToNothing) {
  VectorIndex index(params_for(8));
  const std::vector<float> query(8, 1.0f);
  EXPECT_TRUE(index.search(query.data(), 5, {}).empty());
  EXPECT_TRUE(index.scan(query.data(), 5, {}).empty());
}

TEST_F(VectorIndexTest, L2AndInnerProductOrderTheirOwnNearest) {
  const std::vector<std::vector<float>> points = {
      {0.0f, 0.0f}, {1.0f, 0.0f}, {5.0f, 0.0f}, {10.0f, 0.0f},
  };
  const std::vector<float> query = {0.9f, 0.0f};

  VectorIndex l2(params_for(2, Metric::L2));
  for (const std::vector<float>& p : points) l2.add(p.data());
  EXPECT_EQ(l2.scan(query.data(), 1, {}).front().label, 1u);

  // Inner product favours the longest vector pointing the same way, not the
  // closest one — the ordering is genuinely different, not just rescaled.
  VectorIndex ip(params_for(2, Metric::InnerProduct));
  for (const std::vector<float>& p : points) ip.add(p.data());
  EXPECT_EQ(ip.scan(query.data(), 1, {}).front().label, 3u);
}

}  // namespace
}  // namespace agent
