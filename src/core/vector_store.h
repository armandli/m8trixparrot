#ifndef VECTOR_STORE_H
#define VECTOR_STORE_H

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <core/vector_index.h>

namespace agent {

// ---------------------------------------------------------------------------
// Schema and metadata.
//
// The shape is caliby's: a collection declares typed metadata fields up front,
// documents carry values for them, and a JSON filter DSL selects on them at
// search time. What caliby builds on top of that — secondary indexes over the
// metadata — is left out: this store's whole working set is in memory, so a
// predicate over a few tens of thousands of documents is a map lookup per
// document, not an index probe.
// ---------------------------------------------------------------------------

enum struct FieldType : uint8_t {
  String = 0,
  Int = 1,
  Float = 2,
  Bool = 3,
  StringArray = 4,
};

using MetaValue = std::variant<std::monostate, std::string, int64_t, double,
                               bool, std::vector<std::string>>;

// std::less<> so a lookup by string_view doesn't allocate, matching ToolArgs.
using Metadata = std::map<std::string, MetaValue, std::less<>>;

std::string_view field_type_name(FieldType type);

struct Schema {
  void add_field(std::string name, FieldType type);
  std::optional<FieldType> field_type(std::string_view name) const;
  bool empty() const { return fields.empty(); }

  // Rejects an unknown field name and a value whose variant alternative
  // disagrees with the declared type. Int is accepted where Float is declared
  // (JSON writes 1 for 1.0); the reverse is not.
  bool validate(const Metadata& meta, std::string& error) const;

  std::string to_json() const;
  static std::optional<Schema> from_json(std::string_view json,
                                         std::string& error);

  std::vector<std::pair<std::string, FieldType>> fields;
};

struct Document {
  uint64_t id = 0;
  std::string content;
  Metadata metadata;
};

struct ScoredDoc {
  uint64_t id = 0;
  // Higher is better, whatever the metric: cosine similarity in [-1,1], the
  // raw inner product, or the negated squared L2 distance. Callers rank on one
  // number without having to know which metric produced it.
  float score = 0.0f;
  Document document;
};

// ---------------------------------------------------------------------------
// Filter DSL.
//
// {"type":"semantic"}                      field equals value
// {"importance":{"$gte":0.5}}              one operator
// {"tags":{"$contains":"paris"}}           string-array membership
// {"kind":{"$in":["a","b"]}}               value in list
// {"$or":[{...},{...}]}                    explicit disjunction
// {"a":1,"b":2}                            distinct fields are ANDed
// ---------------------------------------------------------------------------

enum struct FilterOp : uint8_t {
  Eq = 0,
  Ne = 1,
  Lt = 2,
  Lte = 3,
  Gt = 4,
  Gte = 5,
  In = 6,
  Nin = 7,
  Contains = 8,
  And = 9,
  Or = 10,
};

struct FilterNode {
  FilterOp op = FilterOp::And;
  std::string field;               // Empty for And/Or.
  MetaValue value;                 // The operand for the scalar comparisons.
  std::vector<MetaValue> values;   // The operand list for In/Nin.
  std::vector<FilterNode> children;
};

struct Filter {
  // An And with no children: every document passes.
  static Filter match_all() { return Filter{}; }
  bool is_match_all() const {
    return root.op == FilterOp::And and root.children.empty();
  }

  // An empty or whitespace-only `json` means match-all. A parse failure is
  // reported through `error`, never thrown — simdjson's exceptions are caught
  // at this boundary.
  //
  // Parsed with simdjson's DOM API rather than On-Demand: a filter is
  // recursive and its shape is not known ahead of time, which On-Demand's
  // forward-only cursor cannot walk.
  static std::optional<Filter> parse(std::string_view json,
                                     std::string& error);

  bool matches(const Metadata& meta) const;

  FilterNode root;
};

// ---------------------------------------------------------------------------
// Results.
// ---------------------------------------------------------------------------

struct StoreResult {
  bool ok = false;
  std::string error;
};

struct AddResult {
  bool ok = false;
  std::vector<uint64_t> ids;
  std::string error;
};

struct SearchResult {
  bool ok = false;
  std::vector<ScoredDoc> hits;
  std::string error;
};

struct StoreOptions {
  uint32_t dim = 0;              // Required on create; must match on open.
  Metric metric = Metric::Cosine;
  uint32_t hnsw_m = 16;
  uint32_t hnsw_ef_construction = 200;
  uint32_t ef_search = 64;
  // Below this many documents an exact scan is both faster than a graph walk
  // and exact, so search never touches the HNSW graph. 768 dims x 8k documents
  // is ~6M fused multiply-adds: about a millisecond through the NEON kernel.
  uint32_t brute_force_below = 8192;
  // fcntl(F_FULLFSYNC) rather than fsync(). On macOS plain fsync() returns
  // before the drive has flushed its own write cache, so only F_FULLFSYNC
  // actually survives power loss — at a real cost per flush.
  bool full_fsync = true;
  // Rewrite the file on open when dead bytes exceed live ones.
  bool compact_on_open = true;
  // Free-text provenance stamped into the header and compared on reopen. It
  // exists for one failure this format cannot otherwise catch: swapping the
  // embedding model for a different one of the same dimension leaves every
  // stored vector silently incomparable with every new one, and recall quality
  // collapses with nothing to point at. A mismatch is a warning, not an error
  // — the caller decides whether to re-embed or carry on.
  std::string source;
};

struct VectorStore;

// What a store file says about itself, without opening or replaying it.
// MemoryStore needs this: an embedding model does not advertise its dimension,
// so an existing file has to be asked what width its vectors are before a
// store can be opened over it.
struct StoreProbe {
  bool exists = false;   // The path holds a readable store header.
  uint32_t dim = 0;
  Metric metric = Metric::Cosine;
  std::string source;
  uint64_t doc_count = 0;
};

StoreProbe probe_vector_store(const std::string& path);

// VectorStore owns a shared_mutex, which is neither movable nor copyable, so
// the store can only be handed back behind a pointer. `store` is null whenever
// `ok` is false.
struct StoreOpenResult {
  bool ok = false;
  std::unique_ptr<VectorStore> store;
  std::string error;
  // Set, with `ok` still true, when a crash-torn tail was discarded or the
  // file's `source` disagrees with the caller's. Both are conditions the
  // caller should see but neither is a reason to refuse the file.
  std::string warning;
  uint64_t truncated_bytes = 0;
};

// ---------------------------------------------------------------------------
// The store.
//
// One file, one collection of documents, each with text, typed metadata and a
// vector. The file is an append-only log of records behind a duplicated
// header, with the HNSW graph written out as a snapshot record on flush; see
// vector_store.cpp for the byte layout.
//
// Every method is thread-safe. An agent runs its tools on its own thread and
// subagents run concurrently against the same store, so the lock lives here
// rather than in each caller. It is a shared_mutex because recall outnumbers
// remember by a wide margin in the workload this exists for.
// ---------------------------------------------------------------------------

struct VectorStore {
  ~VectorStore();

  // Creates `path` when it is absent, otherwise opens and replays it. A dim or
  // metric disagreeing with the stored header is an error rather than a silent
  // reformat — that file is somebody's memory, and truncating it to satisfy a
  // mismatched caller would destroy it.
  static StoreOpenResult open(const std::string& path, const Schema& schema,
                              const StoreOptions& options);

  AddResult add(const std::vector<std::string>& contents,
                const std::vector<Metadata>& metadatas,
                const std::vector<std::vector<float>>& vectors);

  SearchResult search(const std::vector<float>& query, size_t k,
                      const Filter& filter = Filter::match_all()) const;

  std::vector<Document> get(const std::vector<uint64_t>& ids) const;
  std::vector<Document> get_where(const Filter& filter, size_t limit) const;

  // Merges `changes` into the document's metadata: a field absent from
  // `changes` keeps its current value.
  StoreResult update_metadata(uint64_t id, const Metadata& changes);

  StoreResult remove(const std::vector<uint64_t>& ids);

  // Appends a graph snapshot and rewrites the header. Called by the destructor,
  // so an ordinary shutdown needs no explicit flush.
  StoreResult flush();

  // Rewrites the file with only live documents and one snapshot. Runs through a
  // sibling temp file and a rename, so a crash mid-compaction leaves the
  // original intact — the one moment two files exist.
  StoreResult compact();

  uint64_t doc_count() const;
  uint64_t garbage_bytes() const;
  uint64_t file_size() const;
  uint32_t dim() const { return mOptions.dim; }
  const Schema& schema() const { return mSchema; }
  const std::string& path() const { return mPath; }
  // The `source` the file was created with, which may differ from the one the
  // caller just passed to open(). Empty when the file never carried one.
  const std::string& source() const { return mSource; }

protected:
  struct Entry {
    uint64_t id = 0;
    std::string content;
    Metadata metadata;
    bool deleted = false;
    uint64_t record_bytes = 0;  // Framed size of the PutDoc that defines it.
    uint64_t meta_bytes = 0;    // Framed size of its newest SetMeta, if any.
  };

  VectorStore() = default;

  // ── all require mMutex ──
  StoreResult append_record_locked(uint8_t type, const std::string& payload);
  StoreResult write_header_locked();
  StoreResult sync_locked();
  StoreResult flush_locked();
  StoreResult compact_locked();
  SearchResult search_locked(const std::vector<float>& query, size_t k,
                             const Filter& filter) const;
  std::string put_payload_locked(const Entry& entry, const float* vector) const;

  std::string mPath;
  StoreOptions mOptions;
  Schema mSchema;
  std::string mSource;
  std::unique_ptr<VectorIndex> mIndex;
  std::vector<Entry> mEntries;            // Indexed by the index's label.
  std::map<uint64_t, uint32_t> mIdToLabel;
  int mFd = -1;
  uint64_t mGeneration = 0;
  uint64_t mLogEnd = 0;
  uint64_t mSnapshotOffset = 0;
  uint64_t mSnapshotEnd = 0;
  uint64_t mGarbage = 0;
  uint64_t mNextId = 1;
  uint64_t mLiveCount = 0;
  // Only a fully-opened store may flush on destruction. open() abandons a
  // half-built one on any error, and that one has no index to snapshot and no
  // business writing a header over a file it could not make sense of.
  bool mReady = false;
  mutable std::shared_mutex mMutex;
};

}  // namespace agent

#endif  // VECTOR_STORE_H
