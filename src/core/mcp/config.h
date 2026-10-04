#ifndef M8_MCP_CONFIG_H
#define M8_MCP_CONFIG_H

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/util/json_value.h>

namespace mcp {

// ---------------------------------------------------------------------------
// MCP server configuration.
//
// Three files, all in the `{"mcpServers": {...}}` format Claude Code, Claude
// Desktop and Cursor share, so an entry can be copied between them:
//
//   ~/.m8/mcp.json           user: your own servers, in every workspace
//   <workspace>/.mcp.json    shared: the cross-tool project file, committed
//   <workspace>/.m8/mcp.json project: this workspace's servers (gitignored)
//
// A name defined in more than one wins in the order project > shared > user.
// Servers from the two workspace files run only once approved (trust.h): a
// cloned repository must not be able to start a process by shipping a config.
// ---------------------------------------------------------------------------

enum struct Scope : uint8_t { User, Shared, Project };

const char* scope_name(Scope scope);

struct OAuthSettings {
  std::string client_id;
  std::string client_secret;
  std::vector<std::string> scopes;
  int callback_port = 0;  // 0: any free port
};

struct ServerConfig {
  std::string name;
  Scope scope = Scope::User;
  std::string source;  // the file it came from

  std::string type;  // "stdio" | "http"

  // stdio
  std::string command;
  std::vector<std::string> args;
  std::vector<std::pair<std::string, std::string>> env;
  std::string cwd;
  bool inherit_env = false;  // pass all of m8's environment, not the allowlist

  // http
  std::string url;
  std::vector<std::pair<std::string, std::string>> headers;
  OAuthSettings oauth;

  // m8's own keys, alongside the standard ones.
  bool disabled = false;
  bool always_load = false;  // never defer this server's tools
  std::vector<std::string> enabled_tools;   // empty: all
  std::vector<std::string> disabled_tools;
  int64_t timeout_ms = 0;          // tools/call; 0: the default
  int64_t startup_timeout_ms = 0;  // connect; 0: the default
  std::string protocol = "auto";   // "auto" | "modern" | "legacy"

  util::JsonValue raw;  // the entry exactly as written
  std::string problem;  // non-empty: unusable as written, and why
};

// ${VAR} and ${VAR:-default}, from m8's own environment. An unset variable
// without a default leaves `missing` naming it (the first one) and expands to
// nothing, and the caller refuses to start the server: launching it with a
// silently empty token would fail later and less clearly.
std::string expand_variables(std::string_view text, std::string& missing);

// The entry with every value expanded. False (with why) on a missing variable.
bool expand_server(const ServerConfig& in, ServerConfig& out, std::string& error);

// One file's servers. `warnings` collects what made the file or an entry
// unusable; an unusable entry is still returned, with `problem` set, so the
// user sees it listed rather than wondering where it went.
std::vector<ServerConfig> parse_config(std::string_view text, Scope scope,
                                       const std::string& source,
                                       std::vector<std::string>& warnings);

struct ConfigFiles {
  std::string user;     // ~/.m8/mcp.json
  std::string shared;   // <workspace>/.mcp.json
  std::string project;  // <workspace>/.m8/mcp.json
};

struct LoadedConfig {
  std::vector<ServerConfig> servers;  // merged; file order within each
  std::vector<std::string> warnings;
};

// Reads whichever of the three files exist and merges them. When the
// workspace root is $HOME itself (a directory with no nearer .git falls back
// to it), the project file IS the user file; it is then read once, as user.
LoadedConfig load_config(const ConfigFiles& files);

// Approval fingerprint: SHA-256 over the entry's security-relevant fields
// (what runs and where requests go), with sorted keys and unexpanded values.
// Toggling alwaysLoad, a tool filter or a timeout keeps an approval; changing
// the command, its arguments or environment, or a URL or header does not.
std::string config_fingerprint(const ServerConfig& server);

// ---------------------------------------------------------------------------
// Editing a file in place, keeping every key m8 does not know about.
// ---------------------------------------------------------------------------

struct EditResult {
  bool ok = false;
  std::string error;
};

// Adds `entry` under mcpServers.<name>. With `replace` false an existing name
// is an error.
EditResult add_server(const std::string& path, const std::string& name,
                      const util::JsonValue& entry, bool replace = false);
EditResult remove_server(const std::string& path, const std::string& name);

// Entries as `m8 mcp add` writes them.
util::JsonValue stdio_entry(const std::string& command,
                            const std::vector<std::string>& args,
                            const std::vector<std::pair<std::string, std::string>>& env);
util::JsonValue http_entry(const std::string& url,
                           const std::vector<std::pair<std::string, std::string>>& headers);

// Writes `text` to `path` through a temp file and a rename, so a crash cannot
// leave half a config. `private_file` makes it 0600 (it may hold tokens).
bool write_file_atomically(const std::string& path, const std::string& text,
                           bool private_file, std::string& error);

}  // namespace mcp

#endif  // M8_MCP_CONFIG_H
