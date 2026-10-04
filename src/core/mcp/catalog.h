#ifndef M8_MCP_CATALOG_H
#define M8_MCP_CATALOG_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <core/mcp/protocol.h>
#include <core/mcp/schema.h>
#include <core/util/json_value.h>

namespace mcp {

struct SearchIndex;  // tool_search.h

enum struct ServerState : uint8_t {
  Disabled,       // the user turned it off
  NeedsApproval,  // from a workspace file, not approved yet: never started
  Connecting,
  Connected,
  NeedsAuth,      // an HTTP server answered 401; `/mcp login` it
  Failed,
};

const char* state_name(ServerState state);

// One configured server as the rest of m8 sees it: no secrets in here (header
// and env values never leave the registry), just what a status line needs.
struct CatalogServer {
  std::string name;
  std::string scope;      // "user" | "project" | "shared"
  std::string transport;  // "stdio" | "http"
  std::string detail;     // the command line or URL, for display
  ServerState state = ServerState::Connecting;
  Era era = Era::Unknown;
  std::string protocol_version;
  ServerInfo info;
  std::string instructions;  // the server's own guidance for the model
  std::string error;         // why it is Failed / NeedsAuth
  std::string stderr_tail;
  ServerCapabilities capabilities;
  bool always_load = false;
  // Still connecting, but showing last run's tools (calls wait for it).
  bool cached = false;
  size_t tool_count = 0;
  int64_t schema_tokens = 0;
  std::vector<PromptInfo> prompts;
};

// One MCP tool, ready to hand to the model.
struct CatalogTool {
  std::string exposed;  // mcp__<server>__<tool>: what the model calls
  std::string server;
  std::string name;  // the server's own name, for tools/call
  std::string title;
  std::string description;
  util::JsonValue input_schema;  // as the server sent it
  util::JsonValue parameters;    // lowered for Ollama (schema.h)
  std::string schema_json;       // {"name","description","parameters"}
  std::string signature;         // compact, for tool_search results
  int64_t tokens = 0;            // estimated cost of schema_json
  bool always_load = false;      // the server opted out of deferral
  ToolAnnotations annotations;
  std::vector<HeaderParam> header_params;  // x-mcp-header (HTTP only)
};

// An immutable snapshot of every server and tool. The registry builds a new
// one whenever anything changes and hands out shared pointers, so an agent
// thread holding one for a whole turn never sees it shift underneath it.
struct Catalog {
  uint64_t generation = 0;
  std::vector<CatalogServer> servers;
  std::vector<CatalogTool> tools;  // server order, then the server's order
  int64_t total_tokens = 0;        // every tool's schema, loaded or not
  std::shared_ptr<const SearchIndex> index;

  const CatalogTool* find(std::string_view exposed) const;
  const CatalogServer* server(std::string_view name) const;
  // Tools whose server-side name is `name`, across all servers.
  std::vector<const CatalogTool*> by_original_name(std::string_view name) const;

  bool any_connecting() const;
  size_t connected_count() const;
  bool any_resources() const;

  // Index lookups; filled by finalize_catalog().
  std::unordered_map<std::string, size_t> by_exposed;
};

// True for a name in the mcp__ namespace. Built-in tools never use it.
bool is_mcp_tool_name(std::string_view name);

// Empty when `name` is usable as a server name; otherwise why not. Server
// names become part of every tool name, so they are kept to identifier-ish
// characters, and `__` is reserved as the separator.
std::string server_name_problem(std::string_view name);

// mcp__<server>__<tool>, restricted to [A-Za-z0-9_] — Llama- and Gemma-style
// tool-call formats write calls as Python identifiers — and capped at 64
// characters with a stable hash suffix.
std::string exposed_tool_name(std::string_view server, std::string_view tool);

// Lowers the schema and prepares everything else the model will see.
// Returns false (with why) when the tool cannot be offered at all.
bool make_catalog_tool(std::string_view server, const ToolInfo& tool,
                       bool always_load, bool http, CatalogTool& out,
                       std::string& problem);

// Resolves exposed-name collisions, totals the cost, builds the search index.
std::shared_ptr<const Catalog> finalize_catalog(Catalog catalog);

// The MCP tools one agent has loaded into its tools array, in load order.
// The agent thread adds to it and the UI reads it (/context, /mcp), hence the
// lock.
struct LoadedTools {
  // True when newly added.
  bool add(const std::string& exposed);
  // Marks a use, for least-recently-used eviction.
  void touch(const std::string& exposed);
  bool contains(std::string_view exposed) const;
  std::vector<std::string> names() const;
  size_t size() const;
  void clear();
  void assign(const std::vector<std::string>& exposed);

  // Drops least-recently-used tools (and any no longer in `catalog`) until at
  // most `max_count` remain and their schemas fit in `max_tokens`. Returns the
  // names dropped. The agent calls this between turns only, never mid-turn.
  std::vector<std::string> evict(size_t max_count, int64_t max_tokens,
                                 const Catalog& catalog);

private:
  mutable std::mutex mMutex;
  std::vector<std::string> mNames;
  std::unordered_map<std::string, uint64_t> mLastUse;
  uint64_t mClock = 0;
};

}  // namespace mcp

#endif  // M8_MCP_CATALOG_H
