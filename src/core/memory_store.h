#ifndef MEMORY_STORE_H
#define MEMORY_STORE_H

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <core/basic_ollama_client.h>
#include <core/tools.h>
#include <core/vector_store.h>

namespace agent {

// The six metadata fields a memory carries, mirroring caliby's
// examples/agentic_memory_store.py exactly. Named constants because the filter
// DSL refers to them by string, where a typo is a runtime parse error rather
// than a compile error.
inline constexpr const char* kMemoryFieldType = "memory_type";
inline constexpr const char* kMemoryFieldContext = "context_id";
inline constexpr const char* kMemoryFieldTimestamp = "timestamp";
inline constexpr const char* kMemoryFieldImportance = "importance";
inline constexpr const char* kMemoryFieldAccessCount = "access_count";
inline constexpr const char* kMemoryFieldTags = "tags";

// Documented values, not enforced ones — a caller may use any label.
inline constexpr const char* kMemoryEpisodic = "episodic";
inline constexpr const char* kMemorySemantic = "semantic";
inline constexpr const char* kMemoryProcedural = "procedural";

// Default location. .gitignore already reserves .m8trix/* with the comment
// "m8trixparrot's per-workspace sessions and memory".
inline constexpr const char* kMemoryPath = ".m8trix/memory.m8db";

Schema memory_schema();

// ---------------------------------------------------------------------------
// The embedding boundary.
//
// The only place in the memory system that can touch the network, and a
// std::function so that a test never does. Returns an empty vector and sets
// `error` on failure; nothing throws.
// ---------------------------------------------------------------------------

using Embedder =
    std::function<std::vector<float>(std::string_view text, std::string& error)>;

// Embeds through the OllamaClient work pool, so an embedding counts against
// the same concurrency cap as a chat — see ollama_client.h. The host is the one
// OllamaClient::configure() was given; only the model is chosen here.
//
// Note the narrowing: the embed call hands back std::vector<std::vector<double>>
// and the conversion to float happens here, at the boundary, before the store
// normalizes — normalizing in double and narrowing afterwards would leave
// stored vectors slightly off unit length, which is the one invariant the
// cosine fast path relies on.
Embedder ollama_embedder(std::string model);

// Deterministic, network-free, and deliberately crude: tokens are hashed into
// a fixed-width unit vector, so texts that share words land near each other.
// It exists so the demo runs with no embedding model pulled and so tests stay
// hermetic. It is not a language model and makes no claim to be one.
Embedder hash_embedder(uint32_t dim);

struct Memory {
  uint64_t id = 0;
  std::string content;
  std::string memory_type = kMemoryEpisodic;
  std::string context_id = "default";
  double timestamp = 0.0;   // Unix seconds; 0 means "stamp it on the way in".
  double importance = 0.5;  // [0, 1]
  int64_t access_count = 0;
  std::vector<std::string> tags;
};

struct RecallQuery {
  std::string query;
  size_t k = 5;
  std::optional<std::string> memory_type;
  std::optional<std::string> context_id;
  std::optional<double> min_importance;
  std::vector<std::string> tags;  // Each becomes a $contains; all are ANDed.
  std::string filters;            // Raw filter DSL, ANDed with the above.
  // Unset takes MemoryOptions::recency_weight.
  std::optional<double> recency_weight;
  std::optional<double> importance_weight;
};

struct ScoredMemory {
  Memory memory;
  float similarity = 0.0f;  // Cosine, in [-1, 1].
  double recency = 0.0;     // exp(-age_hours / half_life), in (0, 1].
  double score = 0.0;       // The blend this was actually ranked by.
};

struct MemoryOptions {
  std::string path = kMemoryPath;
  std::string embed_model = kDefaultEmbedModel;
  // Empty: ollama_embedder(embed_model) is built on open.
  Embedder embedder;
  // 0 means "discover it from the first embedding" — see MemoryStore::open.
  uint32_t embedding_dim = 0;

  // The python decays over 24 hours: exp(-age_hours / 24).
  double recency_half_life_hours = 24.0;
  // score = (1 - wr - wi) * similarity + wr * recency + wi * importance.
  // wi defaults to 0, which is the python's exact two-term blend.
  double recency_weight = 0.3;
  double importance_weight = 0.0;

  // Re-ranking needs more candidates than it returns, or a slightly less
  // similar but much fresher memory can never be promoted into the top k.
  size_t recall_oversample = 4;
  size_t recall_oversample_min = 32;

  // A runaway agent should not be able to write a gigabyte one memory at a
  // time. Content longer than this is rejected, not silently truncated.
  size_t max_content_bytes = 64 * 1024;

  StoreOptions store;
};

struct MemoryStats {
  uint64_t total = 0;
  uint64_t episodic = 0;
  uint64_t semantic = 0;
  uint64_t procedural = 0;
  uint64_t other = 0;
  uint32_t dim = 0;
  uint64_t file_bytes = 0;
  std::string path;
};

struct RecallResult {
  bool ok = false;
  std::vector<ScoredMemory> memories;
  std::string error;
};

struct RememberResult {
  bool ok = false;
  uint64_t id = 0;
  std::string error;
};

struct MemoryStore;

struct MemoryOpenResult {
  bool ok = false;
  std::unique_ptr<MemoryStore> store;
  std::string error;
  std::string warning;  // A torn tail, or an embedding-model change.
};

// The agentic layer over VectorStore: text in, ranked memories out.
//
// Thread-safe. The embedding call happens outside the store's lock, which is
// what keeps a slow model from serializing every other agent's recall.
struct MemoryStore {
  // An existing file is asked for its vector width; a new one cannot be
  // created until something is embedded, because the header has to record a
  // dimension and an embedding model does not advertise one. So with
  // `embedding_dim` unset and no file present, open() succeeds without
  // touching the disk and the file appears on the first remember().
  static MemoryOpenResult open(const MemoryOptions& options);

  RememberResult remember(const Memory& memory);
  // Not const: every memory it returns has its access_count bumped, which is a
  // write back into the store.
  RecallResult recall(const RecallQuery& query);
  StoreResult forget(const std::vector<uint64_t>& ids);
  MemoryStats stats() const;
  StoreResult flush();

  const std::string& path() const { return mOptions.path; }

protected:
  MemoryStore() = default;

  // Opens or creates the underlying store now that `dim` is known. A no-op
  // once it is open; an error if `dim` disagrees with the existing file.
  StoreResult ensure_store_locked(uint32_t dim);
  static Metadata metadata_of(const Memory& memory);
  static Memory memory_of(const Document& document);
  std::optional<Filter> build_filter(const RecallQuery& query,
                                     std::string& error) const;

  MemoryOptions mOptions;
  Embedder mEmbedder;
  std::string mOpenWarning;
  std::unique_ptr<VectorStore> mStore;
  mutable std::mutex mMutex;
};

// Process-wide, keyed by the absolute path of the file. A store owns a file
// and holds its whole contents in memory, so two handles on one path would
// each go stale the moment the other wrote — the same reason BashSearchIndex
// (tools_bash_search.cpp) is a singleton over its index file.
struct MemoryStoreRegistry {
  static MemoryStoreRegistry& instance();

  // Opens `options.path` on first use. Null with `error` set when it cannot be
  // opened; the returned pointer lives as long as the process.
  MemoryStore* get(const MemoryOptions& options, std::string& error);

  // Drops every open store. For tests, which chdir between cases and would
  // otherwise have the second case reuse the first one's store.
  void reset();

protected:
  std::mutex mMutex;
  std::map<std::string, std::unique_ptr<MemoryStore>> mStores;
};

// Long-term memory over a single-file vector store. Stateful like SkillTool,
// and declared here rather than in tools.h for the same reason: its
// configuration type lives in this header, and tools.h is included nearly
// everywhere.
struct MemoryTool {
  MemoryOptions options;

  static std::string description();
  // action (string, required): "remember" | "recall" | "forget"
  ToolResult execute(const ToolArgs& args) const;
};

// True when the memory tool has an embedding model it can reach. Mirrors
// web_search_available(), for an app that wants to warn at startup rather than
// let the agent discover it one failed tool call at a time. Probes the host
// OllamaClient::configure() was given, so call it after that.
bool memory_available(const std::string& model);

// Non-empty when `path` already exists and was built with a different embedding
// model than `model`; the text names the file, both models and the stored
// width, and says what to do about it.
//
// This compares the model name, not the vector width, because the width is the
// case that already fails loudly: two 768-dimensional models open fine, accept
// writes and rank against each other, and the only sign anything is wrong is
// that recall quietly stops working. A file written before the model name was
// stamped into the header has none to compare, and is left alone.
std::string memory_model_mismatch(const std::string& path,
                                  const std::string& model);

}  // namespace agent

#endif  // MEMORY_STORE_H
