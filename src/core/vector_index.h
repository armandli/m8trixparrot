#ifndef VECTOR_INDEX_H
#define VECTOR_INDEX_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace agent {

enum struct Metric : uint8_t {
  Cosine = 0,
  L2 = 1,
  InnerProduct = 2,
};

// ---------------------------------------------------------------------------
// Distance kernels.
//
// NEON on arm64, a scalar loop everywhere else. Deliberately no <immintrin.h>:
// an x86-only intrinsic header is what makes caliby's distance.hpp unbuildable
// on this machine, and the scalar fallback is fast enough that an AVX path
// would only add a way to break the build.
// ---------------------------------------------------------------------------

float dot_product(const float* a, const float* b, size_t n);
float l2_squared(const float* a, const float* b, size_t n);

// Scales `v` to unit length in place. A zero vector is left alone — its
// direction is undefined, and dividing would produce NaNs that then poison
// every comparison it takes part in.
void normalize(float* v, size_t n);

// Lower is nearer, for all three metrics, so one comparator orders them all.
// Cosine assumes both operands are already unit length (VectorIndex normalizes
// on add), which turns the cosine into the dot product.
float metric_distance(Metric metric, const float* a, const float* b, size_t n);

struct IndexParams {
  uint32_t dim = 0;
  Metric metric = Metric::Cosine;
  uint32_t m = 16;               // Links per node per layer; 2*m on layer 0.
  uint32_t ef_construction = 200;
  uint32_t ef_search = 64;
  // Fixed so a rebuild from the same insert order produces the same graph,
  // which is what makes the snapshot tests reproducible.
  uint64_t seed = 0x9E3779B97F4A7C15ull;
};

// A label is a dense position in insertion order, not a document id. The store
// owns the label <-> id mapping; the index never sees an id.
struct Neighbor {
  uint32_t label = 0;
  float distance = 0.0f;
};

// True when `label` may appear in the results. Traversal passes through a
// rejected node regardless — cutting the graph at a filtered node disconnects
// the parts of it that only that node reaches.
using LabelPredicate = std::function<bool(uint32_t label)>;

// Hierarchical Navigable Small World graph over vectors it owns.
//
// Not thread-safe: VectorStore holds the lock. Splitting the locking out keeps
// this file free of any I/O or synchronization, so it tests standalone.
struct VectorIndex {
  explicit VectorIndex(const IndexParams& params);

  // Copies `dim` floats, normalizing them first under Cosine, and links the
  // new node into the graph. Returns its label.
  uint32_t add(const float* vector);

  // The k nearest accepted labels, nearest first. Walks the graph.
  std::vector<Neighbor> search(const float* query, size_t k,
                               const LabelPredicate& accept) const;

  // The k nearest accepted labels by exact scan. Always correct, and faster
  // than the graph below a few thousand vectors or behind a selective filter.
  std::vector<Neighbor> scan(const float* query, size_t k,
                             const LabelPredicate& accept) const;

  const float* vector_at(uint32_t label) const {
    return mVectors.data() + static_cast<size_t>(label) * mParams.dim;
  }

  // Vectors held, linked or not. linked_count() trails it only in the middle
  // of a snapshot restore, while the post-snapshot tail is being linked back
  // in.
  uint32_t size() const {
    return mParams.dim == 0
               ? 0
               : static_cast<uint32_t>(mVectors.size() / mParams.dim);
  }
  uint32_t linked_count() const {
    return static_cast<uint32_t>(mLevels.size());
  }
  uint32_t dim() const { return mParams.dim; }
  Metric metric() const { return mParams.metric; }
  const IndexParams& params() const { return mParams; }

  // The graph alone — levels, adjacency, entry point. Vectors are not included:
  // the store already has every one of them in its PutDoc records, and writing
  // them twice would double the file for no gain.
  std::string serialize_graph() const;

  // Rebuilds the graph over vectors already loaded by `load_vector`. The node
  // count in `blob` must equal the number loaded, or this fails.
  bool deserialize_graph(std::string_view blob, std::string& error);

  // Appends a vector without linking it into the graph, for the replay path
  // that will restore the graph from a snapshot instead of rebuilding it.
  // Normalizes under Cosine exactly as add() does.
  uint32_t load_vector(const float* vector);

  // Links label `from` (already loaded by load_vector, and beyond whatever the
  // snapshot covered) into the graph.
  void link(uint32_t label);

  // Drops the graph and keeps every loaded vector, so the caller can link them
  // all again from scratch. The recovery path for a snapshot that failed to
  // deserialize: deserialize_graph clears the graph before it starts filling
  // it, so a failure part way leaves a partial one behind.
  void reset_graph();

protected:
  // Neighbours of `label` on `layer`, as a slice of mAdjacency.
  uint32_t* neighbors(uint32_t label, uint32_t layer);
  const uint32_t* neighbors(uint32_t label, uint32_t layer) const;
  uint32_t degree(uint32_t label, uint32_t layer) const;
  void set_degree(uint32_t label, uint32_t layer, uint32_t count);
  uint32_t capacity_at(uint32_t layer) const {
    return layer == 0 ? mParams.m * 2 : mParams.m;
  }

  float distance_to(const float* query, uint32_t label) const {
    return metric_distance(mParams.metric, query, vector_at(label),
                           mParams.dim);
  }

  uint32_t random_level();
  // Greedy descent on one layer: returns the nearest node found from `entry`.
  uint32_t greedy_descend(const float* query, uint32_t entry,
                          uint32_t layer) const;
  // The ef-sized candidate set on `layer`, nearest first.
  std::vector<Neighbor> search_layer(const float* query, uint32_t entry,
                                     size_t ef, uint32_t layer) const;
  // Algorithm 4 of the HNSW paper: prefer a candidate that is nearer to the
  // query than to any already-selected neighbour, which spreads the links out
  // instead of clustering them all on one side.
  std::vector<Neighbor> select_neighbors(std::vector<Neighbor> candidates,
                                         size_t limit) const;
  void connect(uint32_t label, const std::vector<Neighbor>& selected,
               uint32_t layer);
  void reserve_node(uint32_t label, uint32_t level);

  IndexParams mParams;
  std::vector<float> mVectors;      // dim-strided, one allocation.
  std::vector<uint8_t> mLevels;     // Top layer each node reaches.
  // Adjacency for every node, layers packed back to back at a fixed stride so
  // a node's slice is one offset computation rather than a chase through
  // per-layer vectors. Slot 0 of each layer's block holds the degree.
  std::vector<uint32_t> mAdjacency;
  std::vector<size_t> mNodeOffset;  // Into mAdjacency, per node.
  uint32_t mEntryPoint = 0;
  uint32_t mMaxLevel = 0;
  bool mEmpty = true;
  uint64_t mRngState = 0;
};

}  // namespace agent

#endif  // VECTOR_INDEX_H
