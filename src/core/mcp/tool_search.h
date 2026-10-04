#ifndef M8_MCP_TOOL_SEARCH_H
#define M8_MCP_TOOL_SEARCH_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <core/mcp/catalog.h>
#include <core/tools/tools.h>

namespace mcp {

// ---------------------------------------------------------------------------
// MCP tool search: progressive discovery, done client side.
//
// Ollama has no tool_reference blocks or server-side search, so deferral is
// all ours: deferred MCP tools are left out of the tools array, the model sees
// only their names (agent::mcp_block) and this tool, and whatever it finds is
// added to that agent's loaded set — the next model call carries those
// schemas. The query forms mirror Claude Code's ToolSearch so a model that
// knows one knows the other.
// ---------------------------------------------------------------------------

// When MCP tool schemas are deferred behind tool_search: the same values as
// Claude Code's ENABLE_TOOL_SEARCH.
//   auto     defer when the schemas would take more than min(10% of the
//            context window, 10k tokens), or there are more than 30 tools
//   auto:N   the same at N%
//   on       always defer (alias: true)
//   off      never defer (alias: false)
// The tool-count trigger is m8's own: local models choose tools noticeably
// worse past a few dozen, even when the schemas would fit.
struct ToolSearchSettings {
  enum struct Mode : uint8_t { Auto, On, Off };

  Mode mode = Mode::Auto;
  int threshold_pct = 10;

  static constexpr int64_t kMaxThresholdTokens = 10000;
  static constexpr int64_t kUnknownWindowTokens = 4000;
  static constexpr size_t kMaxUndeferredTools = 30;

  static std::optional<ToolSearchSettings> parse(std::string_view text);
  std::string text() const;

  // The schema budget, in tokens, above which `auto` defers.
  int64_t threshold_tokens(int64_t context_window) const;
  bool defers(int64_t schema_tokens, size_t tool_count,
              int64_t context_window) const;
};

// Lowercase search terms: split on anything that is not a letter or digit, on
// camelCase and acronym boundaries (getHTTPResponse -> get, http, response) and
// on letter/digit boundaries; stopwords dropped; plurals folded.
std::vector<std::string> search_terms(std::string_view text);

struct ParsedQuery {
  std::vector<std::string> select;    // select:a,b — exact names, in order
  std::vector<std::string> terms;     // keywords, already tokenized
  std::string keywords;               // the keyword words as typed
  std::vector<std::string> required;  // +word — must be in the tool's name
  std::string server;                 // server:<name> — one server only
};

ParsedQuery parse_query(std::string_view query);

struct SearchHit {
  size_t tool = 0;  // index into Catalog::tools
  double score = 0.0;
};

// BM25F over each tool's name, title, server, parameter names, description,
// parameter descriptions and enum values (in falling weight). Immutable once
// built; one per catalog snapshot.
struct SearchIndex {
  static std::shared_ptr<const SearchIndex> build(
      const std::vector<CatalogTool>& tools);

  // Best first, filtered by `server:` and `+word`, ties broken by catalog
  // order. With no keyword hits, retries once matching keyword prefixes.
  std::vector<SearchHit> search(const ParsedQuery& query,
                                const std::vector<CatalogTool>& tools) const;

private:
  struct Doc {
    std::unordered_map<std::string, double> tf;  // weighted term frequency
    double length = 0.0;
    std::vector<std::string> name_terms;    // name + server, for +word
    std::string compact_name;               // lowercase alnum original name
  };

  std::vector<SearchHit> score(const std::vector<std::string>& terms,
                               const ParsedQuery& query,
                               const std::vector<CatalogTool>& tools) const;

  std::vector<Doc> mDocs;
  std::unordered_map<std::string, size_t> mDocFreq;
  double mAverageLength = 1.0;
};

// What `select:<name>` resolves to. Tried in order: the exposed name; server
// and tool joined by `__`, `.`, `/` or `:`; the server's own tool name when
// only one server has it; the same again ignoring case; the sanitized
// spelling. Never fuzzy: an ambiguous name lists its candidates instead.
struct NameResolution {
  const CatalogTool* tool = nullptr;
  std::vector<std::string> candidates;  // exposed names, when ambiguous
};

NameResolution resolve_tool_name(const Catalog& catalog, std::string_view name);

// The `tool_search` tool. Constructed at the dispatch site with the turn's
// catalog snapshot and the calling agent's loaded set.
struct ToolSearchTool {
  const Catalog& catalog;
  LoadedTools& loaded;

  static constexpr size_t kDefaultLimit = 5;
  static constexpr size_t kMaxLimit = 10;
  static constexpr size_t kMaxSelect = 20;
  // Of a keyword search, only results scoring at least this fraction of the
  // best are loaded: a model asking for one thing should not get five.
  static constexpr double kRelativeCutoff = 0.25;

  static std::string description();
  // query (string, required), limit (integer, optional).
  tools::ToolResult execute(const tools::ToolArgs& args) const;
};

}  // namespace mcp

#endif  // M8_MCP_TOOL_SEARCH_H
