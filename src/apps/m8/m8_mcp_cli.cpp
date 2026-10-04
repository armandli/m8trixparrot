#include <m8_mcp_cli.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <utility>

#include <core/mcp/config.h>
#include <core/mcp/oauth.h>
#include <core/mcp/registry.h>
#include <core/mcp/trust.h>
#include <core/util/open_url.h>

#include <m8_mcp_view.h>

namespace m8 {

namespace {

using Pairs = std::vector<std::pair<std::string, std::string>>;

// How long `list` and `get` wait for servers to connect: past every server's
// own startup timeout, so a slow one reports its own failure.
constexpr std::chrono::seconds kHealthCheckWait{60};

std::string real_path(const std::string& path) {
  std::error_code ec;
  const std::filesystem::path resolved = std::filesystem::weakly_canonical(path, ec);
  return ec ? path : resolved.string();
}

mcp::ConfigFiles config_files(const M8Paths& paths) {
  mcp::ConfigFiles files;
  files.user = paths.user_mcp_config();
  files.shared = paths.shared_mcp_config();
  files.project = paths.mcp_config();
  return files;
}

// In a directory under $HOME with no nearer .git the workspace IS $HOME, and
// the project file is the user file.
bool project_is_user(const M8Paths& paths) {
  return not paths.user_mcp_config().empty() and
         real_path(paths.mcp_config()) == real_path(paths.user_mcp_config());
}

// "K=V" or "Name: value" into its two halves; false when it is neither.
bool split_pair(const std::string& text, char separator, bool trim,
                std::pair<std::string, std::string>& out) {
  const size_t at = text.find(separator);
  if (at == std::string::npos or at == 0) return false;
  std::string key = text.substr(0, at);
  std::string value = text.substr(at + 1);
  if (trim) {
    while (not key.empty() and key.back() == ' ') key.pop_back();
    value.erase(0, value.find_first_not_of(' '));
  }
  if (key.empty()) return false;
  out = {key, value};
  return true;
}

std::optional<mcp::ServerConfig> find_server(const mcp::LoadedConfig& loaded,
                                             const std::string& name) {
  for (const mcp::ServerConfig& server : loaded.servers) {
    if (server.name == name) return server;
  }
  return std::nullopt;
}

// What m8 would say about the entry on reading it back, so `add` refuses an
// entry that could only fail later, at startup.
std::string entry_problem(const std::string& name, const util::JsonValue& entry) {
  util::JsonValue servers = util::JsonValue::object();
  servers.set(name, entry);
  util::JsonValue document = util::JsonValue::object();
  document.set("mcpServers", std::move(servers));
  std::vector<std::string> warnings;
  const std::vector<mcp::ServerConfig> parsed =
      mcp::parse_config(document.dump(), mcp::Scope::Project, "", warnings);
  if (parsed.empty()) return warnings.empty() ? "unusable entry" : warnings.front();
  return parsed.front().problem;
}

mcp::RegistryOptions registry_options(const M8Paths& paths,
                                      const std::string& client_metadata_url = "") {
  mcp::RegistryOptions options;
  options.workspace = workspace_key(paths);
  options.logs_dir = paths.mcp_logs();
  options.state_path = paths.mcp_state();
  options.trust_path = paths.mcp_trust();
  options.cache_path = paths.mcp_cache();
  options.credentials_path = paths.mcp_credentials();
  options.client_metadata_url = client_metadata_url;
  return options;
}

bool confirm(std::ostream& out, std::istream& in, const std::string& question) {
  out << question << " [y/N] " << std::flush;
  std::string answer;
  if (not std::getline(in, answer)) return false;
  return answer == "y" or answer == "Y" or answer == "yes" or answer == "Yes";
}

std::string server_line(const mcp::CatalogServer& server) {
  std::string line = "  " + server.name + "  [" + server.scope + ", " +
                     server.transport + "]  " + mcp::state_name(server.state);
  if (server.state == mcp::ServerState::Connected) {
    line += " \xe2\x80\x94 " + std::to_string(server.tool_count) + " tools (~" +
            human_tokens(server.schema_tokens) + " tokens of schemas), protocol " +
            server.protocol_version;
  } else if (not server.error.empty()) {
    line += ": " + server.error;
  }
  line += "\n      " + server.detail;
  if (server.state == mcp::ServerState::Failed and not server.stderr_tail.empty()) {
    line += "\n      stderr: " + server.stderr_tail;
  }
  return line;
}

// The file a scope's servers are written to; empty, with `error` set, when the
// scope has none.
std::string scope_path(const M8Paths& paths, const std::string& scope,
                       std::ostream& out, std::string& error) {
  if (scope == "user") {
    if (paths.user_mcp_config().empty()) {
      error = "there is no home directory to keep a user-scope server in";
    }
    return paths.user_mcp_config();
  }
  if (scope == "shared") return paths.shared_mcp_config();
  if (project_is_user(paths)) {
    out << "note: this workspace is your home directory, so the server goes in "
           "~/.m8/mcp.json (user scope)\n";
  }
  return paths.mcp_config();
}

// The approvals, or false (with why) when the file is there but unreadable:
// saving over it would silently drop every other approval in it.
bool load_trust(mcp::TrustStore& trust, std::ostream& err) {
  std::string error;
  if (trust.load(error)) return true;
  err << "cannot read the MCP approvals: " << error << "\n";
  return false;
}

// After adding a workspace server: approve it now when a person typed the
// command at a terminal, otherwise say how.
void approve_added(const M8Paths& paths, const std::string& name, bool interactive,
                   std::ostream& out, std::ostream& err) {
  const mcp::LoadedConfig loaded = mcp::load_config(config_files(paths));
  const std::optional<mcp::ServerConfig> server = find_server(loaded, name);
  if (not server or not mcp::needs_approval(*server)) return;
  if (not interactive or paths.mcp_trust().empty()) {
    out << "It runs once approved: m8 mcp approve " << name << " (or /mcp approve "
        << name << " in m8).\n";
    return;
  }
  mcp::TrustStore trust(paths.mcp_trust());
  if (not load_trust(trust, err)) return;
  trust.approve(workspace_key(paths), *server);
  std::string error;
  if (trust.save(error)) {
    out << "Approved for this workspace.\n";
  } else {
    err << "could not record the approval: " << error << "\n";
  }
}

int write_entry(const M8Paths& paths, const std::string& scope,
                const std::string& name, const util::JsonValue& entry,
                const std::string& what, bool interactive, std::ostream& out,
                std::ostream& err) {
  if (const std::string problem = entry_problem(name, entry); not problem.empty()) {
    err << "cannot add '" << name << "': " << problem << "\n";
    return 1;
  }
  std::string error;
  const std::string path = scope_path(paths, scope, out, error);
  if (not error.empty()) {
    err << error << "\n";
    return 1;
  }
  const mcp::EditResult result = mcp::add_server(path, name, entry);
  if (not result.ok) {
    err << result.error << "\n";
    return 1;
  }
  out << "Added MCP server '" << name << "'" << what << " to " << path << ".\n";
  approve_added(paths, name, interactive, out, err);
  return 0;
}

}  // namespace

std::string workspace_key(const M8Paths& paths) { return real_path(paths.root); }

void McpCli::declare(CLI::App& app) {
  mMcp = app.add_subcommand("mcp", "Install and manage MCP servers");
  mMcp->require_subcommand(1);

  mAdd = mMcp->add_subcommand(
      "add",
      "Add an MCP server: `m8 mcp add <name> -- <command> [args...]` for a local "
      "(stdio) server, `m8 mcp add <name> <url>` for a remote (http) one");
  mAdd->add_option("-s,--scope", mScope,
                   "project (.m8/mcp.json), user (~/.m8/mcp.json) or shared "
                   "(.mcp.json, committed with the repository)")
      ->check(CLI::IsMember({"project", "user", "shared"}))
      ->capture_default_str();
  mAdd->add_option("-t,--transport", mTransport,
                   "stdio or http (default: http for an http(s) URL, else stdio)")
      ->check(CLI::IsMember({"stdio", "http"}));
  // One value per -e/-H: otherwise `-e A=1 name` would take the server name as
  // a second value.
  mAdd->add_option("-e,--env", mEnv, "Environment variable for the server, KEY=VALUE")
      ->allow_extra_args(false);
  mAdd->add_option("-H,--header", mHeaders, "HTTP header for the server, \"Name: value\"")
      ->allow_extra_args(false);
  mAdd->add_option("name", mName, "Name for the server")->required();
  mAdd->add_option("target", mTarget, "The command to run, or the server's URL")
      ->required();
  mAdd->add_option("args", mArgs, "Arguments for the command (after --)");

  mAddJson = mMcp->add_subcommand("add-json", "Add an MCP server from a JSON entry");
  mAddJson->add_option("-s,--scope", mScope, "project, user or shared")
      ->check(CLI::IsMember({"project", "user", "shared"}))
      ->capture_default_str();
  mAddJson->add_option("name", mName, "Name for the server")->required();
  mAddJson->add_option("json", mJson, "The server's entry, as in an mcpServers file")
      ->required();

  mList = mMcp->add_subcommand("list", "List MCP servers and check that they connect");
  mGet = mMcp->add_subcommand("get", "Show one MCP server and its tools");
  mGet->add_option("name", mName, "The server")->required();

  mRemove = mMcp->add_subcommand("remove", "Remove an MCP server");
  mRemove->add_option("name", mName, "The server")->required();
  mRemove->add_option("-s,--scope", mScope, "Which file, when it is in several")
      ->check(CLI::IsMember({"project", "user", "shared"}));

  mEnable = mMcp->add_subcommand("enable", "Enable an MCP server in this workspace");
  mEnable->add_option("name", mName, "The server")->required();
  mDisable = mMcp->add_subcommand("disable", "Disable an MCP server in this workspace");
  mDisable->add_option("name", mName, "The server")->required();

  mApprove = mMcp->add_subcommand(
      "approve", "Approve a server from a workspace file (.m8/mcp.json, .mcp.json)");
  mApprove->add_option("name", mName, "The server");
  mApprove->add_flag("--all", mAll, "Every server waiting for approval");

  mLogin = mMcp->add_subcommand("login", "Log in to a remote MCP server (OAuth)");
  mLogin->add_option("name", mName, "The server")->required();
  mLogout = mMcp->add_subcommand("logout", "Forget a remote MCP server's login");
  mLogout->add_option("name", mName, "The server")->required();

  mImport = mMcp->add_subcommand(
      "import", "Copy MCP servers from another tool's config file (Claude Desktop, Cursor, "
                "VS Code, a .mcp.json)");
  mImport->add_option("file", mFile, "The file to copy from")->required();
  mImport->add_option("-s,--scope", mScope, "project, user or shared")
      ->check(CLI::IsMember({"project", "user", "shared"}))
      ->capture_default_str();
}

bool McpCli::parsed() const { return mMcp != nullptr and mMcp->parsed(); }

int McpCli::run(const M8Paths& paths, bool agent_shell, std::ostream& out,
                std::ostream& err, std::istream& in) {
  const bool changes_servers = mAdd->parsed() or mAddJson->parsed() or
                               mRemove->parsed() or mEnable->parsed() or
                               mApprove->parsed() or mLogin->parsed() or
                               mImport->parsed();
  if (changes_servers and agent_shell) {
    err << "refused: this changes which MCP servers m8 runs, which is for the "
           "user to decide, and this is an m8 agent's shell (M8_AGENT_SHELL is "
           "set). Run it in a terminal of your own, or use /mcp in m8.\n";
    return 1;
  }
  if (mAdd->parsed()) return add(paths, out, err);
  if (mAddJson->parsed()) return add_json(paths, out, err);
  if (mList->parsed()) return list(paths, out, err);
  if (mGet->parsed()) return get(paths, out, err);
  if (mRemove->parsed()) return remove(paths, out, err);
  if (mEnable->parsed()) return set_enabled(paths, true, out, err);
  if (mDisable->parsed()) return set_enabled(paths, false, out, err);
  if (mApprove->parsed()) return approve(paths, out, err, in);
  if (mLogin->parsed()) return login(paths, false, out, err);
  if (mLogout->parsed()) return login(paths, true, out, err);
  if (mImport->parsed()) return import_file(paths, out, err);
  err << "unknown mcp subcommand\n";
  return 1;
}

int McpCli::add(const M8Paths& paths, std::ostream& out, std::ostream& err) {
  std::string transport = mTransport;
  if (transport.empty()) {
    const bool url = mTarget.rfind("http://", 0) == 0 or mTarget.rfind("https://", 0) == 0;
    transport = url ? "http" : "stdio";
  }

  Pairs env;
  for (const std::string& item : mEnv) {
    std::pair<std::string, std::string> pair;
    if (not split_pair(item, '=', /*trim=*/false, pair)) {
      err << "-e wants KEY=VALUE, not '" << item << "'\n";
      return 1;
    }
    env.push_back(pair);
  }
  Pairs headers;
  for (const std::string& item : mHeaders) {
    std::pair<std::string, std::string> pair;
    if (not split_pair(item, ':', /*trim=*/true, pair)) {
      err << "-H wants \"Name: value\", not '" << item << "'\n";
      return 1;
    }
    headers.push_back(pair);
  }

  util::JsonValue entry;
  if (transport == "http") {
    if (not mArgs.empty() or not env.empty()) {
      err << "an http server takes no command arguments or -e; headers go in -H\n";
      return 1;
    }
    entry = mcp::http_entry(mTarget, headers);
  } else {
    if (not headers.empty()) {
      err << "-H is for http servers; a stdio server takes -e environment variables\n";
      return 1;
    }
    entry = mcp::stdio_entry(mTarget, mArgs, env);
  }
  return write_entry(paths, mScope, mName, entry, " (" + transport + ")", interactive,
                     out, err);
}

int McpCli::add_json(const M8Paths& paths, std::ostream& out, std::ostream& err) {
  std::string parse_error;
  const std::optional<util::JsonValue> entry = util::JsonValue::parse(mJson, &parse_error);
  if (not entry or not entry->is_object()) {
    err << "the entry is not a JSON object"
        << (parse_error.empty() ? "" : " (" + parse_error + ")") << "\n";
    return 1;
  }
  return write_entry(paths, mScope, mName, *entry, "", interactive, out, err);
}

int McpCli::list(const M8Paths& paths, std::ostream& out, std::ostream& err) {
  const mcp::LoadedConfig loaded = mcp::load_config(config_files(paths));
  for (const std::string& warning : loaded.warnings) err << "warning: " << warning << "\n";
  if (loaded.servers.empty()) {
    out << "No MCP servers configured. Add one with:\n"
           "  m8 mcp add <name> -- <command> [args...]\n"
           "  m8 mcp add <name> <url>\n";
    return 0;
  }
  out << "Checking " << loaded.servers.size() << " MCP server"
      << (loaded.servers.size() == 1 ? "" : "s") << "...\n";
  mcp::Registry registry(registry_options(paths));
  registry.start(loaded.servers);
  const auto catalog = registry.wait_until_settled(kHealthCheckWait);
  for (const mcp::CatalogServer& server : catalog->servers) {
    out << server_line(server) << "\n";
  }
  const std::vector<std::string> pending = registry.pending_approval();
  if (not pending.empty()) {
    out << "\nWaiting for your approval before they run (m8 mcp approve <name>):";
    for (const std::string& name : pending) out << " " << name;
    out << "\n";
  }
  registry.shutdown();
  return 0;
}

int McpCli::get(const M8Paths& paths, std::ostream& out, std::ostream& err) {
  const mcp::LoadedConfig loaded = mcp::load_config(config_files(paths));
  const std::optional<mcp::ServerConfig> server = find_server(loaded, mName);
  if (not server) {
    err << "no MCP server named '" << mName << "'\n";
    return 1;
  }
  out << server_config_text(*server);
  mcp::Registry registry(registry_options(paths));
  registry.start({*server});
  const auto catalog = registry.wait_until_settled(kHealthCheckWait);
  if (const mcp::CatalogServer* status = catalog->server(mName)) {
    out << server_line(*status) << "\n";
    if (status->state == mcp::ServerState::Connected) {
      out << mcp_tools_text(*catalog, mName, /*deferred=*/false, {}, /*status=*/false)
          << "\n";
      for (const mcp::PromptInfo& prompt : status->prompts) {
        out << "  prompt  " << prompt_usage(mName, prompt) << "\n";
      }
    }
  }
  registry.shutdown();
  return 0;
}

int McpCli::remove(const M8Paths& paths, std::ostream& out, std::ostream& err) {
  struct Holder {
    std::string scope;
    std::string path;
  };
  std::vector<Holder> files = {{"project", paths.mcp_config()},
                               {"shared", paths.shared_mcp_config()}};
  // Read once when the project file is the user file.
  if (not paths.user_mcp_config().empty() and not project_is_user(paths)) {
    files.push_back({"user", paths.user_mcp_config()});
  }
  const bool scoped = mRemove->count("--scope") != 0;
  std::vector<Holder> holders;
  for (const Holder& file : files) {
    if (scoped and file.scope != mScope and
        not (mScope == "user" and file.scope == "project" and project_is_user(paths))) {
      continue;
    }
    mcp::ConfigFiles one;
    one.project = file.path;  // the scope only decides the approval rule
    if (find_server(mcp::load_config(one), mName)) holders.push_back(file);
  }
  if (holders.empty()) {
    err << "no MCP server named '" << mName << "'"
        << (scoped ? " in the " + mScope + " file" : "") << "\n";
    return 1;
  }
  if (holders.size() > 1) {
    err << "'" << mName << "' is in more than one file; say which with -s";
    for (const Holder& holder : holders) err << " " << holder.scope;
    err << "\n";
    return 1;
  }
  const Holder& holder = holders.front();
  const mcp::EditResult result = mcp::remove_server(holder.path, mName);
  if (not result.ok) {
    err << result.error << "\n";
    return 1;
  }
  out << "Removed '" << mName << "' from " << holder.path << ".\n";
  // A server added again later under the same name is a new decision.
  if (holder.scope != "user" and not paths.mcp_trust().empty()) {
    mcp::TrustStore trust(paths.mcp_trust());
    std::string error;
    if (trust.load(error)) {
      trust.revoke(workspace_key(paths), mName);
      trust.save(error);
    }
  }
  return 0;
}

int McpCli::set_enabled(const M8Paths& paths, bool enabled, std::ostream& out,
                        std::ostream& err) {
  const mcp::LoadedConfig loaded = mcp::load_config(config_files(paths));
  if (not find_server(loaded, mName)) {
    err << "no MCP server named '" << mName << "'\n";
    return 1;
  }
  mcp::McpState state(paths.mcp_state());
  state.load();
  state.set_enabled(mName, enabled);
  std::string error;
  if (not state.save(error)) {
    err << error << "\n";
    return 1;
  }
  out << (enabled ? "Enabled '" : "Disabled '") << mName << "' in this workspace.\n";
  return 0;
}

int McpCli::approve(const M8Paths& paths, std::ostream& out, std::ostream& err,
                    std::istream& in) {
  if (not interactive) {
    err << "m8 mcp approve shows each command and asks first, so it needs a "
           "terminal\n";
    return 1;
  }
  if (mName.empty() == not mAll) {
    err << "approve one server by name, or every waiting one with --all\n";
    return 1;
  }
  if (paths.mcp_trust().empty()) {
    err << "there is no home directory to keep approvals in\n";
    return 1;
  }
  const mcp::LoadedConfig loaded = mcp::load_config(config_files(paths));
  if (not mAll and not find_server(loaded, mName)) {
    err << "no MCP server named '" << mName << "'\n";
    return 1;
  }
  mcp::TrustStore trust(paths.mcp_trust());
  if (not load_trust(trust, err)) return 1;
  const std::string workspace = workspace_key(paths);

  int asked = 0;
  int approved = 0;
  for (const mcp::ServerConfig& server : loaded.servers) {
    if (not mAll and server.name != mName) continue;
    if (not mcp::needs_approval(server)) {
      if (not mAll) out << "'" << server.name << "' is your own (user scope); it needs no approval.\n";
      continue;
    }
    if (trust.approved(workspace, server)) {
      if (not mAll) out << "'" << server.name << "' is already approved.\n";
      continue;
    }
    if (not server.problem.empty()) {
      out << "'" << server.name << "' cannot run as written (" << server.problem
          << "); fix it first.\n";
      continue;
    }
    ++asked;
    out << "\n" << server_config_text(server);
    if (confirm(out, in, "Let m8 run '" + server.name + "' in this workspace?")) {
      trust.approve(workspace, server);
      ++approved;
    }
  }
  if (approved > 0) {
    std::string error;
    if (not trust.save(error)) {
      err << "could not record the approval: " << error << "\n";
      return 1;
    }
  }
  if (asked == 0) {
    if (mAll) out << "No MCP server is waiting for approval.\n";
    return 0;
  }
  out << "Approved " << approved << " of " << asked << ".\n";
  return 0;
}

int McpCli::import_file(const M8Paths& paths, std::ostream& out, std::ostream& err) {
  std::ifstream in(mFile, std::ios::binary);
  if (not in) {
    err << "cannot read " << mFile << "\n";
    return 1;
  }
  std::stringstream text;
  text << in.rdbuf();
  std::string parse_error;
  const std::optional<util::JsonValue> document = util::JsonValue::parse(text.str(), &parse_error);
  if (not document or not document->is_object()) {
    err << mFile << " is not a JSON object" << (parse_error.empty() ? "" : " (" + parse_error + ")")
        << "\n";
    return 1;
  }
  // Claude Desktop, Claude Code and Cursor say mcpServers; VS Code says servers.
  const util::JsonValue* servers = document->find("mcpServers");
  if (servers == nullptr) servers = document->find("servers");
  if (servers == nullptr or not servers->is_object() or servers->size() == 0) {
    err << mFile << " has no MCP servers (no \"mcpServers\" or \"servers\" object)\n";
    return 1;
  }
  std::string error;
  const std::string path = scope_path(paths, mScope, out, error);
  if (not error.empty()) {
    err << error << "\n";
    return 1;
  }

  int added = 0;
  for (const util::JsonValue::Member& member : servers->members()) {
    if (const std::string problem = entry_problem(member.key, member.value); not problem.empty()) {
      out << "  skipped " << member.key << ": " << problem << "\n";
      continue;
    }
    const mcp::EditResult result = mcp::add_server(path, member.key, member.value);
    if (not result.ok) {
      out << "  skipped " << member.key << ": " << result.error << "\n";
      continue;
    }
    out << "  added " << member.key << "\n";
    approve_added(paths, member.key, interactive, out, err);
    ++added;
  }
  out << "Imported " << added << " of " << servers->size() << " into " << path << ".\n";
  return 0;
}

int McpCli::login(const M8Paths& paths, bool logout, std::ostream& out,
                  std::ostream& err) {
  const mcp::LoadedConfig loaded = mcp::load_config(config_files(paths));
  const std::optional<mcp::ServerConfig> server = find_server(loaded, mName);
  if (not server) {
    err << "no MCP server named '" << mName << "'\n";
    return 1;
  }
  if (server->type != "http") {
    err << "'" << mName << "' is a local server; it takes its credentials from its "
           "environment (-e), not a login\n";
    return 1;
  }
  mcp::Registry registry(registry_options(paths, client_metadata_url));
  registry.start({*server});
  std::string error;
  if (logout) {
    const bool ok = registry.logout(mName, error);
    registry.shutdown();
    if (not ok) {
      err << error << "\n";
      return 1;
    }
    out << "Logged out of '" << mName << "'.\n";
    return 0;
  }

  // Connecting first hears the server's challenge: where its metadata is,
  // which scopes it wants.
  registry.wait_until_settled(std::chrono::seconds(30));
  const bool ok = registry.login(
      mName,
      [&](const std::string& url) {
        out << "To log in to " << mName << ", open this link:\n\n  " << url << "\n\n";
        std::string why;
        if (util::open_url(url, why)) {
          out << "(It is open in your browser.)\n";
        } else {
          out << "(Could not open a browser: " << why << ".)\n";
        }
        out << "The browser comes back to this machine on 127.0.0.1; over SSH, forward "
               "that port (set \"oauth\": {\"callbackPort\": N} to fix it).\n"
            << "Waiting for it, up to five minutes (Ctrl+C gives up)...\n"
            << std::flush;
      },
      nullptr, error);
  if (not ok) {
    registry.shutdown();
    err << "login failed: " << error << "\n";
    return 1;
  }
  const auto catalog = registry.wait_until_settled(std::chrono::seconds(30));
  out << "Logged in to '" << mName << "'.\n";
  if (const mcp::CatalogServer* status = catalog->server(mName)) {
    out << server_line(*status) << "\n";
  }
  registry.shutdown();
  return 0;
}

}  // namespace m8
