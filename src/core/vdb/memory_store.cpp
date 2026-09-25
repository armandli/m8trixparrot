#include <core/vdb/memory_store.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <utility>

#include <core/util/json_util.h>
#include <core/oc/ollama_client.h>

namespace vdb {

namespace {

double now_seconds() {
  using namespace std::chrono;
  return duration<double>(system_clock::now().time_since_epoch()).count();
}

template<typename T>
T meta_or(const Metadata& meta, std::string_view key, T fallback) {
  const auto found = meta.find(key);
  if (found == meta.end()) return fallback;
  if (std::holds_alternative<T>(found->second)) {
    return std::get<T>(found->second);
  }
  // An int where a double is wanted: JSON writes 1 for 1.0, so a memory stored
  // with importance exactly 1 reads back as an integer.
  if constexpr (std::is_same_v<T, double>) {
    if (std::holds_alternative<int64_t>(found->second)) {
      return static_cast<double>(std::get<int64_t>(found->second));
    }
  }
  return fallback;
}

// A bounded odd multiplier mix (splitmix64's finalizer): cheap, and it spreads
// adjacent token hashes across the whole width instead of clustering them.
uint64_t mix64(uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

std::vector<std::string> tokenize_lowercase(std::string_view text) {
  std::vector<std::string> tokens;
  std::string current;
  for (const char c : text) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u)) {
      current.push_back(static_cast<char>(std::tolower(u)));
      continue;
    }
    if (not current.empty()) {
      tokens.push_back(std::move(current));
      current.clear();
    }
  }
  if (not current.empty()) tokens.push_back(std::move(current));
  return tokens;
}

}  // namespace

Schema memory_schema() {
  Schema schema;
  schema.add_field(kMemoryFieldType, FieldType::String);
  schema.add_field(kMemoryFieldContext, FieldType::String);
  schema.add_field(kMemoryFieldTimestamp, FieldType::Float);
  schema.add_field(kMemoryFieldImportance, FieldType::Float);
  schema.add_field(kMemoryFieldAccessCount, FieldType::Int);
  schema.add_field(kMemoryFieldTags, FieldType::StringArray);
  return schema;
}

// ───────────────────────────────── embedders ───────────────────────────────

Embedder ollama_embedder(std::string model) {
  return [model = std::move(model)](std::string_view text, std::string& error) {
    oc::OllamaClient& client = oc::OllamaClient::instance();
    const uint64_t ticket = client.enqueue_embed({std::string(text)}, model);
    const oc::EmbedResult embedded = client.wait_for_embed(ticket);
    if (not embedded.ok) {
      error = embedded.error.empty() ? "the embedding request failed"
                                     : embedded.error;
      return std::vector<float>();
    }
    if (embedded.embeddings.empty() or embedded.embeddings.front().empty()) {
      error = "model '" + model + "' returned no embedding";
      return std::vector<float>();
    }
    const std::vector<double>& row = embedded.embeddings.front();
    std::vector<float> out;
    out.reserve(row.size());
    for (const double value : row) out.push_back(static_cast<float>(value));
    return out;
  };
}

Embedder hash_embedder(uint32_t dim) {
  return [dim](std::string_view text, std::string& error) {
    if (dim == 0) {
      error = "hash embedder was built with a dimension of 0";
      return std::vector<float>();
    }
    std::vector<float> out(dim, 0.0f);
    const std::vector<std::string> tokens = tokenize_lowercase(text);
    for (const std::string& token : tokens) {
      uint64_t hash = 1469598103934665603ull;
      for (const char c : token) {
        hash = (hash ^ static_cast<unsigned char>(c)) * 1099511628211ull;
      }
      // Four dimensions per token rather than one: a single spike makes two
      // texts sharing no tokens exactly orthogonal, which turns every ranking
      // into a coin flip on the tail.
      for (int probe = 0; probe < 4; ++probe) {
        const uint64_t mixed = mix64(hash + static_cast<uint64_t>(probe));
        const uint32_t slot = static_cast<uint32_t>(mixed % dim);
        out[slot] += (mixed & 0x10000ull) ? 1.0f : -1.0f;
      }
    }
    // An empty or punctuation-only text has no tokens; leave it as zeros and
    // let the store's normalize() leave it alone rather than produce NaNs.
    return out;
  };
}

// ──────────────────────────────── MemoryStore ──────────────────────────────

Metadata MemoryStore::metadata_of(const Memory& memory) {
  Metadata meta;
  meta[kMemoryFieldType] = memory.memory_type;
  meta[kMemoryFieldContext] = memory.context_id;
  meta[kMemoryFieldTimestamp] = memory.timestamp;
  meta[kMemoryFieldImportance] = memory.importance;
  meta[kMemoryFieldAccessCount] = memory.access_count;
  meta[kMemoryFieldTags] = memory.tags;
  return meta;
}

Memory MemoryStore::memory_of(const Document& document) {
  Memory memory;
  memory.id = document.id;
  memory.content = document.content;
  memory.memory_type =
      meta_or<std::string>(document.metadata, kMemoryFieldType, kMemoryEpisodic);
  memory.context_id =
      meta_or<std::string>(document.metadata, kMemoryFieldContext, "default");
  memory.timestamp = meta_or<double>(document.metadata, kMemoryFieldTimestamp, 0.0);
  memory.importance =
      meta_or<double>(document.metadata, kMemoryFieldImportance, 0.5);
  memory.access_count =
      meta_or<int64_t>(document.metadata, kMemoryFieldAccessCount, 0);
  memory.tags = meta_or<std::vector<std::string>>(document.metadata,
                                                  kMemoryFieldTags, {});
  return memory;
}

std::string memory_model_mismatch(const std::string& path,
                                  const std::string& model) {
  const StoreProbe probe = probe_vector_store(path);
  // Nothing there yet, or a file from before the model name was stamped into
  // the header: there is nothing to disagree with.
  if (not probe.exists or probe.source.empty() or model.empty()) return {};
  if (probe.source == model) return {};

  return path + " was built with '" + probe.source + "' (" +
         std::to_string(probe.dim) +
         "-dimensional) but the configured embed model is '" + model +
         "'; their vectors are not comparable even at the same width. Delete "
         "the file to rebuild it, or set the embed model back to '" +
         probe.source + "'.";
}

MemoryOpenResult MemoryStore::open(const MemoryOptions& options) {
  MemoryOpenResult result;
  std::unique_ptr<MemoryStore> store(new MemoryStore());
  store->mOptions = options;
  store->mEmbedder = options.embedder
                         ? options.embedder
                         : ollama_embedder(options.embed_model);

  const std::string mismatch =
      memory_model_mismatch(options.path, options.embed_model);
  if (not mismatch.empty()) {
    result.error = "memory: " + mismatch;
    return result;
  }

  const StoreProbe probe = probe_vector_store(options.path);
  uint32_t dim = options.embedding_dim;
  if (probe.exists) {
    if (dim != 0 and dim != probe.dim) {
      result.error = "memory: " + options.path + " holds " +
                     std::to_string(probe.dim) +
                     "-dimensional vectors, but " + std::to_string(dim) +
                     " was configured";
      return result;
    }
    dim = probe.dim;
  }

  if (dim != 0) {
    const StoreResult opened = store->ensure_store_locked(dim);
    if (not opened.ok) {
      result.error = opened.error;
      return result;
    }
    result.warning = store->mOpenWarning;
  }

  result.ok = true;
  result.store = std::move(store);
  return result;
}

StoreResult MemoryStore::ensure_store_locked(uint32_t dim) {
  StoreResult result;
  if (mStore) {
    if (mStore->dim() != dim) {
      result.error = "memory: the embedding model returned " +
                     std::to_string(dim) + " dimensions but " + mOptions.path +
                     " holds " + std::to_string(mStore->dim());
      return result;
    }
    result.ok = true;
    return result;
  }

  StoreOptions store_options = mOptions.store;
  store_options.dim = dim;
  store_options.metric = Metric::Cosine;
  // Stamped into the header so a later run using a different model of the same
  // width is at least noticed; the vectors would be silently incomparable.
  store_options.source = mOptions.embed_model;

  StoreOpenResult opened =
      VectorStore::open(mOptions.path, memory_schema(), store_options);
  if (not opened.ok) {
    result.error = opened.error;
    return result;
  }
  mOpenWarning = opened.warning;
  mStore = std::move(opened.store);
  result.ok = true;
  return result;
}

RememberResult MemoryStore::remember(const Memory& memory) {
  RememberResult result;
  if (memory.content.empty()) {
    result.error = "memory: a memory needs some content";
    return result;
  }
  if (memory.content.size() > mOptions.max_content_bytes) {
    result.error = "memory: content is " +
                   std::to_string(memory.content.size()) +
                   " bytes, over the " +
                   std::to_string(mOptions.max_content_bytes) + " byte limit";
    return result;
  }

  // Embedding happens before the lock is taken: it is the slow part by orders
  // of magnitude, and holding the store's lock across it would serialize every
  // concurrent subagent behind one model call.
  std::string error;
  const std::vector<float> vector = mEmbedder(memory.content, error);
  if (vector.empty()) {
    result.error = "memory: embedding failed: " +
                   (error.empty() ? std::string("no vector returned") : error);
    return result;
  }

  std::lock_guard<std::mutex> lock(mMutex);
  const StoreResult ready =
      ensure_store_locked(static_cast<uint32_t>(vector.size()));
  if (not ready.ok) {
    result.error = ready.error;
    return result;
  }

  Memory stamped = memory;
  if (stamped.timestamp == 0.0) stamped.timestamp = now_seconds();
  const AddResult added =
      mStore->add({stamped.content}, {metadata_of(stamped)}, {vector});
  if (not added.ok) {
    result.error = added.error;
    return result;
  }
  result.ok = true;
  result.id = added.ids.front();
  return result;
}

std::optional<Filter> MemoryStore::build_filter(const RecallQuery& query,
                                                std::string& error) const {
  // The structured fields and the raw DSL are both turned into JSON and handed
  // to one parser, rather than building a FilterNode tree by hand here. One
  // code path means the tool's `filters` argument and its convenience
  // arguments cannot drift apart in what they mean.
  util::JsonWriter writer;
  writer.begin_object();
  bool any = false;
  if (query.memory_type) {
    writer.field(kMemoryFieldType, *query.memory_type);
    any = true;
  }
  if (query.context_id) {
    writer.field(kMemoryFieldContext, *query.context_id);
    any = true;
  }
  if (query.min_importance) {
    writer.key(kMemoryFieldImportance)
        .begin_object()
        .field("$gte", *query.min_importance)
        .end_object();
    any = true;
  }
  writer.end_object();

  std::vector<Filter> parts;
  if (any) {
    const std::optional<Filter> structured = Filter::parse(writer.str(), error);
    if (not structured) return std::nullopt;
    parts.push_back(*structured);
  }
  for (const std::string& tag : query.tags) {
    util::JsonWriter tag_writer;
    tag_writer.begin_object()
        .key(kMemoryFieldTags)
        .begin_object()
        .field("$contains", tag)
        .end_object()
        .end_object();
    const std::optional<Filter> parsed = Filter::parse(tag_writer.str(), error);
    if (not parsed) return std::nullopt;
    parts.push_back(*parsed);
  }
  if (not query.filters.empty()) {
    const std::optional<Filter> raw = Filter::parse(query.filters, error);
    if (not raw) return std::nullopt;
    parts.push_back(*raw);
  }

  if (parts.empty()) return Filter::match_all();
  if (parts.size() == 1) return parts.front();

  Filter combined;
  combined.root.op = FilterOp::And;
  for (Filter& part : parts) {
    combined.root.children.push_back(std::move(part.root));
  }
  return combined;
}

RecallResult MemoryStore::recall(const RecallQuery& query) {
  RecallResult result;
  if (query.query.empty()) {
    result.error = "memory: a recall needs a query";
    return result;
  }

  std::string filter_error;
  const std::optional<Filter> filter = build_filter(query, filter_error);
  if (not filter) {
    result.error = "memory: filter parse error: " + filter_error;
    return result;
  }

  {
    std::lock_guard<std::mutex> lock(mMutex);
    // No file yet means nothing has ever been remembered. That is an empty
    // answer, not an error, and it must not force the file into existence.
    if (not mStore) {
      result.ok = true;
      return result;
    }
  }

  std::string embed_error;
  const std::vector<float> vector = mEmbedder(query.query, embed_error);
  if (vector.empty()) {
    result.error = "memory: embedding failed: " +
                   (embed_error.empty() ? std::string("no vector returned")
                                        : embed_error);
    return result;
  }

  const double recency_weight =
      query.recency_weight.value_or(mOptions.recency_weight);
  const double importance_weight =
      query.importance_weight.value_or(mOptions.importance_weight);
  const double similarity_weight =
      std::max(0.0, 1.0 - recency_weight - importance_weight);

  std::vector<ScoredMemory> scored;
  std::vector<uint64_t> to_bump;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mStore->dim() != vector.size()) {
      result.error = "memory: the embedding model returned " +
                     std::to_string(vector.size()) + " dimensions but " +
                     mOptions.path + " holds " + std::to_string(mStore->dim());
      return result;
    }

    const size_t fetch = std::max(query.k * mOptions.recall_oversample,
                                  mOptions.recall_oversample_min);
    const SearchResult found = mStore->search(vector, fetch, *filter);
    if (not found.ok) {
      result.error = "memory: " + found.error;
      return result;
    }

    const double now = now_seconds();
    scored.reserve(found.hits.size());
    for (const ScoredDoc& hit : found.hits) {
      ScoredMemory entry;
      entry.memory = memory_of(hit.document);
      entry.similarity = hit.score;
      const double age_hours =
          std::max(0.0, (now - entry.memory.timestamp) / 3600.0);
      entry.recency =
          std::exp(-age_hours / std::max(1e-9, mOptions.recency_half_life_hours));
      entry.score = similarity_weight * static_cast<double>(entry.similarity) +
                    recency_weight * entry.recency +
                    importance_weight * entry.memory.importance;
      scored.push_back(std::move(entry));
    }

    std::stable_sort(scored.begin(), scored.end(),
                     [](const ScoredMemory& a, const ScoredMemory& b) {
                       return a.score > b.score;
                     });
    if (scored.size() > query.k) scored.resize(query.k);

    for (ScoredMemory& entry : scored) {
      ++entry.memory.access_count;
      Metadata bump;
      bump[kMemoryFieldAccessCount] = entry.memory.access_count;
      // A failed bump is not worth failing the recall over: the caller asked
      // for memories, and it has them.
      (void)mStore->update_metadata(entry.memory.id, bump);
    }
  }

  result.ok = true;
  result.memories = std::move(scored);
  return result;
}

StoreResult MemoryStore::forget(const std::vector<uint64_t>& ids) {
  std::lock_guard<std::mutex> lock(mMutex);
  StoreResult result;
  if (not mStore) {
    result.ok = true;
    return result;
  }
  return mStore->remove(ids);
}

StoreResult MemoryStore::flush() {
  std::lock_guard<std::mutex> lock(mMutex);
  StoreResult result;
  if (not mStore) {
    result.ok = true;
    return result;
  }
  return mStore->flush();
}

MemoryStats MemoryStore::stats() const {
  std::lock_guard<std::mutex> lock(mMutex);
  MemoryStats out;
  out.path = mOptions.path;
  if (not mStore) return out;
  out.total = mStore->doc_count();
  out.dim = mStore->dim();
  out.file_bytes = mStore->file_size();
  for (const Document& document :
       mStore->get_where(Filter::match_all(), out.total)) {
    const std::string type =
        meta_or<std::string>(document.metadata, kMemoryFieldType, "");
    if (type == kMemoryEpisodic) {
      ++out.episodic;
    } else if (type == kMemorySemantic) {
      ++out.semantic;
    } else if (type == kMemoryProcedural) {
      ++out.procedural;
    } else {
      ++out.other;
    }
  }
  return out;
}

}  // namespace vdb
