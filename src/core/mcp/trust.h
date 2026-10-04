#ifndef M8_MCP_TRUST_H
#define M8_MCP_TRUST_H

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <core/mcp/config.h>
#include <core/mcp/protocol.h>
#include <core/util/json_value.h>

namespace mcp {

// True for servers that need an approval before they run: those from a
// workspace file, which arrive with a clone. The user's own file is theirs.
bool needs_approval(const ServerConfig& server);

// ~/.m8/mcp_trust.json: which workspace servers the user approved, by the
// fingerprint of the entry they saw (config_fingerprint). Editing an entry
// changes its fingerprint, so a server whose command was changed — by a pull,
// or by an agent — has to be approved again before it runs.
//
// Kept in the home directory and not in the workspace, where the repository
// (or the agent working in it) could write its own approvals; it is also on
// the protected-paths list.
struct TrustStore {
  explicit TrustStore(std::string path);

  // A missing file is an empty store; an unreadable one is reported.
  bool load(std::string& error);
  bool save(std::string& error) const;

  bool approved(const std::string& workspace, const ServerConfig& server) const;
  void approve(const std::string& workspace, const ServerConfig& server);
  void revoke(const std::string& workspace, const std::string& name);

private:
  std::string mPath;
  mutable std::mutex mMutex;
  util::JsonValue mData;  // {"workspaces": {<path>: {<server>: <fingerprint>}}}
};

// <workspace>/.m8/mcp_state.json: what m8 remembers about a workspace's
// servers between runs — the protocol era it detected for each exact
// configuration (so a silent legacy server does not cost the probe wait every
// launch) and the user's enable/disable choices (kept here rather than written
// into a config file, which may be committed).
struct McpState {
  explicit McpState(std::string path);

  void load();
  bool save(std::string& error) const;

  Era era(const ServerConfig& server) const;
  void set_era(const ServerConfig& server, Era era);

  // nullopt: no override, the config file decides.
  std::optional<bool> enabled(const std::string& name) const;
  void set_enabled(const std::string& name, bool enabled);

private:
  std::string mPath;
  mutable std::mutex mMutex;
  util::JsonValue mData;  // {"eras": {key: "modern"|"legacy"}, "enabled": {name: bool}}
};

// <workspace>/.m8/mcp_cache.json: each server's last tool and prompt lists,
// for this exact configuration, so a server that takes seconds to start (npx
// fetching a package) shows its tools at once. Kept apart from the state
// file, which stays small enough to read.
struct ToolCache {
  explicit ToolCache(std::string path);

  struct Entry {
    std::vector<ToolInfo> tools;
    std::vector<PromptInfo> prompts;
  };
  std::optional<Entry> get(const ServerConfig& server) const;
  bool put(const ServerConfig& server, const std::vector<ToolInfo>& tools,
           const std::vector<PromptInfo>& prompts, std::string& error);

private:
  std::string mPath;
  mutable std::mutex mMutex;
};

}  // namespace mcp

#endif  // M8_MCP_TRUST_H
