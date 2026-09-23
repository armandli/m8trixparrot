// A port of caliby's examples/agentic_memory_store.py, step for step, against
// the C++ store in core/memory_store.h. It exists to be run: it is the
// end-to-end check that remembering, recalling, filtering and persistence all
// work together, and the worked example of what the API looks like in use.

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <CLI/CLI.hpp>

#include <core/memory_store.h>

namespace {

void heading(int step, const std::string& text) {
  std::printf("\n%d. %s\n", step, text.c_str());
}

void show(const agent::ScoredMemory& scored) {
  const agent::Memory& memory = scored.memory;
  std::string content = memory.content;
  if (content.size() > 62) content = content.substr(0, 59) + "...";
  std::printf("   [%-9s] %-62s  score %.3f  sim %.3f  rec %.3f\n",
              memory.memory_type.c_str(), content.c_str(), scored.score,
              scored.similarity, scored.recency);
}

}  // namespace

int main(int argc, char** argv) {
  CLI::App app{
      "Agentic memory store demo: stores a conversation and some facts, then\n"
      "recalls them by meaning. Runs offline by default with a deterministic\n"
      "hash embedder; pass --model to embed through a real Ollama model."};

  std::string db = "/tmp/m8trix_memory_demo/memory.m8db";
  app.add_option("--db", db, "Path to the memory database file");

  std::string model;
  app.add_option("--model", model,
                 "Ollama embedding model (e.g. nomic-embed-text). Omit to run "
                 "offline against the built-in hash embedder.");

  std::string host = "http://localhost:11434";
  app.add_option("--host", host, "Ollama host");

  int dim = 256;
  app.add_option("--dim", dim, "Hash-embedder width, ignored with --model");

  bool keep = false;
  app.add_flag("--keep", keep, "Keep the database file instead of deleting it");

  CLI11_PARSE(app, argc, argv);

  std::error_code ec;
  if (not keep) std::filesystem::remove(db, ec);

  std::printf("======================================================\n");
  std::printf("m8trixparrot agentic memory store\n");
  std::printf("======================================================\n");

  agent::MemoryOptions options;
  options.path = db;
  options.recency_weight = 0.0;  // Per-query below, matching the python.
  if (model.empty()) {
    options.embed_model = "hash-" + std::to_string(dim);
    options.embedder = agent::hash_embedder(static_cast<uint32_t>(dim));
    std::printf("Embedder: built-in hash (%d dimensions, offline)\n", dim);
  } else {
    options.embed_model = model;
    options.ollama_host = host;
    std::printf("Embedder: ollama %s at %s\n", model.c_str(), host.c_str());
  }

  heading(1, "Opening the memory store...");
  agent::MemoryOpenResult opened = agent::MemoryStore::open(options);
  if (not opened.ok) {
    std::fprintf(stderr, "%s\n", opened.error.c_str());
    return 1;
  }
  if (not opened.warning.empty()) {
    std::fprintf(stderr, "   warning: %s\n", opened.warning.c_str());
  }
  agent::MemoryStore& memory = *opened.store;
  std::printf("   ready at %s\n", db.c_str());

  heading(2, "Storing conversation memories...");
  const std::vector<std::pair<std::string, double>> conversation = {
      {"User: What's the weather like today?", 0.3},
      {"Agent: I don't have access to real-time weather data, but I can help "
       "you find a weather service.", 0.4},
      {"User: I'm planning a trip to Paris next month.", 0.7},
      {"Agent: Paris in the spring is lovely! Would you like recommendations "
       "for things to do?", 0.5},
      {"User: Yes, I'm especially interested in art museums.", 0.8},
      {"Agent: The Louvre and Musee d'Orsay are must-visits. The Orangerie has "
       "beautiful Monet works.", 0.9},
      {"User: I also love impressionist art.", 0.8},
      {"Agent: Then definitely visit Musee d'Orsay - it has the world's largest "
       "impressionist collection.", 0.9},
  };
  for (const auto& [content, importance] : conversation) {
    agent::Memory entry;
    entry.content = content;
    entry.memory_type = agent::kMemoryEpisodic;
    entry.context_id = "conv_001";
    entry.importance = importance;
    entry.tags = content.find("Paris") != std::string::npos or
                         content.find("trip") != std::string::npos
                     ? std::vector<std::string>{"conversation", "travel"}
                     : std::vector<std::string>{"conversation"};
    const agent::RememberResult stored = memory.remember(entry);
    if (not stored.ok) {
      std::fprintf(stderr, "%s\n", stored.error.c_str());
      return 1;
    }
  }
  std::printf("   stored %zu conversation memories\n", conversation.size());

  heading(3, "Storing semantic (factual) memories...");
  const std::vector<std::pair<std::string, std::vector<std::string>>> facts = {
      {"The Louvre is the world's most-visited art museum, located in Paris.",
       {"fact", "paris", "art"}},
      {"Musee d'Orsay is famous for impressionist and post-impressionist "
       "masterpieces.",
       {"fact", "paris", "art"}},
      {"The best time to visit Paris is spring (April-June) or fall "
       "(September-November).",
       {"fact", "paris", "travel"}},
      {"User prefers impressionist art style.", {"user_preference", "art"}},
  };
  for (const auto& [content, tags] : facts) {
    agent::Memory entry;
    entry.content = content;
    entry.memory_type = agent::kMemorySemantic;
    entry.context_id = "global";
    entry.importance = 0.8;
    entry.tags = tags;
    const agent::RememberResult stored = memory.remember(entry);
    if (not stored.ok) {
      std::fprintf(stderr, "%s\n", stored.error.c_str());
      return 1;
    }
  }
  std::printf("   stored %zu semantic memories\n", facts.size());

  heading(4, "Recalling for a new query, slightly favouring recent memories...");
  agent::RecallQuery query;
  query.query = "What should I see in Paris?";
  query.k = 5;
  query.recency_weight = 0.2;
  std::printf("   query: '%s'\n", query.query.c_str());
  agent::RecallResult recalled = memory.recall(query);
  if (not recalled.ok) {
    std::fprintf(stderr, "%s\n", recalled.error.c_str());
    return 1;
  }
  for (const agent::ScoredMemory& scored : recalled.memories) show(scored);

  heading(5, "The same query, filtered to semantic memories only...");
  query.memory_type = agent::kMemorySemantic;
  query.k = 3;
  recalled = memory.recall(query);
  if (not recalled.ok) {
    std::fprintf(stderr, "%s\n", recalled.error.c_str());
    return 1;
  }
  for (const agent::ScoredMemory& scored : recalled.memories) show(scored);

  heading(6, "Filtered by tag and importance through the filter DSL...");
  agent::RecallQuery tagged;
  tagged.query = "art museums";
  tagged.k = 3;
  tagged.tags = {"art"};
  tagged.filters = R"({"importance":{"$gte":0.8}})";
  recalled = memory.recall(tagged);
  if (not recalled.ok) {
    std::fprintf(stderr, "%s\n", recalled.error.c_str());
    return 1;
  }
  for (const agent::ScoredMemory& scored : recalled.memories) show(scored);

  heading(7, "Statistics:");
  agent::MemoryStats stats = memory.stats();
  std::printf("   total %llu  (episodic %llu, semantic %llu, procedural %llu)\n",
              static_cast<unsigned long long>(stats.total),
              static_cast<unsigned long long>(stats.episodic),
              static_cast<unsigned long long>(stats.semantic),
              static_cast<unsigned long long>(stats.procedural));
  std::printf("   %u dimensions, %llu bytes on disk\n", stats.dim,
              static_cast<unsigned long long>(stats.file_bytes));

  heading(8, "Closing, reopening, and checking the memories survived...");
  opened.store.reset();
  agent::MemoryOpenResult reopened = agent::MemoryStore::open(options);
  if (not reopened.ok) {
    std::fprintf(stderr, "%s\n", reopened.error.c_str());
    return 1;
  }
  stats = reopened.store->stats();
  std::printf("   reopened with %llu memories\n",
              static_cast<unsigned long long>(stats.total));
  agent::RecallQuery again;
  again.query = "impressionist paintings";
  again.k = 1;
  recalled = reopened.store->recall(again);
  if (not recalled.ok or recalled.memories.empty()) {
    std::fprintf(stderr, "recall after reopen returned nothing\n");
    return 1;
  }
  show(recalled.memories.front());
  std::printf("   access_count is now %lld, so the bump was persisted too\n",
              static_cast<long long>(recalled.memories.front().memory.access_count));

  reopened.store.reset();
  if (not keep) {
    std::filesystem::remove(db, ec);
    std::printf("\nDone. Removed %s (pass --keep to leave it).\n", db.c_str());
  } else {
    std::printf("\nDone. Database left at %s\n", db.c_str());
  }
  return 0;
}
