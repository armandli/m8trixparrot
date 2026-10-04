#include <core/mcp/catalog.h>

#include <algorithm>
#include <cctype>

#include <core/mcp/tool_search.h>
#include <core/util/sha256.h>
#include <core/util/text.h>

namespace mcp {

namespace {

constexpr size_t kMaxExposedName = 64;

std::string sanitize(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    const auto u = static_cast<unsigned char>(c);
    out += std::isalnum(u) ? c : '_';
  }
  return out;
}

std::string short_hash(std::string_view text) {
  return util::sha256_hex(text).substr(0, 6);
}

}  // namespace

const char* state_name(ServerState state) {
  switch (state) {
    case ServerState::Disabled:
      return "disabled";
    case ServerState::NeedsApproval:
      return "needs approval";
    case ServerState::Connecting:
      return "connecting";
    case ServerState::Connected:
      return "connected";
    case ServerState::NeedsAuth:
      return "needs login";
    case ServerState::Failed:
    default:
      return "failed";
  }
}

const CatalogTool* Catalog::find(std::string_view exposed) const {
  if (auto it = by_exposed.find(std::string(exposed)); it != by_exposed.end()) {
    return &tools[it->second];
  }
  return nullptr;
}

const CatalogServer* Catalog::server(std::string_view name) const {
  for (const CatalogServer& entry : servers) {
    if (entry.name == name) return &entry;
  }
  return nullptr;
}

std::vector<const CatalogTool*> Catalog::by_original_name(
    std::string_view name) const {
  std::vector<const CatalogTool*> out;
  for (const CatalogTool& tool : tools) {
    if (tool.name == name) out.push_back(&tool);
  }
  return out;
}

bool Catalog::any_connecting() const {
  return std::any_of(servers.begin(), servers.end(), [](const CatalogServer& s) {
    return s.state == ServerState::Connecting;
  });
}

size_t Catalog::connected_count() const {
  return static_cast<size_t>(
      std::count_if(servers.begin(), servers.end(), [](const CatalogServer& s) {
        return s.state == ServerState::Connected;
      }));
}

bool Catalog::any_resources() const {
  return std::any_of(servers.begin(), servers.end(), [](const CatalogServer& s) {
    return s.state == ServerState::Connected and s.capabilities.resources;
  });
}

bool is_mcp_tool_name(std::string_view name) {
  return name.rfind("mcp__", 0) == 0;
}

std::string server_name_problem(std::string_view name) {
  if (name.empty()) return "a server name cannot be empty";
  if (name.size() > 40) return "a server name is at most 40 characters";
  for (const char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (not std::isalnum(u) and c != '_' and c != '-') {
      return "a server name may only use letters, digits, '_' and '-'";
    }
  }
  if (name.find("__") != std::string_view::npos) {
    return "a server name cannot contain '__' (it separates the server from "
           "the tool in tool names)";
  }
  return std::string();
}

std::string exposed_tool_name(std::string_view server, std::string_view tool) {
  std::string name = "mcp__" + sanitize(server) + "__" + sanitize(tool);
  if (name.size() <= kMaxExposedName) return name;
  // Stable across refreshes: the suffix depends only on the original names.
  const std::string suffix =
      "_" + short_hash(std::string(server) + "/" + std::string(tool));
  name.resize(kMaxExposedName - suffix.size());
  return name + suffix;
}

bool make_catalog_tool(std::string_view server, const ToolInfo& tool,
                       bool always_load, bool http, CatalogTool& out,
                       std::string& problem) {
  out = CatalogTool{};
  out.server = std::string(server);
  out.name = tool.name;
  out.exposed = exposed_tool_name(server, tool.name);
  out.title = tool.title;
  out.description = util::sanitize_utf8(tool.description);
  out.input_schema = tool.input_schema;
  out.always_load = always_load;
  out.annotations = tool.annotations;

  if (http) {
    problem = collect_header_params(tool.input_schema, out.header_params);
    if (not problem.empty()) return false;
  }

  LoweredSchema lowered = lower_schema(tool.input_schema);
  if (std::string shape = ollama_shape_problem(lowered.parameters);
      not shape.empty()) {
    problem = "its input schema could not be made safe for Ollama (" + shape +
              ")";
    return false;
  }
  out.parameters = std::move(lowered.parameters);

  std::string description = out.description;
  if (description.empty()) description = out.title;
  if (description.empty()) description = "MCP tool " + tool.name + " from " +
                                          std::string(server) + ".";
  out.schema_json = tool_schema_json(out.exposed, description, out.parameters);
  out.signature = tool_signature(out.exposed, out.parameters);
  out.tokens = util::estimate_tokens(out.schema_json);
  return true;
}

std::shared_ptr<const Catalog> finalize_catalog(Catalog catalog) {
  catalog.by_exposed.clear();
  catalog.total_tokens = 0;
  for (size_t i = 0; i < catalog.tools.size(); ++i) {
    CatalogTool& tool = catalog.tools[i];
    if (catalog.by_exposed.count(tool.exposed) != 0) {
      // Two tools sanitized to one name ("a-b" and "a_b"): the later one gets a
      // suffix derived from its original names, so it is the same every time.
      const std::string suffix = "_" + short_hash(tool.server + "/" + tool.name);
      std::string renamed = tool.exposed;
      if (renamed.size() + suffix.size() > kMaxExposedName) {
        renamed.resize(kMaxExposedName - suffix.size());
      }
      tool.exposed = renamed + suffix;
      tool.schema_json = tool_schema_json(
          tool.exposed,
          tool.description.empty() ? tool.title : tool.description,
          tool.parameters);
      tool.signature = tool_signature(tool.exposed, tool.parameters);
      tool.tokens = util::estimate_tokens(tool.schema_json);
    }
    catalog.by_exposed[tool.exposed] = i;
    catalog.total_tokens += tool.tokens;
  }
  for (CatalogServer& server : catalog.servers) {
    server.tool_count = 0;
    server.schema_tokens = 0;
    for (const CatalogTool& tool : catalog.tools) {
      if (tool.server != server.name) continue;
      ++server.tool_count;
      server.schema_tokens += tool.tokens;
    }
  }
  catalog.index = SearchIndex::build(catalog.tools);
  return std::make_shared<const Catalog>(std::move(catalog));
}

// ───────────────────────────── LoadedTools ──────────────────────────────────

bool LoadedTools::add(const std::string& exposed) {
  std::lock_guard<std::mutex> lock(mMutex);
  mLastUse[exposed] = ++mClock;
  if (std::find(mNames.begin(), mNames.end(), exposed) != mNames.end()) {
    return false;
  }
  mNames.push_back(exposed);
  return true;
}

void LoadedTools::touch(const std::string& exposed) {
  std::lock_guard<std::mutex> lock(mMutex);
  if (std::find(mNames.begin(), mNames.end(), exposed) != mNames.end()) {
    mLastUse[exposed] = ++mClock;
  }
}

bool LoadedTools::contains(std::string_view exposed) const {
  std::lock_guard<std::mutex> lock(mMutex);
  return std::find(mNames.begin(), mNames.end(), exposed) != mNames.end();
}

std::vector<std::string> LoadedTools::names() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mNames;
}

size_t LoadedTools::size() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mNames.size();
}

void LoadedTools::clear() {
  std::lock_guard<std::mutex> lock(mMutex);
  mNames.clear();
  mLastUse.clear();
}

void LoadedTools::assign(const std::vector<std::string>& exposed) {
  std::lock_guard<std::mutex> lock(mMutex);
  mNames.clear();
  mLastUse.clear();
  for (const std::string& name : exposed) {
    if (std::find(mNames.begin(), mNames.end(), name) != mNames.end()) continue;
    mNames.push_back(name);
    mLastUse[name] = ++mClock;
  }
}

std::vector<std::string> LoadedTools::evict(size_t max_count, int64_t max_tokens,
                                            const Catalog& catalog) {
  std::lock_guard<std::mutex> lock(mMutex);
  std::vector<std::string> dropped;

  // Tools whose server went away cannot be offered any more.
  for (auto it = mNames.begin(); it != mNames.end();) {
    if (catalog.find(*it) == nullptr) {
      dropped.push_back(*it);
      mLastUse.erase(*it);
      it = mNames.erase(it);
    } else {
      ++it;
    }
  }

  const auto total = [&] {
    int64_t sum = 0;
    for (const std::string& name : mNames) sum += catalog.find(name)->tokens;
    return sum;
  };
  while (not mNames.empty() and
         (mNames.size() > max_count or (max_tokens > 0 and total() > max_tokens))) {
    auto oldest = std::min_element(
        mNames.begin(), mNames.end(), [&](const std::string& a, const std::string& b) {
          return mLastUse[a] < mLastUse[b];
        });
    dropped.push_back(*oldest);
    mLastUse.erase(*oldest);
    mNames.erase(oldest);
  }
  return dropped;
}

}  // namespace mcp
