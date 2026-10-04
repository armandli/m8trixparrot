#include <core/mcp/registry.h>

#include <algorithm>
#include <filesystem>

#include <core/mcp/content.h>
#include <core/mcp/schema.h>
#include <core/mcp/stdio_transport.h>
#include <core/util/process.h>

namespace mcp {

namespace {

std::string join_command(const ServerConfig& config) {
  std::string out = config.command;
  for (const std::string& arg : config.args) out += " " + arg;
  if (out.size() > 120) out = out.substr(0, 117) + "...";
  return out;
}

bool listed(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

std::chrono::milliseconds or_default(int64_t ms, std::chrono::milliseconds fallback) {
  return ms > 0 ? std::chrono::milliseconds(ms) : fallback;
}

tools::ToolResult failure(std::string message) {
  tools::ToolResult result;
  result.error = std::move(message);
  return result;
}

}  // namespace

Registry::Registry(RegistryOptions options)
    : mOptions(std::move(options)),
      mTrust(mOptions.trust_path),
      mState(mOptions.state_path) {
  mSnapshot = finalize_catalog(Catalog{});
}

Registry::~Registry() { shutdown(); }

void Registry::set_observer(RegistryObserver observer) {
  std::lock_guard<std::mutex> lock(mMutex);
  mObserver = std::move(observer);
}

void Registry::set_elicitation_handler(ElicitationHandler handler) {
  std::lock_guard<std::mutex> lock(mMutex);
  mElicit = std::move(handler);
}

void Registry::set_http_factory(HttpTransportFactory factory) {
  std::lock_guard<std::mutex> lock(mMutex);
  mHttpFactory = std::move(factory);
}

Registry::Server* Registry::find(const std::string& name) {
  for (const std::unique_ptr<Server>& server : mServers) {
    if (server->config.name == name) return server.get();
  }
  return nullptr;
}

const Registry::Server* Registry::find(const std::string& name) const {
  for (const std::unique_ptr<Server>& server : mServers) {
    if (server->config.name == name) return server.get();
  }
  return nullptr;
}

void Registry::spawn(std::function<void()> work) {
  std::lock_guard<std::mutex> lock(mTaskMutex);
  // Join what has finished, so a long session does not pile up threads.
  for (auto it = mTasks.begin(); it != mTasks.end();) {
    if (it->done->load()) {
      if (it->thread.joinable()) it->thread.join();
      it = mTasks.erase(it);
    } else {
      ++it;
    }
  }
  auto done = std::make_shared<std::atomic<bool>>(false);
  Task task;
  task.done = done;
  task.thread = std::thread([work = std::move(work), done] {
    work();
    done->store(true);
  });
  mTasks.push_back(std::move(task));
}

void Registry::start(std::vector<ServerConfig> servers) {
  std::vector<RegistryEvent> notices;
  {
    std::string error;
    if (not mTrust.load(error)) notices.push_back(RegistryEvent{"", ServerState::Failed, error});
    mState.load();
  }
  {
    std::lock_guard<std::mutex> lock(mMutex);
    for (ServerConfig& config : servers) {
      auto server = std::make_unique<Server>();
      server->config = std::move(config);
      const ServerConfig& c = server->config;
      const std::optional<bool> enabled = mState.enabled(c.name);
      if (not c.problem.empty()) {
        server->state = ServerState::Failed;
        server->error = c.problem;
      } else if (enabled.has_value() ? not *enabled : c.disabled) {
        server->state = ServerState::Disabled;
      } else if (needs_approval(c) and not mTrust.approved(mOptions.workspace, c)) {
        server->state = ServerState::NeedsApproval;
      } else {
        server->state = ServerState::Connecting;
      }
      mServers.push_back(std::move(server));
    }
    for (const std::unique_ptr<Server>& server : mServers) {
      if (server->state == ServerState::Connecting) start_connect(*server);
    }
  }
  publish();
  RegistryObserver observer;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    observer = mObserver;
  }
  if (observer) {
    for (const RegistryEvent& notice : notices) observer(notice);
  }
}

void Registry::start_connect(Server& server) {
  server.state = ServerState::Connecting;
  server.error.clear();
  const uint64_t epoch = ++server.epoch;
  const std::string name = server.config.name;
  spawn([this, name, epoch] { connect(name, epoch); });
}

std::shared_ptr<Client> Registry::make_client(const ServerConfig& expanded,
                                              std::string& error) {
  ClientOptions options;
  options.server = expanded.name;
  options.protocol = expanded.protocol;
  options.remembered_era = mState.era(expanded);
  options.probe_timeout = mOptions.probe_timeout;
  options.startup_timeout = or_default(expanded.startup_timeout_ms, mOptions.startup_timeout);
  options.list_timeout = mOptions.list_timeout;
  options.call_timeout = or_default(expanded.timeout_ms, mOptions.call_timeout);
  options.read_timeout = mOptions.read_timeout;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    options.elicit = mElicit;
  }
  const std::string name = expanded.name;
  options.on_list_changed = [this, name] { schedule_refresh(name); };

  if (expanded.type == "http") {
    HttpTransportFactory factory;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      factory = mHttpFactory;
    }
    if (not factory) {
      error = "HTTP MCP servers are not available in this build";
      return nullptr;
    }
    options.http = true;
    options.make_transport = [factory, expanded](TransportHandlers handlers) {
      return factory(expanded, std::move(handlers));
    };
    return std::make_shared<Client>(std::move(options));
  }

  const util::EnvList env = util::child_environment(expanded.inherit_env, expanded.env);
  std::string cwd = expanded.cwd.empty() ? mOptions.workspace : expanded.cwd;
  if (not cwd.empty() and cwd.front() != '/' and not mOptions.workspace.empty()) {
    cwd = mOptions.workspace + "/" + cwd;
  }
  const std::string resolved =
      util::find_executable(expanded.command, util::env_lookup(env, "PATH"), cwd);
  if (resolved.empty()) {
    error = "the command '" + expanded.command + "' was not found on the PATH" +
            " it would run with (is it installed?)";
    return nullptr;
  }

  StdioConfig stdio;
  stdio.server = expanded.name;
  stdio.command = resolved;
  stdio.args = expanded.args;
  stdio.env = env;
  stdio.cwd = cwd;
  if (not mOptions.logs_dir.empty()) {
    stdio.log_path = mOptions.logs_dir + "/" + expanded.name + ".log";
  }
  stdio.close_grace = mOptions.close_grace;
  stdio.term_grace = mOptions.term_grace;
  options.make_transport = [stdio](TransportHandlers handlers) {
    return std::make_unique<StdioTransport>(stdio, std::move(handlers));
  };
  return std::make_shared<Client>(std::move(options));
}

void Registry::connect(const std::string& name, uint64_t epoch) {
  ServerConfig config;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    Server* server = find(name);
    if (server == nullptr or server->epoch != epoch or mShutdown) return;
    config = server->config;
  }
  publish();

  const auto fail = [&](ServerState state, const std::string& error) {
    {
      std::lock_guard<std::mutex> lock(mMutex);
      Server* server = find(name);
      if (server == nullptr or server->epoch != epoch) return;
      server->state = state;
      server->error = error;
      server->client.reset();
      server->tools.clear();
      server->prompts.clear();
    }
    const RegistryEvent event{name, state,
                              state == ServerState::NeedsAuth
                                  ? "mcp: " + name + " needs you to log in — /mcp login " + name
                                  : "mcp: " + name + " failed: " + error};
    publish(&event);
  };

  ServerConfig expanded;
  std::string error;
  if (not expand_server(config, expanded, error)) return fail(ServerState::Failed, error);

  std::shared_ptr<Client> client = make_client(expanded, error);
  if (not client) return fail(ServerState::Failed, error);
  {
    // Visible to shutdown() while the handshake runs, so quitting does not
    // wait out a slow server's startup timeout.
    std::lock_guard<std::mutex> lock(mMutex);
    Server* server = find(name);
    if (server == nullptr or server->epoch != epoch or mShutdown) return;
    server->connecting = client;
  }

  const ConnectResult connected = client->connect();
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (Server* server = find(name); server != nullptr and server->connecting == client) {
      server->connecting.reset();
    }
  }
  if (not connected.ok) {
    std::string why = connected.error;
    const std::string tail = client->stderr_tail();
    if (not tail.empty() and why.find(tail) == std::string::npos) {
      why += "; its last output: " + tail.substr(tail.size() > 400 ? tail.size() - 400 : 0);
    }
    client->close();
    return fail(connected.needs_auth ? ServerState::NeedsAuth : ServerState::Failed, why);
  }

  std::vector<ToolInfo> tools;
  std::vector<PromptInfo> prompts;
  std::vector<std::string> notes;
  const ServerCapabilities capabilities = client->capabilities();
  if (capabilities.tools and not client->list_tools(tools, error)) {
    notes.push_back("could not list tools: " + error);
  }
  if (capabilities.prompts and not client->list_prompts(prompts, error)) {
    notes.push_back("could not list prompts: " + error);
  }

  std::vector<CatalogTool> catalog_tools;
  for (const ToolInfo& tool : tools) {
    if (not config.enabled_tools.empty() and not listed(config.enabled_tools, tool.name)) {
      continue;
    }
    if (listed(config.disabled_tools, tool.name)) continue;
    CatalogTool built;
    std::string problem;
    if (make_catalog_tool(name, tool, config.always_load, config.type == "http",
                          built, problem)) {
      catalog_tools.push_back(std::move(built));
    } else {
      notes.push_back("tool '" + tool.name + "' skipped: " + problem);
    }
  }

  bool orphaned = false;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    Server* server = find(name);
    if (server == nullptr or server->epoch != epoch or mShutdown) {
      orphaned = true;
    } else {
      server->client = client;
      server->state = ServerState::Connected;
      server->error.clear();
      server->tools = std::move(catalog_tools);
      server->prompts = std::move(prompts);
      server->listed_at = Clock::now();
      server->ttl_ms = client->tools_ttl_ms();
    }
  }
  if (orphaned) {
    client->close();
    return;
  }

  mState.set_era(config, client->era());
  std::string save_error;
  mState.save(save_error);

  size_t tool_count = 0;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (const Server* server = find(name)) tool_count = server->tools.size();
  }
  std::string text = "mcp: " + name + " connected (" + std::to_string(tool_count) +
                     (tool_count == 1 ? " tool" : " tools") + ")";
  for (const std::string& note : notes) text += "\n  " + note;
  const RegistryEvent event{name, ServerState::Connected, text};
  publish(&event);
}

void Registry::schedule_refresh(const std::string& name) {
  {
    std::lock_guard<std::mutex> lock(mMutex);
    Server* server = find(name);
    if (server == nullptr or server->refreshing or mShutdown) return;
    server->refreshing = true;
  }
  spawn([this, name] {
    // A server announcing several changes at once gets one re-list.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    refresh(name);
  });
}

void Registry::refresh(const std::string& name) {
  std::shared_ptr<Client> client;
  ServerConfig config;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    Server* server = find(name);
    if (server == nullptr) return;
    server->refreshing = false;
    if (server->state != ServerState::Connected or mShutdown) return;
    client = server->client;
    config = server->config;
  }
  if (not client) return;

  std::vector<ToolInfo> tools;
  std::vector<PromptInfo> prompts;
  std::string error;
  const ServerCapabilities capabilities = client->capabilities();
  if (capabilities.tools and not client->list_tools(tools, error)) return;
  if (capabilities.prompts) client->list_prompts(prompts, error);

  std::vector<CatalogTool> catalog_tools;
  for (const ToolInfo& tool : tools) {
    if (not config.enabled_tools.empty() and not listed(config.enabled_tools, tool.name)) {
      continue;
    }
    if (listed(config.disabled_tools, tool.name)) continue;
    CatalogTool built;
    std::string problem;
    if (make_catalog_tool(name, tool, config.always_load, config.type == "http",
                          built, problem)) {
      catalog_tools.push_back(std::move(built));
    }
  }
  {
    std::lock_guard<std::mutex> lock(mMutex);
    Server* server = find(name);
    if (server == nullptr or server->client != client) return;
    server->tools = std::move(catalog_tools);
    server->prompts = std::move(prompts);
    server->listed_at = Clock::now();
    server->ttl_ms = client->tools_ttl_ms();
  }
  publish();
}

void Registry::refresh_stale() {
  std::vector<std::string> stale;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    const Clock::time_point now = Clock::now();
    for (const std::unique_ptr<Server>& server : mServers) {
      if (server->state != ServerState::Connected or server->ttl_ms <= 0) continue;
      if (now - server->listed_at > std::chrono::milliseconds(server->ttl_ms)) {
        stale.push_back(server->config.name);
      }
    }
  }
  for (const std::string& name : stale) schedule_refresh(name);
}

void Registry::publish(const RegistryEvent* event) {
  std::lock_guard<std::mutex> publish_lock(mPublishMutex);
  Catalog catalog;
  RegistryObserver observer;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    catalog.generation = ++mGeneration;
    for (const std::unique_ptr<Server>& server : mServers) {
      CatalogServer entry;
      const ServerConfig& config = server->config;
      entry.name = config.name;
      entry.scope = scope_name(config.scope);
      entry.transport = config.type.empty() ? "?" : config.type;
      entry.detail = config.type == "http" ? config.url : join_command(config);
      entry.state = server->state;
      entry.error = server->error;
      entry.always_load = config.always_load;
      if (server->client) {
        entry.era = server->client->era();
        entry.protocol_version = server->client->protocol_version();
        entry.info = server->client->info();
        entry.instructions = server->client->instructions();
        entry.capabilities = server->client->capabilities();
        if (server->state != ServerState::Connected) {
          entry.stderr_tail = server->client->stderr_tail();
        }
      }
      if (server->state == ServerState::Connected) {
        entry.prompts = server->prompts;
        for (const CatalogTool& tool : server->tools) catalog.tools.push_back(tool);
      }
      catalog.servers.push_back(std::move(entry));
    }
    observer = mObserver;
  }
  std::shared_ptr<const Catalog> snapshot = finalize_catalog(std::move(catalog));
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mSnapshot = std::move(snapshot);
  }
  mChanged.notify_all();
  if (event != nullptr and observer) observer(*event);
}

std::shared_ptr<const Catalog> Registry::snapshot() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mSnapshot;
}

std::shared_ptr<const Catalog> Registry::wait_until_settled(
    std::chrono::milliseconds limit) {
  std::unique_lock<std::mutex> lock(mMutex);
  mChanged.wait_for(lock, limit, [&] {
    if (mShutdown) return true;
    return std::none_of(mServers.begin(), mServers.end(),
                        [](const std::unique_ptr<Server>& server) {
                          return server->state == ServerState::Connecting;
                        });
  });
  return mSnapshot;
}

tools::ToolResult Registry::call_tool(const std::string& exposed,
                                      std::string_view arguments_json,
                                      const CallContext& context) {
  const std::shared_ptr<const Catalog> catalog = snapshot();
  const CatalogTool* tool = catalog->find(exposed);
  if (tool == nullptr) {
    return failure("no MCP tool named '" + exposed +
                   "' is available (its server may have disconnected); use "
                   "tool_search to find tools");
  }

  std::shared_ptr<Client> client;
  ServerState state = ServerState::Failed;
  std::string server_error;
  bool http = false;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (const Server* server = find(tool->server)) {
      client = server->client;
      state = server->state;
      server_error = server->error;
      http = server->config.type == "http";
    }
  }
  if (state != ServerState::Connected or not client) {
    return failure("the MCP server '" + tool->server + "' is " + state_name(state) +
                   (server_error.empty() ? std::string() : ": " + server_error));
  }

  std::string error;
  util::JsonValue arguments = arguments_object(arguments_json, error);
  if (not error.empty()) return failure(error);
  arguments = fix_arguments(arguments, tool->parameters);
  const std::vector<std::pair<std::string, std::string>> headers =
      http ? header_values(tool->header_params, arguments)
           : std::vector<std::pair<std::string, std::string>>{};

  const CallOutcome outcome = client->call_tool(tool->name, arguments, context, headers);
  if (not outcome.ok) {
    if (outcome.needs_auth) {
      {
        std::lock_guard<std::mutex> lock(mMutex);
        if (Server* server = find(tool->server)) {
          server->state = ServerState::NeedsAuth;
          server->error = outcome.error;
        }
      }
      const RegistryEvent event{tool->server, ServerState::NeedsAuth,
                                "mcp: " + tool->server +
                                    " needs you to log in — /mcp login " + tool->server};
      publish(&event);
      return failure("the MCP server '" + tool->server +
                     "' needs the user to log in (/mcp login " + tool->server +
                     "); tell the user");
    }
    std::string message = "MCP server '" + tool->server + "': " + outcome.error;
    if (outcome.rpc_code == kInvalidParams) {
      message += ". Expected: " + tool->signature;
    }
    return failure(message);
  }

  RenderOptions options;
  options.label = "mcp-" + tool->server;
  const Rendered rendered = render_tool_result(outcome.result, options);
  tools::ToolResult result;
  result.ok = not rendered.is_error;
  (result.ok ? result.output : result.error) = rendered.text;
  result.truncated = rendered.truncated;
  result.overflow_path = rendered.overflow_path;
  return result;
}

tools::ToolResult Registry::resource(std::string_view action,
                                     const std::string& server_name,
                                     const std::string& uri,
                                     const CallContext& context) {
  std::vector<std::pair<std::string, std::shared_ptr<Client>>> targets;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    for (const std::unique_ptr<Server>& server : mServers) {
      if (server->state != ServerState::Connected or not server->client) continue;
      if (not server->client->capabilities().resources) continue;
      if (not server_name.empty() and server->config.name != server_name) continue;
      targets.emplace_back(server->config.name, server->client);
    }
  }
  if (targets.empty()) {
    return failure(server_name.empty()
                       ? std::string("no connected MCP server offers resources")
                       : "the MCP server '" + server_name +
                             "' is not connected or offers no resources");
  }

  if (action == "read") {
    if (uri.empty()) return failure("action \"read\" needs a uri");
    if (targets.size() > 1) {
      return failure("several MCP servers offer resources; say which with \"server\"");
    }
    const CallOutcome outcome = targets.front().second->read_resource(uri, context);
    if (not outcome.ok) return failure(outcome.error);
    RenderOptions options;
    options.label = "mcp-" + targets.front().first;
    const Rendered rendered = render_resource_contents(outcome.result, options);
    tools::ToolResult result;
    result.ok = true;
    result.output = rendered.text;
    result.truncated = rendered.truncated;
    result.overflow_path = rendered.overflow_path;
    return result;
  }

  const bool templates = action == "templates";
  if (action != "list" and not templates) {
    return failure("action must be \"list\", \"templates\" or \"read\"");
  }
  std::string out;
  for (const auto& [name, client] : targets) {
    std::string error;
    out += name + ":\n";
    size_t shown = 0;
    if (templates) {
      std::vector<ResourceTemplateInfo> list;
      if (not client->list_resource_templates(list, error)) {
        out += "  (could not list: " + error + ")\n";
        continue;
      }
      for (const ResourceTemplateInfo& item : list) {
        if (++shown > 50) {
          out += "  ... " + std::to_string(list.size() - 50) + " more\n";
          break;
        }
        out += "  - " + item.uri_template;
        if (not item.name.empty()) out += " — " + item.name;
        if (not item.description.empty()) out += ": " + item.description;
        out += "\n";
      }
      if (list.empty()) out += "  (no templates)\n";
    } else {
      std::vector<ResourceInfo> list;
      if (not client->list_resources(list, error)) {
        out += "  (could not list: " + error + ")\n";
        continue;
      }
      for (const ResourceInfo& item : list) {
        if (++shown > 50) {
          out += "  ... " + std::to_string(list.size() - 50) + " more\n";
          break;
        }
        out += "  - " + item.uri;
        if (not item.name.empty()) out += " — " + item.name;
        if (not item.mime_type.empty()) out += " (" + item.mime_type + ")";
        if (not item.description.empty()) out += ": " + item.description;
        out += "\n";
      }
      if (list.empty()) out += "  (no resources)\n";
    }
  }
  tools::ToolResult result;
  result.ok = true;
  result.output = out;
  return result;
}

bool Registry::approve(const std::string& name, std::string& error) {
  {
    std::lock_guard<std::mutex> lock(mMutex);
    Server* server = find(name);
    if (server == nullptr) {
      error = "no MCP server named '" + name + "'";
      return false;
    }
    if (not needs_approval(server->config)) {
      error = "'" + name + "' comes from your own ~/.m8/mcp.json and needs no approval";
      return false;
    }
    mTrust.approve(mOptions.workspace, server->config);
  }
  if (not mTrust.save(error)) return false;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    Server* server = find(name);
    if (server != nullptr and server->state == ServerState::NeedsApproval) {
      start_connect(*server);
    }
  }
  publish();
  return true;
}

bool Registry::set_enabled(const std::string& name, bool enabled, std::string& error) {
  std::shared_ptr<Client> stopping;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    Server* server = find(name);
    if (server == nullptr) {
      error = "no MCP server named '" + name + "'";
      return false;
    }
    mState.set_enabled(name, enabled);
    if (not enabled) {
      ++server->epoch;  // orphan any connect in flight
      stopping = std::move(server->client);
      server->state = ServerState::Disabled;
      server->tools.clear();
      server->prompts.clear();
    } else if (server->state == ServerState::Disabled) {
      if (needs_approval(server->config) and
          not mTrust.approved(mOptions.workspace, server->config)) {
        server->state = ServerState::NeedsApproval;
      } else {
        start_connect(*server);
      }
    }
  }
  if (stopping) spawn([stopping] { stopping->close(); });
  publish();
  return mState.save(error);
}

void Registry::reconnect(const std::string& name) {
  std::vector<std::shared_ptr<Client>> stopping;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    for (const std::unique_ptr<Server>& server : mServers) {
      if (not name.empty() and server->config.name != name) continue;
      if (server->state == ServerState::Disabled or
          server->state == ServerState::NeedsApproval or
          not server->config.problem.empty()) {
        continue;
      }
      if (server->client) stopping.push_back(std::move(server->client));
      server->tools.clear();
      server->prompts.clear();
      start_connect(*server);
    }
  }
  for (std::shared_ptr<Client>& client : stopping) {
    spawn([client] { client->close(); });
  }
  publish();
}

std::vector<std::string> Registry::pending_approval() const {
  std::lock_guard<std::mutex> lock(mMutex);
  std::vector<std::string> out;
  for (const std::unique_ptr<Server>& server : mServers) {
    if (server->state == ServerState::NeedsApproval) out.push_back(server->config.name);
  }
  return out;
}

std::optional<ServerConfig> Registry::config(const std::string& name) const {
  std::lock_guard<std::mutex> lock(mMutex);
  if (const Server* server = find(name)) return server->config;
  return std::nullopt;
}

CallOutcome Registry::get_prompt(const std::string& server_name,
                                 const std::string& prompt,
                                 const util::JsonValue& arguments,
                                 const CallContext& context) {
  std::shared_ptr<Client> client;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (const Server* server = find(server_name);
        server != nullptr and server->state == ServerState::Connected) {
      client = server->client;
    }
  }
  if (not client) {
    CallOutcome outcome;
    outcome.error = "the MCP server '" + server_name + "' is not connected";
    return outcome;
  }
  return client->get_prompt(prompt, arguments, context);
}

void Registry::shutdown() {
  std::vector<std::shared_ptr<Client>> clients;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mShutdown) return;
    mShutdown = true;
    for (const std::unique_ptr<Server>& server : mServers) {
      ++server->epoch;
      if (server->client) clients.push_back(server->client);
      if (server->connecting) clients.push_back(server->connecting);
    }
    mObserver = nullptr;
    mElicit = nullptr;
  }
  mChanged.notify_all();
  // Begin every ladder before waiting on any: servers stop in parallel.
  for (const std::shared_ptr<Client>& client : clients) client->begin_close();
  for (const std::shared_ptr<Client>& client : clients) client->wait_closed();

  std::vector<Task> tasks;
  {
    std::lock_guard<std::mutex> lock(mTaskMutex);
    tasks.swap(mTasks);
  }
  for (Task& task : tasks) {
    if (task.thread.joinable()) task.thread.join();
  }
  // A connect that was mid-handshake finished after the clients above were
  // collected; close whatever it left behind.
  std::vector<std::shared_ptr<Client>> late;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    for (const std::unique_ptr<Server>& server : mServers) {
      if (server->client) late.push_back(std::move(server->client));
    }
  }
  for (const std::shared_ptr<Client>& client : late) client->close();
}

}  // namespace mcp
