#include <core/tools.hpp>

#include <cstdint>

#include <algorithm>
#include <optional>
#include <string>

#include <core/tools_util.hpp>

namespace agent {

namespace {

// The schema names no default, so the stub picks one; whatever backend lands
// here should keep honoring it.
constexpr int64_t kDefaultResultLimit = 10;

}  // namespace

std::string WebSearchTool::description() const {
  return R"json({"name":"websearch","description":"Search the web for a given query and return matching results.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Search query string"},"limit":{"type":"number","description":"Maximum number of search results to return"}},"required":["query"]}})json";
}

ToolResult WebSearchTool::execute(const ToolArgs& args) const {
  ToolResult result;

  const std::optional<std::string> query = string_arg(args, "query");
  if (not query or query->empty()) {
    result.error = "websearch: missing required string argument 'query'";
    return result;
  }

  const int64_t limit =
      std::max<int64_t>(int_arg(args, "limit").value_or(kDefaultResultLimit), 1);

  // TODO: issue `query` against a search backend. The backend is undecided —
  // an API-key service and a scraped results page pull the rest of this
  // function in different directions, so nothing here assumes either.
  //
  // TODO: turn the response into a list of results, each with a title, a URL,
  // and a snippet.
  //
  // TODO: format that list for the model — numbered, one result per entry,
  // URLs kept intact so `webfetch` can follow them — honoring `limit`.
  //
  // TODO: run the formatted text through truncate_output(..., "websearch") and
  // append truncation_note(), the way every other output-producing tool does,
  // then set result.ok / result.truncated / result.overflow_path from it.

  // Until then, say so plainly rather than returning an empty result set: a
  // model that sees zero hits retries with a reworded query, while a model
  // told the tool doesn't work moves on.
  result.error = "websearch: web search is not implemented yet (asked for '" +
                 *query + "', limit " + std::to_string(limit) +
                 "); no search backend is configured";
  return result;
}

}  // namespace agent
