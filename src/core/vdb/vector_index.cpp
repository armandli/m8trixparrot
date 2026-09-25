#include <core/vdb/vector_index.h>

#include <algorithm>
#include <cmath>
#include <queue>

#include <core/vdb/byte_io.h>

#if defined(__ARM_NEON) or defined(__aarch64__)
#include <arm_neon.h>
#define M8_HAS_NEON 1
#endif

namespace vdb {

namespace {

// Slot 0 of every layer's adjacency block holds that layer's degree.
inline constexpr uint32_t kDegreeSlot = 1;

// Orders a max-heap by distance: the farthest candidate sits on top, so the
// worst result is the one that gets evicted when the beam is full.
struct FartherFirst {
  bool operator()(const Neighbor& a, const Neighbor& b) const {
    return a.distance < b.distance;
  }
};

struct NearerFirst {
  bool operator()(const Neighbor& a, const Neighbor& b) const {
    return a.distance > b.distance;
  }
};

// xorshift64*, so a rebuild from the same insert order yields the same graph.
// std::mt19937 would do as well; this avoids pulling <random> in for one call.
uint64_t next_random(uint64_t& state) {
  state ^= state >> 12;
  state ^= state << 25;
  state ^= state >> 27;
  return state * 0x2545F4914F6CDD1Dull;
}

}  // namespace

// ─────────────────────────── distance kernels ──────────────────────────────

float dot_product(const float* a, const float* b, size_t n) {
#ifdef M8_HAS_NEON
  // Four independent accumulators: the FMA latency is several cycles and one
  // accumulator would serialize on it, so this is roughly 4x the throughput of
  // a single chain.
  float32x4_t acc0 = vdupq_n_f32(0.0f);
  float32x4_t acc1 = vdupq_n_f32(0.0f);
  float32x4_t acc2 = vdupq_n_f32(0.0f);
  float32x4_t acc3 = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    acc0 = vfmaq_f32(acc0, vld1q_f32(a + i), vld1q_f32(b + i));
    acc1 = vfmaq_f32(acc1, vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
    acc2 = vfmaq_f32(acc2, vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
    acc3 = vfmaq_f32(acc3, vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
  }
  for (; i + 4 <= n; i += 4) {
    acc0 = vfmaq_f32(acc0, vld1q_f32(a + i), vld1q_f32(b + i));
  }
  float sum = vaddvq_f32(vaddq_f32(vaddq_f32(acc0, acc1), vaddq_f32(acc2, acc3)));
  for (; i < n; ++i) sum += a[i] * b[i];
  return sum;
#else
  float sum = 0.0f;
  for (size_t i = 0; i < n; ++i) sum += a[i] * b[i];
  return sum;
#endif
}

float l2_squared(const float* a, const float* b, size_t n) {
#ifdef M8_HAS_NEON
  float32x4_t acc0 = vdupq_n_f32(0.0f);
  float32x4_t acc1 = vdupq_n_f32(0.0f);
  float32x4_t acc2 = vdupq_n_f32(0.0f);
  float32x4_t acc3 = vdupq_n_f32(0.0f);
  size_t i = 0;
  for (; i + 16 <= n; i += 16) {
    const float32x4_t d0 = vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i));
    const float32x4_t d1 = vsubq_f32(vld1q_f32(a + i + 4), vld1q_f32(b + i + 4));
    const float32x4_t d2 = vsubq_f32(vld1q_f32(a + i + 8), vld1q_f32(b + i + 8));
    const float32x4_t d3 =
        vsubq_f32(vld1q_f32(a + i + 12), vld1q_f32(b + i + 12));
    acc0 = vfmaq_f32(acc0, d0, d0);
    acc1 = vfmaq_f32(acc1, d1, d1);
    acc2 = vfmaq_f32(acc2, d2, d2);
    acc3 = vfmaq_f32(acc3, d3, d3);
  }
  for (; i + 4 <= n; i += 4) {
    const float32x4_t d = vsubq_f32(vld1q_f32(a + i), vld1q_f32(b + i));
    acc0 = vfmaq_f32(acc0, d, d);
  }
  float sum = vaddvq_f32(vaddq_f32(vaddq_f32(acc0, acc1), vaddq_f32(acc2, acc3)));
  for (; i < n; ++i) {
    const float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
#else
  float sum = 0.0f;
  for (size_t i = 0; i < n; ++i) {
    const float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
#endif
}

void normalize(float* v, size_t n) {
  const float norm = std::sqrt(dot_product(v, v, n));
  if (norm <= 0.0f or not std::isfinite(norm)) return;
  const float inv = 1.0f / norm;
  for (size_t i = 0; i < n; ++i) v[i] *= inv;
}

float metric_distance(Metric metric, const float* a, const float* b, size_t n) {
  switch (metric) {
    case Metric::Cosine: return 1.0f - dot_product(a, b, n);
    case Metric::L2: return l2_squared(a, b, n);
    case Metric::InnerProduct: return -dot_product(a, b, n);
    default: return l2_squared(a, b, n);
  }
}

// ─────────────────────────────── VectorIndex ───────────────────────────────

VectorIndex::VectorIndex(const IndexParams& params)
    : mParams(params), mRngState(params.seed ? params.seed : 1ull) {
  if (mParams.m == 0) mParams.m = 16;
  if (mParams.ef_construction < mParams.m) mParams.ef_construction = mParams.m;
  if (mParams.ef_search == 0) mParams.ef_search = mParams.m;
}

uint32_t VectorIndex::load_vector(const float* vector) {
  const uint32_t label = size();
  mVectors.insert(mVectors.end(), vector, vector + mParams.dim);
  if (mParams.metric == Metric::Cosine) {
    normalize(mVectors.data() + static_cast<size_t>(label) * mParams.dim,
              mParams.dim);
  }
  return label;
}

uint32_t VectorIndex::add(const float* vector) {
  const uint32_t label = load_vector(vector);
  link(label);
  return label;
}

uint32_t* VectorIndex::neighbors(uint32_t label, uint32_t layer) {
  return const_cast<uint32_t*>(
      static_cast<const VectorIndex*>(this)->neighbors(label, layer));
}

const uint32_t* VectorIndex::neighbors(uint32_t label, uint32_t layer) const {
  size_t at = mNodeOffset[label];
  if (layer > 0) at += 1 + capacity_at(0) + (layer - 1) * (1 + capacity_at(1));
  return mAdjacency.data() + at + kDegreeSlot;
}

uint32_t VectorIndex::degree(uint32_t label, uint32_t layer) const {
  return *(neighbors(label, layer) - kDegreeSlot);
}

void VectorIndex::set_degree(uint32_t label, uint32_t layer, uint32_t count) {
  *(neighbors(label, layer) - kDegreeSlot) = count;
}

void VectorIndex::reserve_node(uint32_t label, uint32_t level) {
  size_t block = 1 + capacity_at(0);
  block += static_cast<size_t>(level) * (1 + capacity_at(1));
  if (label >= mNodeOffset.size()) mNodeOffset.resize(label + 1, 0);
  mNodeOffset[label] = mAdjacency.size();
  mAdjacency.resize(mAdjacency.size() + block, 0);
}

uint32_t VectorIndex::random_level() {
  // mL = 1/ln(M): the level distribution that makes the expected number of
  // layers logarithmic in the node count, per the HNSW paper.
  const double level_mult = 1.0 / std::log(static_cast<double>(mParams.m));
  const double unit =
      static_cast<double>(next_random(mRngState) >> 11) / 9007199254740992.0;
  const double draw = unit > 0.0 ? unit : 1e-12;
  const double level = -std::log(draw) * level_mult;
  // Clamped so one freak draw cannot allocate a tower of empty layers.
  return static_cast<uint32_t>(std::min(level, 30.0));
}

uint32_t VectorIndex::greedy_descend(const float* query, uint32_t entry,
                                     uint32_t layer) const {
  uint32_t current = entry;
  float best = distance_to(query, current);
  bool improved = true;
  while (improved) {
    improved = false;
    const uint32_t count = degree(current, layer);
    const uint32_t* links = neighbors(current, layer);
    for (uint32_t i = 0; i < count; ++i) {
      const float candidate = distance_to(query, links[i]);
      if (candidate < best) {
        best = candidate;
        current = links[i];
        improved = true;
      }
    }
  }
  return current;
}

std::vector<Neighbor> VectorIndex::search_layer(const float* query,
                                                uint32_t entry, size_t ef,
                                                uint32_t layer) const {
  // A local visited set rather than a mutable member: search() runs under a
  // shared lock, so concurrent readers would race on shared scratch space.
  std::vector<bool> visited(linked_count(), false);
  std::priority_queue<Neighbor, std::vector<Neighbor>, NearerFirst> candidates;
  std::priority_queue<Neighbor, std::vector<Neighbor>, FartherFirst> results;

  const float entry_distance = distance_to(query, entry);
  visited[entry] = true;
  candidates.push({entry, entry_distance});
  results.push({entry, entry_distance});

  while (not candidates.empty()) {
    const Neighbor nearest = candidates.top();
    if (results.size() >= ef and nearest.distance > results.top().distance) {
      break;
    }
    candidates.pop();

    const uint32_t count = degree(nearest.label, layer);
    const uint32_t* links = neighbors(nearest.label, layer);
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t next = links[i];
      if (next >= visited.size() or visited[next]) continue;
      visited[next] = true;
      const float distance = distance_to(query, next);
      if (results.size() < ef or distance < results.top().distance) {
        candidates.push({next, distance});
        results.push({next, distance});
        if (results.size() > ef) results.pop();
      }
    }
  }

  std::vector<Neighbor> out;
  out.reserve(results.size());
  while (not results.empty()) {
    out.push_back(results.top());
    results.pop();
  }
  std::reverse(out.begin(), out.end());
  return out;
}

std::vector<Neighbor> VectorIndex::select_neighbors(
    std::vector<Neighbor> candidates, size_t limit) const {
  std::sort(candidates.begin(), candidates.end(),
            [](const Neighbor& a, const Neighbor& b) {
              return a.distance < b.distance;
            });

  std::vector<Neighbor> selected;
  selected.reserve(limit);
  for (const Neighbor& candidate : candidates) {
    if (selected.size() >= limit) break;
    bool dominated = false;
    for (const Neighbor& chosen : selected) {
      // Drop a candidate that sits nearer to an already-chosen neighbour than
      // to the query: it adds a link into a direction the graph already
      // reaches, where a farther-but-unrepresented candidate adds a new one.
      const float to_chosen =
          metric_distance(mParams.metric, vector_at(candidate.label),
                          vector_at(chosen.label), mParams.dim);
      if (to_chosen < candidate.distance) {
        dominated = true;
        break;
      }
    }
    if (not dominated) selected.push_back(candidate);
  }
  return selected;
}

void VectorIndex::connect(uint32_t label,
                          const std::vector<Neighbor>& selected,
                          uint32_t layer) {
  const uint32_t capacity = capacity_at(layer);
  uint32_t* links = neighbors(label, layer);
  const uint32_t count =
      static_cast<uint32_t>(std::min<size_t>(selected.size(), capacity));
  for (uint32_t i = 0; i < count; ++i) links[i] = selected[i].label;
  set_degree(label, layer, count);

  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t other = selected[i].label;
    uint32_t* back = neighbors(other, layer);
    const uint32_t back_count = degree(other, layer);
    if (back_count < capacity) {
      back[back_count] = label;
      set_degree(other, layer, back_count + 1);
      continue;
    }

    // Full: re-run the heuristic over the existing links plus the new one, so
    // the link that gets dropped is the most redundant rather than the oldest.
    std::vector<Neighbor> pool;
    pool.reserve(back_count + 1);
    for (uint32_t j = 0; j < back_count; ++j) {
      pool.push_back({back[j], metric_distance(mParams.metric, vector_at(other),
                                               vector_at(back[j]),
                                               mParams.dim)});
    }
    pool.push_back({label, metric_distance(mParams.metric, vector_at(other),
                                           vector_at(label), mParams.dim)});
    const std::vector<Neighbor> kept = select_neighbors(std::move(pool), capacity);
    for (size_t j = 0; j < kept.size(); ++j) back[j] = kept[j].label;
    set_degree(other, layer, static_cast<uint32_t>(kept.size()));
  }
}

void VectorIndex::link(uint32_t label) {
  // Labels are handed out by load_vector in order and linked in that same
  // order, so anything else means the caller lost track of the replay.
  if (label != linked_count()) return;

  const uint32_t level = random_level();
  reserve_node(label, level);
  mLevels.push_back(static_cast<uint8_t>(level));

  if (mEmpty) {
    mEntryPoint = label;
    mMaxLevel = level;
    mEmpty = false;
    return;
  }

  const float* vector = vector_at(label);
  uint32_t current = mEntryPoint;
  for (uint32_t layer = mMaxLevel; layer > level; --layer) {
    current = greedy_descend(vector, current, layer);
  }

  for (uint32_t layer = std::min(level, mMaxLevel);; --layer) {
    std::vector<Neighbor> candidates =
        search_layer(vector, current, mParams.ef_construction, layer);
    // The node itself is in the graph's adjacency but not yet reachable, so it
    // never appears here; nothing to filter out.
    const std::vector<Neighbor> selected =
        select_neighbors(std::move(candidates), capacity_at(layer));
    connect(label, selected, layer);
    if (not selected.empty()) current = selected.front().label;
    if (layer == 0) break;
  }

  if (level > mMaxLevel) {
    mMaxLevel = level;
    mEntryPoint = label;
  }
}

void VectorIndex::reset_graph() {
  mLevels.clear();
  mAdjacency.clear();
  mNodeOffset.clear();
  mEntryPoint = 0;
  mMaxLevel = 0;
  mEmpty = true;
}

std::vector<Neighbor> VectorIndex::search(const float* query, size_t k,
                                          const LabelPredicate& accept) const {
  if (mEmpty or k == 0) return {};

  uint32_t current = mEntryPoint;
  for (uint32_t layer = mMaxLevel; layer > 0; --layer) {
    current = greedy_descend(query, current, layer);
  }

  const size_t ef = std::max<size_t>(mParams.ef_search, k);
  const std::vector<Neighbor> beam = search_layer(query, current, ef, 0);

  std::vector<Neighbor> out;
  out.reserve(std::min(k, beam.size()));
  for (const Neighbor& candidate : beam) {
    if (out.size() >= k) break;
    if (accept and not accept(candidate.label)) continue;
    out.push_back(candidate);
  }
  return out;
}

std::vector<Neighbor> VectorIndex::scan(const float* query, size_t k,
                                        const LabelPredicate& accept) const {
  if (k == 0) return {};
  std::priority_queue<Neighbor, std::vector<Neighbor>, FartherFirst> best;
  const uint32_t count = size();
  for (uint32_t label = 0; label < count; ++label) {
    // The predicate first: it is a map lookup where the distance is a pass over
    // `dim` floats, so rejecting early is what makes a selective filter cheap.
    if (accept and not accept(label)) continue;
    const float distance = distance_to(query, label);
    if (best.size() < k) {
      best.push({label, distance});
    } else if (distance < best.top().distance) {
      best.pop();
      best.push({label, distance});
    }
  }

  std::vector<Neighbor> out;
  out.reserve(best.size());
  while (not best.empty()) {
    out.push_back(best.top());
    best.pop();
  }
  std::reverse(out.begin(), out.end());
  return out;
}

// ────────────────────────── graph serialization ────────────────────────────

std::string VectorIndex::serialize_graph() const {
  std::string out;
  const uint32_t nodes = linked_count();
  out.reserve(static_cast<size_t>(nodes) * (1 + capacity_at(0)) * sizeof(uint32_t));
  put_u32(out, nodes);
  put_u32(out, mEntryPoint);
  put_u32(out, mMaxLevel);
  put_u8(out, mEmpty ? 1 : 0);
  for (uint32_t label = 0; label < nodes; ++label) {
    const uint32_t level = mLevels[label];
    put_u8(out, static_cast<uint8_t>(level));
    for (uint32_t layer = 0; layer <= level; ++layer) {
      const uint32_t count = degree(label, layer);
      put_u16(out, static_cast<uint16_t>(count));
      const uint32_t* links = neighbors(label, layer);
      for (uint32_t i = 0; i < count; ++i) put_u32(out, links[i]);
    }
  }
  return out;
}

bool VectorIndex::deserialize_graph(std::string_view blob, std::string& error) {
  ByteReader reader(blob);
  const uint32_t nodes = reader.u32();
  const uint32_t entry = reader.u32();
  const uint32_t max_level = reader.u32();
  const bool empty = reader.u8() != 0;
  if (not reader.ok()) {
    error = "graph snapshot is truncated in its header";
    return false;
  }
  if (nodes > size()) {
    error = "graph snapshot names " + std::to_string(nodes) +
            " nodes but only " + std::to_string(size()) +
            " vectors were loaded";
    return false;
  }

  mLevels.clear();
  mAdjacency.clear();
  mNodeOffset.clear();
  mLevels.reserve(nodes);
  mNodeOffset.reserve(nodes);

  for (uint32_t label = 0; label < nodes; ++label) {
    const uint32_t level = reader.u8();
    if (not reader.ok() or level > 30) {
      error = "graph snapshot has a bad level for node " +
              std::to_string(label);
      return false;
    }
    reserve_node(label, level);
    mLevels.push_back(static_cast<uint8_t>(level));
    for (uint32_t layer = 0; layer <= level; ++layer) {
      const uint32_t count = reader.u16();
      if (not reader.ok() or count > capacity_at(layer)) {
        error = "graph snapshot has a bad degree for node " +
                std::to_string(label);
        return false;
      }
      uint32_t* links = neighbors(label, layer);
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t next = reader.u32();
        if (not reader.ok() or next >= nodes) {
          error = "graph snapshot has an out-of-range link on node " +
                  std::to_string(label);
          return false;
        }
        links[i] = next;
      }
      set_degree(label, layer, count);
    }
  }

  if (not empty and entry >= nodes) {
    error = "graph snapshot has an out-of-range entry point";
    return false;
  }
  mEntryPoint = entry;
  mMaxLevel = max_level;
  mEmpty = empty or nodes == 0;
  return true;
}

}  // namespace vdb
