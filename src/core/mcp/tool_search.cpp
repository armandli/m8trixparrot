#include <core/mcp/tool_search.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include <core/tools/tools_util.h>

namespace mcp {

namespace {

using util::JsonValue;

constexpr double kK1 = 1.2;
constexpr double kB = 0.75;

// Field weights: a word in the tool's name says far more about what it does
// than the same word somewhere in a parameter's description.
constexpr double kWeightName = 3.0;
constexpr double kWeightTitle = 2.0;
constexpr double kWeightParamName = 2.0;
constexpr double kWeightServer = 1.5;
constexpr double kWeightDescription = 1.0;
constexpr double kWeightParamDescription = 0.5;
constexpr double kWeightEnum = 0.5;

bool is_stopword(std::string_view word) {
  static const std::set<std::string, std::less<>> kStopwords = {
      "a",   "an",   "and",  "the",   "to",   "of",   "for", "in",
      "on",  "with", "by",   "from",  "is",   "it",   "or",  "this",
      "that", "use", "using", "be",   "as",   "at",   "are", "can",
      "will", "if",  "its",  "into",  "via",  "your", "you", "me",
      "my",  "i",    "do",   "please", "tool", "tools"};
  return kStopwords.count(word) != 0;
}

// Folds the plurals people and descriptions most often vary on, and nothing
// cleverer: a stemmer that is wrong is worse than none.
std::string stem(std::string word) {
  if (word.size() > 4 and word.compare(word.size() - 3, 3, "ies") == 0) {
    word.replace(word.size() - 3, 3, "y");
  } else if (word.size() > 4 and word.compare(word.size() - 4, 4, "sses") == 0) {
    word.resize(word.size() - 2);
  } else if (word.size() > 3 and word.back() == 's' and
             word[word.size() - 2] != 's' and word[word.size() - 2] != 'u' and
             word[word.size() - 2] != 'i') {
    word.pop_back();
  }
  return word;
}

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

// Raw words: alphanumeric runs, cut again at camel and digit boundaries.
std::vector<std::string> raw_words(std::string_view text) {
  std::vector<std::string> words;
  std::string current;
  const auto flush = [&] {
    if (not current.empty()) words.push_back(lower(current));
    current.clear();
  };
  for (size_t i = 0; i < text.size(); ++i) {
    const auto c = static_cast<unsigned char>(text[i]);
    // Non-ASCII bytes stay inside words, so a non-English word survives whole.
    if (not std::isalnum(c) and c < 0x80) {
      flush();
      continue;
    }
    if (not current.empty() and c < 0x80) {
      const auto prev = static_cast<unsigned char>(current.back());
      const bool next_lower = i + 1 < text.size() and
                              std::islower(static_cast<unsigned char>(text[i + 1]));
      const bool camel = std::islower(prev) and std::isupper(c);
      // The R in "HTTPResponse": an upper case letter followed by a lower case
      // one starts a word after a run of capitals.
      const bool acronym_end = std::isupper(prev) and std::isupper(c) and next_lower;
      const bool digit_edge = (std::isdigit(prev) != 0) != (std::isdigit(c) != 0);
      if (camel or acronym_end or digit_edge) flush();
    }
    current += static_cast<char>(c);
  }
  flush();
  return words;
}

void add_terms(std::unordered_map<std::string, double>& tf, double& length,
               const std::vector<std::string>& terms, double weight) {
  for (const std::string& term : terms) tf[term] += weight;
  length += weight * static_cast<double>(terms.size());
}

// "create_issue" -> "createissue": a model that writes the name without its
// separator still finds the tool.
std::string compact_alnum(std::string_view text) {
  std::string out;
  for (const char c : text) {
    if (std::isalnum(static_cast<unsigned char>(c))) {
      out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  return out;
}

void collect_parameters(const JsonValue& schema, int depth,
                        std::vector<std::string>& names,
                        std::vector<std::string>& descriptions,
                        std::vector<std::string>& enums) {
  if (depth > 2) return;
  for (const JsonValue::Member& property : schema.get("properties").members()) {
    for (std::string& term : search_terms(property.key)) names.push_back(term);
    for (std::string& term :
         search_terms(property.value.get("description").as_string())) {
      descriptions.push_back(term);
    }
    for (const JsonValue& value : property.value.get("enum").items()) {
      if (not value.is_string()) continue;
      for (std::string& term : search_terms(value.as_string())) {
        enums.push_back(term);
      }
    }
    collect_parameters(property.value, depth + 1, names, descriptions, enums);
    collect_parameters(property.value.get("items"), depth + 1, names,
                       descriptions, enums);
  }
}

std::string first_sentence(const std::string& text, size_t limit = 160) {
  size_t end = text.find_first_of("\n");
  for (size_t at = text.find(". "); at != std::string::npos;
       at = text.find(". ", at + 1)) {
    end = std::min(end, at + 1);
    break;
  }
  std::string out = text.substr(0, end);
  if (out.size() > limit) {
    size_t cut = limit;
    while (cut > 0 and (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) {
      --cut;
    }
    out = out.substr(0, cut) + "...";
  }
  return out;
}

}  // namespace

std::vector<std::string> search_terms(std::string_view text) {
  std::vector<std::string> out;
  for (std::string& word : raw_words(text)) {
    if (word.size() < 2 and not std::isdigit(static_cast<unsigned char>(word[0]))) {
      continue;
    }
    if (is_stopword(word)) continue;
    out.push_back(stem(std::move(word)));
  }
  return out;
}

ParsedQuery parse_query(std::string_view query) {
  ParsedQuery parsed;
  std::string keywords;

  size_t at = 0;
  while (at < query.size()) {
    while (at < query.size() and std::isspace(static_cast<unsigned char>(query[at]))) {
      ++at;
    }
    size_t end = at;
    while (end < query.size() and
           not std::isspace(static_cast<unsigned char>(query[end]))) {
      ++end;
    }
    const std::string_view word = query.substr(at, end - at);
    at = end;
    if (word.empty()) continue;

    if (word.rfind("select:", 0) == 0) {
      // select:a,b — also tolerate spaces after the commas ("select:a, b").
      std::string_view rest = word.substr(7);
      std::string list(rest);
      while (not list.empty() and list.back() == ',' and at < query.size()) {
        while (at < query.size() and
               std::isspace(static_cast<unsigned char>(query[at]))) {
          ++at;
        }
        size_t next = at;
        while (next < query.size() and
               not std::isspace(static_cast<unsigned char>(query[next]))) {
          ++next;
        }
        list += std::string(query.substr(at, next - at));
        at = next;
      }
      size_t start = 0;
      while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        std::string name = list.substr(start, comma == std::string::npos
                                                  ? std::string::npos
                                                  : comma - start);
        name.erase(0, name.find_first_not_of(" \t"));
        while (not name.empty() and std::isspace(static_cast<unsigned char>(name.back()))) {
          name.pop_back();
        }
        if (not name.empty()) parsed.select.push_back(name);
        if (comma == std::string::npos) break;
        start = comma + 1;
      }
    } else if (word.rfind("server:", 0) == 0) {
      parsed.server = std::string(word.substr(7));
    } else if (word.size() > 1 and word.front() == '+') {
      for (std::string& term : search_terms(word.substr(1))) {
        parsed.required.push_back(term);
        parsed.terms.push_back(term);  // a required word also ranks
      }
    } else {
      if (not keywords.empty()) keywords += ' ';
      keywords += word;
    }
  }
  for (std::string& term : search_terms(keywords)) parsed.terms.push_back(term);
  parsed.keywords = keywords;
  return parsed;
}

std::shared_ptr<const SearchIndex> SearchIndex::build(
    const std::vector<CatalogTool>& tools) {
  auto index = std::make_shared<SearchIndex>();
  double total_length = 0.0;
  for (const CatalogTool& tool : tools) {
    Doc doc;
    std::vector<std::string> name = search_terms(tool.name);
    const std::string compact = compact_alnum(tool.name);
    if (name.size() > 1 and not compact.empty()) name.push_back(compact);
    add_terms(doc.tf, doc.length, name, kWeightName);
    add_terms(doc.tf, doc.length, search_terms(tool.title), kWeightTitle);
    const std::vector<std::string> server = search_terms(tool.server);
    add_terms(doc.tf, doc.length, server, kWeightServer);
    add_terms(doc.tf, doc.length, search_terms(tool.description),
              kWeightDescription);

    std::vector<std::string> parameter_names;
    std::vector<std::string> parameter_descriptions;
    std::vector<std::string> enums;
    collect_parameters(tool.parameters, 0, parameter_names,
                       parameter_descriptions, enums);
    add_terms(doc.tf, doc.length, parameter_names, kWeightParamName);
    add_terms(doc.tf, doc.length, parameter_descriptions,
              kWeightParamDescription);
    add_terms(doc.tf, doc.length, enums, kWeightEnum);

    doc.name_terms = name;
    doc.name_terms.insert(doc.name_terms.end(), server.begin(), server.end());
    doc.compact_name = compact;

    for (const auto& [term, weight] : doc.tf) {
      (void)weight;
      ++index->mDocFreq[term];
    }
    total_length += doc.length;
    index->mDocs.push_back(std::move(doc));
  }
  if (not index->mDocs.empty()) {
    index->mAverageLength =
        std::max(1.0, total_length / static_cast<double>(index->mDocs.size()));
  }
  return index;
}

std::vector<SearchHit> SearchIndex::score(
    const std::vector<std::string>& terms, const ParsedQuery& query,
    const std::vector<CatalogTool>& tools) const {
  const double n = static_cast<double>(mDocs.size());
  const std::string compact_query = compact_alnum(query.keywords);

  std::vector<SearchHit> hits;
  for (size_t i = 0; i < mDocs.size() and i < tools.size(); ++i) {
    const Doc& doc = mDocs[i];
    const CatalogTool& tool = tools[i];

    if (not query.server.empty() and
        lower(tool.server) != lower(query.server) and
        compact_alnum(tool.server) != compact_alnum(query.server)) {
      continue;
    }
    bool has_required = true;
    for (const std::string& word : query.required) {
      has_required = has_required and
                     std::find(doc.name_terms.begin(), doc.name_terms.end(),
                               word) != doc.name_terms.end();
    }
    if (not has_required) continue;

    double total = 0.0;
    for (const std::string& term : terms) {
      const auto tf_it = doc.tf.find(term);
      if (tf_it == doc.tf.end()) continue;
      const auto df_it = mDocFreq.find(term);
      const double df = df_it == mDocFreq.end() ? 0.0 : static_cast<double>(df_it->second);
      const double idf = std::log(1.0 + (n - df + 0.5) / (df + 0.5));
      const double tf = tf_it->second;
      total += idf * tf * (kK1 + 1.0) /
               (tf + kK1 * (1.0 - kB + kB * doc.length / mAverageLength));
    }
    // Asked for by name: that tool, ahead of anything that merely mentions it.
    if (not compact_query.empty() and compact_query == doc.compact_name) {
      total += 100.0;
    }
    // server:x with no keywords lists that server's tools.
    if (terms.empty() and not query.server.empty()) total = 1.0;
    if (total > 0.0) hits.push_back(SearchHit{i, total});
  }
  std::stable_sort(hits.begin(), hits.end(),
                   [](const SearchHit& a, const SearchHit& b) {
                     return a.score > b.score;
                   });
  return hits;
}

std::vector<SearchHit> SearchIndex::search(
    const ParsedQuery& query, const std::vector<CatalogTool>& tools) const {
  std::vector<SearchHit> hits = score(query.terms, query, tools);
  if (not hits.empty()) return hits;

  // Nothing matched whole terms: try them as prefixes ("auth" -> "authorize").
  std::vector<std::string> expanded;
  for (const std::string& term : query.terms) {
    if (term.size() < 3) continue;
    for (const auto& [word, df] : mDocFreq) {
      (void)df;
      if (word.size() > term.size() and word.compare(0, term.size(), term) == 0) {
        expanded.push_back(word);
      }
    }
  }
  if (expanded.empty()) return hits;
  return score(expanded, query, tools);
}

NameResolution resolve_tool_name(const Catalog& catalog, std::string_view name) {
  NameResolution resolution;
  if (name.empty()) return resolution;

  if (const CatalogTool* tool = catalog.find(name)) {
    resolution.tool = tool;
    return resolution;
  }

  // server<sep>tool, with any of the separators a model might invent.
  for (const std::string_view separator : {"__", ".", "/", ":"}) {
    const size_t at = name.find(separator);
    if (at == std::string_view::npos or at == 0) continue;
    const std::string_view server = name.substr(0, at);
    const std::string_view tool = name.substr(at + separator.size());
    for (const CatalogTool& candidate : catalog.tools) {
      if (candidate.server == server and candidate.name == tool) {
        resolution.tool = &candidate;
        return resolution;
      }
    }
  }

  const auto unique = [&](auto matches) -> bool {
    std::vector<const CatalogTool*> found;
    for (const CatalogTool& tool : catalog.tools) {
      if (matches(tool)) found.push_back(&tool);
    }
    if (found.size() == 1) {
      resolution.tool = found.front();
      return true;
    }
    if (found.size() > 1) {
      for (const CatalogTool* tool : found) {
        resolution.candidates.push_back(tool->exposed);
      }
      return true;
    }
    return false;
  };

  const std::string wanted(name);
  if (unique([&](const CatalogTool& tool) { return tool.name == wanted; })) {
    return resolution;
  }
  const std::string folded = lower(name);
  if (unique([&](const CatalogTool& tool) {
        return lower(tool.name) == folded or lower(tool.exposed) == folded;
      })) {
    return resolution;
  }
  const std::string compact = compact_alnum(name);
  unique([&](const CatalogTool& tool) {
    return compact_alnum(tool.name) == compact or
           compact_alnum(tool.exposed) == compact;
  });
  return resolution;
}

std::string ToolSearchTool::description() {
  return R"json({"name":"tool_search","description":"Find and load MCP tools (tools from the MCP servers listed in your instructions). Most of them are not loaded, to save context. Search with keywords for what you need (e.g. \"create github issue\"), or load tools by exact name with \"select:<name>[,<name>...]\". Matching tools are loaded and become callable on your next step. Add \"server:<name>\" to search one server, and \"+word\" to require a word in the tool's name.","parameters":{"type":"object","properties":{"query":{"type":"string","description":"Keywords describing the capability you need, or select:<tool>[,<tool>...]. May include server:<name> and +word."},"limit":{"type":"integer","description":"Most tools a keyword search loads (default 5, max 10)."}},"required":["query"]}})json";
}

tools::ToolResult ToolSearchTool::execute(const tools::ToolArgs& args) const {
  tools::ToolResult result;
  const std::optional<std::string> query = tools::string_arg(args, "query");
  if (not query or query->find_first_not_of(" \t\n") == std::string::npos) {
    result.error =
        "tool_search needs a query: keywords for what you need, or "
        "select:<tool name> to load a tool you saw listed";
    return result;
  }
  const int64_t requested = tools::int_arg(args, "limit").value_or(kDefaultLimit);
  const size_t limit = static_cast<size_t>(
      std::clamp<int64_t>(requested, 1, static_cast<int64_t>(kMaxLimit)));

  if (catalog.tools.empty()) {
    result.ok = true;
    result.output = "No MCP tools are available right now";
    std::string states;
    for (const CatalogServer& server : catalog.servers) {
      if (not states.empty()) states += ", ";
      states += server.name + " (" + state_name(server.state) + ")";
    }
    result.output += states.empty() ? "." : ": " + states + ".";
    return result;
  }

  const ParsedQuery parsed = parse_query(*query);
  std::vector<const CatalogTool*> newly_loaded;
  std::vector<const CatalogTool*> already;
  std::vector<std::string> problems;
  std::vector<std::string> also_matched;

  const auto load = [&](const CatalogTool& tool) {
    if (loaded.add(tool.exposed)) {
      newly_loaded.push_back(&tool);
    } else {
      already.push_back(&tool);
    }
  };

  if (not parsed.select.empty()) {
    size_t taken = 0;
    for (const std::string& name : parsed.select) {
      if (++taken > kMaxSelect) {
        problems.push_back("only the first " + std::to_string(kMaxSelect) +
                           " names in select: were loaded");
        break;
      }
      const NameResolution found = resolve_tool_name(catalog, name);
      if (found.tool != nullptr) {
        load(*found.tool);
        continue;
      }
      if (not found.candidates.empty()) {
        std::string list;
        for (const std::string& candidate : found.candidates) {
          list += (list.empty() ? "" : ", ") + candidate;
        }
        problems.push_back("'" + name + "' is ambiguous: " + list);
        continue;
      }
      // Suggest, never guess: loading the wrong tool silently is worse.
      ParsedQuery fallback;
      fallback.terms = search_terms(name);
      std::string suggestions;
      if (catalog.index) {
        const std::vector<SearchHit> hits = catalog.index->search(fallback, catalog.tools);
        for (size_t i = 0; i < hits.size() and i < 3; ++i) {
          suggestions += (suggestions.empty() ? "" : ", ") +
                         catalog.tools[hits[i].tool].exposed;
        }
      }
      problems.push_back("no MCP tool named '" + name + "'" +
                         (suggestions.empty() ? std::string()
                                              : "; closest: " + suggestions));
    }
  } else {
    std::vector<SearchHit> hits;
    if (catalog.index) hits = catalog.index->search(parsed, catalog.tools);
    if (hits.empty()) {
      result.ok = true;
      std::string servers;
      for (const CatalogServer& server : catalog.servers) {
        if (server.tool_count == 0) continue;
        servers += (servers.empty() ? "" : ", ") + server.name + " (" +
                   std::to_string(server.tool_count) + " tools)";
      }
      result.output = "No MCP tools matched \"" + *query +
                      "\". Servers with tools: " + servers +
                      ". Try other keywords, server:<name> to list one "
                      "server's tools, or select:<name> for a name you saw.";
      return result;
    }
    const double best = hits.front().score;
    for (const SearchHit& hit : hits) {
      const CatalogTool& tool = catalog.tools[hit.tool];
      const bool strong = hit.score >= best * kRelativeCutoff;
      if (strong and newly_loaded.size() + already.size() < limit) {
        load(tool);
      } else if (also_matched.size() < 5) {
        also_matched.push_back(tool.exposed);
      }
    }
  }

  std::string out;
  if (not newly_loaded.empty()) {
    out += "Loaded " + std::to_string(newly_loaded.size()) +
           (newly_loaded.size() == 1 ? " tool" : " tools") +
           ", callable from your next step:\n";
    for (const CatalogTool* tool : newly_loaded) {
      out += "- " + tool->signature;
      const std::string summary = first_sentence(tool->description);
      if (not summary.empty()) out += " — " + summary;
      out += "\n";
    }
  }
  if (not already.empty()) {
    out += "Already loaded:";
    for (const CatalogTool* tool : already) out += " " + tool->exposed;
    out += "\n";
  }
  if (not also_matched.empty()) {
    out += "Also matched (not loaded; use select:<name> to load):";
    for (const std::string& name : also_matched) out += " " + name;
    out += "\n";
  }
  for (const std::string& problem : problems) out += problem + "\n";

  if (newly_loaded.empty() and already.empty()) {
    result.error = out.empty() ? "nothing was loaded" : out;
    return result;
  }
  result.ok = true;
  result.output = out;
  return result;
}

}  // namespace mcp
