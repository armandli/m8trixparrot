#ifndef M8_MCP_CLIENT_H
#define M8_MCP_CLIENT_H

#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <core/mcp/protocol.h>
#include <core/mcp/transport.h>

namespace mcp {

struct ClientOptions {
  std::string server;  // the configured name

  // Builds a fresh transport; called on connect and again to restart a server
  // that died. The handlers are the client's own.
  std::function<std::unique_ptr<Transport>(TransportHandlers)> make_transport;
  bool http = false;  // era detection differs (see connect())

  // "auto" detects the era; "modern" and "legacy" skip the probe.
  std::string protocol = "auto";
  // A previous run's answer for this exact configuration (or Unknown).
  Era remembered_era = Era::Unknown;

  std::chrono::milliseconds startup_timeout{30000};
  std::chrono::milliseconds probe_timeout{2000};
  std::chrono::milliseconds list_timeout{30000};
  std::chrono::milliseconds call_timeout{300000};
  std::chrono::milliseconds read_timeout{60000};  // resources/read, prompts/get

  // Restarts allowed within `restart_window` before the server is left down.
  int max_restarts = 3;
  std::chrono::seconds restart_window{300};

  // Unset: no elicitation capability is declared, so servers do not ask.
  ElicitationHandler elicit;
  // A server said its tools, prompts or resources changed.
  std::function<void()> on_list_changed;
};

struct ConnectResult {
  bool ok = false;
  std::string error;
  bool needs_auth = false;       // HTTP 401
  std::string www_authenticate;  // the challenge, for the OAuth flow
};

// The outcome of one tools/call, resources/read or prompts/get. `result` is
// the final (complete) result object; a tool that failed on its own terms is
// still ok here, with result.isError set.
struct CallOutcome {
  bool ok = false;
  util::JsonValue result;
  std::string error;
  int64_t rpc_code = 0;  // when the server answered with a JSON-RPC error
  bool timed_out = false;
  bool needs_auth = false;
  std::string www_authenticate;
};

// One connection to one MCP server, of either era.
//
// Era detection follows the 2026-07-28 spec. Over stdio the client probes with
// `server/discover`: a DiscoverResult, or one of the error codes only a modern
// server uses, means modern; any other error means legacy, and the client
// falls back to the `initialize` handshake. A server that does not answer the
// probe within probe_timeout is sent `initialize` too, and whichever answer
// arrives first decides, so a modern server slow to start (npx downloading a
// package) is not mistaken for a silent legacy one. Over HTTP the first modern
// request is the probe.
//
// Thread-safe: any number of agents may call the same server at once.
struct Client {
  explicit Client(ClientOptions options);
  ~Client();

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

  ConnectResult connect();

  bool list_tools(std::vector<ToolInfo>& out, std::string& error);
  bool list_resources(std::vector<ResourceInfo>& out, std::string& error);
  bool list_resource_templates(std::vector<ResourceTemplateInfo>& out,
                               std::string& error);
  bool list_prompts(std::vector<PromptInfo>& out, std::string& error);

  CallOutcome call_tool(
      const std::string& name, const util::JsonValue& arguments,
      const CallContext& context,
      const std::vector<std::pair<std::string, std::string>>& headers = {},
      const std::atomic<bool>* cancel = nullptr);
  CallOutcome read_resource(const std::string& uri, const CallContext& context);
  CallOutcome get_prompt(const std::string& name,
                         const util::JsonValue& arguments,
                         const CallContext& context);

  void begin_close();
  void wait_closed();
  void close() {
    begin_close();
    wait_closed();
  }

  Era era() const;
  std::string protocol_version() const;
  ServerInfo info() const;
  ServerCapabilities capabilities() const;
  std::string instructions() const;
  bool alive() const;
  std::string stderr_tail() const;
  // The freshness hint (ms) of the last tools/list; 0 when none was given.
  int64_t tools_ttl_ms() const;

private:
  struct Handshake {
    bool ok = false;
    std::string error;
    bool needs_auth = false;
    std::string www_authenticate;
  };

  Handshake handshake(Transport& transport);
  Handshake legacy_initialize(Transport& transport, Clock::time_point deadline);
  Handshake accept_discover(const RpcMessage& message);
  Handshake accept_initialize(Transport& transport, const RpcMessage& message);

  // The client half of `_meta`, on every modern request.
  util::JsonValue request_meta(bool progress = false,
                               const util::JsonValue& id = {}) const;
  util::JsonValue client_capabilities() const;

  // One request on the current transport, restarting a dead server first.
  Reply request(const std::string& method, util::JsonValue params,
                std::chrono::milliseconds timeout, const std::string& name = "",
                const std::vector<std::pair<std::string, std::string>>& headers = {},
                const std::atomic<bool>* cancel = nullptr);
  bool ensure_connected(std::string& error);

  // Fetches every page of a list method into `items`.
  bool list_all(const std::string& method, const char* key,
                std::vector<util::JsonValue>& items, std::string& error);

  // tools/call, resources/read, prompts/get: the multi-round-trip loop.
  CallOutcome call_with_input(
      const std::string& method, util::JsonValue params, const std::string& name,
      std::chrono::milliseconds timeout, const CallContext& context,
      const std::vector<std::pair<std::string, std::string>>& headers,
      const std::atomic<bool>* cancel);
  bool answer_input_requests(const util::JsonValue& requests,
                             const CallContext& context,
                             util::JsonValue& responses, std::string& error);
  ElicitationResult elicit(const util::JsonValue& params,
                           const CallContext& context);

  // Legacy server-to-client requests, from the transport's worker thread.
  void on_server_request(const RpcMessage& request, util::JsonValue& result,
                         RpcError& error);
  void on_notification(const RpcMessage& message);

  util::JsonValue next_id();

  ClientOptions mOptions;
  std::atomic<int64_t> mNextId{1};

  mutable std::mutex mMutex;  // guards the transport pointer and server facts
  std::shared_ptr<Transport> mTransport;
  // The transport a connect() is still shaking hands over, so begin_close()
  // can abort a handshake instead of waiting out its timeout.
  std::shared_ptr<Transport> mConnecting;
  bool mClosing = false;
  Era mEra = Era::Unknown;
  std::string mVersion;
  ServerInfo mInfo;
  ServerCapabilities mCapabilities;
  std::string mInstructions;
  int64_t mToolsTtlMs = 0;
  std::deque<Clock::time_point> mRestarts;
  std::mutex mConnectMutex;  // one (re)connect at a time
};

}  // namespace mcp

#endif  // M8_MCP_CLIENT_H
