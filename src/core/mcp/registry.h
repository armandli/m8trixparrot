#ifndef M8_MCP_REGISTRY_H
#define M8_MCP_REGISTRY_H

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <core/mcp/client.h>
#include <core/mcp/config.h>
#include <core/mcp/oauth.h>
#include <core/mcp/toolbox.h>
#include <core/mcp/trust.h>

namespace mcp {

struct RegistryOptions {
  // The workspace root's real path: the key approvals are kept under, and the
  // directory a stdio server runs in unless its config names another.
  std::string workspace;
  std::string logs_dir;    // one stderr log per stdio server; "" for none
  std::string state_path;  // .m8/mcp_state.json; "" to remember nothing
  std::string trust_path;  // ~/.m8/mcp_trust.json; "" to approve nothing
  // ~/.m8/mcp_credentials.json; "" for no OAuth at all.
  std::string credentials_path;
  // Where m8's OAuth Client ID Metadata Document is hosted, if anywhere.
  std::string client_metadata_url;

  std::chrono::milliseconds probe_timeout{2000};
  std::chrono::milliseconds startup_timeout{30000};
  std::chrono::milliseconds list_timeout{30000};
  std::chrono::milliseconds call_timeout{300000};
  std::chrono::milliseconds read_timeout{60000};
  std::chrono::milliseconds close_grace{1500};
  std::chrono::milliseconds term_grace{1000};
};

struct RegistryEvent {
  std::string server;
  ServerState state = ServerState::Connecting;
  std::string text;  // a one-line notice for the user
};

using RegistryObserver = std::function<void(const RegistryEvent&)>;

// A call found the server wants a login (or more permissions): asks the
// person, and logs in if they agree. True when there is a new login to retry
// the call with. Runs on the calling agent's thread.
using LoginPrompt = std::function<bool(const std::string& server, const std::string& reason)>;

// Builds the transport for an HTTP server. Unset, the registry uses
// make_http_transport (http_transport.h); an app installs its own to wire in
// credentials, and tests to observe the traffic.
using HttpTransportFactory = std::function<std::unique_ptr<Transport>(
    const ServerConfig& expanded, TransportHandlers handlers)>;

// Every configured MCP server, connected in the background, behind one
// catalog snapshot.
//
// Lifetime: an app creates one and deliberately never frees it, because agent
// threads are detached and may still hold it (through AgentOptions::mcp) as
// the process exits; shutdown() stops the servers and joins every thread the
// registry started, after which calls fail cleanly.
//
// Locking: callbacks (the observer, elicitation) are never invoked while the
// registry holds its own lock — m8's observer takes the UI lock, and the agent
// pool calls observers under its own.
struct Registry : Toolbox {
  explicit Registry(RegistryOptions options);
  ~Registry() override;

  Registry(const Registry&) = delete;
  Registry& operator=(const Registry&) = delete;

  // Set before start().
  void set_observer(RegistryObserver observer);
  void set_elicitation_handler(ElicitationHandler handler);
  void set_http_factory(HttpTransportFactory factory);
  void set_login_prompt(LoginPrompt prompt);

  // Records the servers and starts connecting the enabled, approved ones on
  // background threads. Returns at once.
  void start(std::vector<ServerConfig> servers);

  // ── Toolbox ──
  std::shared_ptr<const Catalog> snapshot() const override;
  std::shared_ptr<const Catalog> wait_until_settled(
      std::chrono::milliseconds limit) override;
  tools::ToolResult call_tool(const std::string& exposed,
                              std::string_view arguments_json,
                              const CallContext& context) override;
  tools::ToolResult resource(std::string_view action, const std::string& server,
                             const std::string& uri,
                             const CallContext& context) override;

  // ── management: the /mcp commands ──
  // Approves a workspace server (recorded in the trust store) and starts it.
  bool approve(const std::string& name, std::string& error);
  // Remembered per workspace in the state file; connects or stops at once.
  bool set_enabled(const std::string& name, bool enabled, std::string& error);
  // Drops and re-establishes a connection ("" for every server).
  void reconnect(const std::string& name);
  // Names of workspace servers waiting for approval.
  std::vector<std::string> pending_approval() const;
  // The configured entry, as loaded (nullopt if unknown).
  std::optional<ServerConfig> config(const std::string& name) const;

  // Logs in to a remote server with OAuth. `show` gets the authorization URL
  // to open or print; this then blocks until the browser comes back (five
  // minutes at most), `cancel` turns true, or shutdown(). A server that was
  // waiting for the login is reconnected (and waited for) before it returns.
  bool login(const std::string& name, const std::function<void(const std::string& url)>& show,
             const std::atomic<bool>* cancel, std::string& error);
  // Forgets the server's tokens and reconnects it (it will ask again).
  bool logout(const std::string& name, std::string& error);

  CallOutcome get_prompt(const std::string& server, const std::string& prompt,
                         const util::JsonValue& arguments,
                         const CallContext& context);

  // Re-lists tools for modern servers whose list has outlived its ttlMs. Cheap
  // when nothing is stale; m8 calls it at the start of each turn.
  void refresh_stale();

  // Closes every server (in parallel) and joins every thread. Idempotent.
  void shutdown();

  const RegistryOptions& options() const { return mOptions; }

private:
  struct Server {
    ServerConfig config;  // as loaded: unexpanded
    ServerState state = ServerState::Connecting;
    std::string error;
    std::shared_ptr<Client> client;
    std::shared_ptr<Client> connecting;  // mid-handshake, for shutdown()
    std::vector<CatalogTool> tools;
    std::vector<PromptInfo> prompts;
    Clock::time_point listed_at{};
    int64_t ttl_ms = 0;
    bool refreshing = false;
    uint64_t epoch = 0;  // bumped to orphan an in-flight connect
  };

  struct Task {
    std::thread thread;
    std::shared_ptr<std::atomic<bool>> done;
  };

  Server* find(const std::string& name);  // caller holds mMutex
  const Server* find(const std::string& name) const;
  void spawn(std::function<void()> work);
  void start_connect(Server& server);  // caller holds mMutex
  void connect(const std::string& name, uint64_t epoch);
  void refresh(const std::string& name);
  void schedule_refresh(const std::string& name);
  std::shared_ptr<Client> make_client(const ServerConfig& expanded,
                                      std::string& error);
  // The server's OAuth session, made on first use; null when OAuth does not
  // apply (stdio, a fixed Authorization header, no credential file).
  std::shared_ptr<OAuthSession> oauth_session(const ServerConfig& expanded);
  std::shared_ptr<OAuthSession> existing_oauth_session(const std::string& name);
  // Rebuilds the snapshot and tells the observer what changed.
  void publish(const RegistryEvent* event = nullptr);

  RegistryOptions mOptions;
  TrustStore mTrust;
  McpState mState;

  mutable std::mutex mMutex;
  std::condition_variable mChanged;
  std::vector<std::unique_ptr<Server>> mServers;
  std::shared_ptr<const Catalog> mSnapshot;
  uint64_t mGeneration = 0;
  bool mShutdown = false;
  RegistryObserver mObserver;
  ElicitationHandler mElicit;
  HttpTransportFactory mHttpFactory;
  LoginPrompt mLoginPrompt;
  std::map<std::string, std::shared_ptr<OAuthSession>> mOAuth;
  std::vector<std::weak_ptr<PendingLogin>> mLogins;  // cancelled by shutdown()

  std::mutex mPublishMutex;  // keeps snapshots and events in order
  std::mutex mTaskMutex;
  std::vector<Task> mTasks;
};

}  // namespace mcp

#endif  // M8_MCP_REGISTRY_H
