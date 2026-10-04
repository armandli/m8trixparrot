#ifndef M8_MCP_TRANSPORT_H
#define M8_MCP_TRANSPORT_H

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <string>
#include <utility>
#include <vector>

#include <core/mcp/json_rpc.h>
#include <core/mcp/protocol.h>
#include <core/util/json_value.h>

namespace mcp {

using Clock = std::chrono::steady_clock;

// One JSON-RPC request as a transport needs it.
struct RequestSpec {
  util::JsonValue id;
  std::string method;
  util::JsonValue params;
  // Streamable HTTP mirrors these into headers; stdio ignores them.
  std::string name;  // Mcp-Name: the tool, prompt or resource
  std::vector<std::pair<std::string, std::string>> headers;  // Mcp-Param-*
  Clock::time_point deadline = Clock::time_point::max();
  // Set from another thread to give up on the call; the transport cancels it
  // on the wire (notifications/cancelled, or closing the HTTP stream).
  const std::atomic<bool>* cancel = nullptr;
};

// What came back for one request.
struct Reply {
  bool ok = false;      // a JSON-RPC response arrived: `message` is Result/Error
  RpcMessage message;
  std::string error;    // why there is no response
  bool timed_out = false;
  bool cancelled = false;
  bool exited = false;  // stdio: the server process is gone
  // Streamable HTTP only.
  long http_status = 0;
  std::string www_authenticate;
  // A legacy HTTP server no longer knows our session, so it never ran the
  // request: safe to resend once a new session is started.
  bool session_expired = false;
};

using ReplyFuture = std::shared_future<Reply>;

// What a server sends that is not a reply to us.
struct TransportHandlers {
  // Notifications (list_changed, legacy log messages, ...). Called on the
  // transport's own thread: must be quick and must not send requests.
  std::function<void(const RpcMessage&)> on_notification;
  // Server-to-client requests, which only legacy servers send (elicitation,
  // roots, sampling; `ping` is answered by the transport itself). Called on a
  // worker thread, since an elicitation waits on a human. Fills `result`, or
  // sets `error.code` non-zero.
  std::function<void(const RpcMessage& request, util::JsonValue& result,
                     RpcError& error)>
      on_request;
};

// The wire under one MCP client. A transport is single use: once its server
// is gone it stays gone, and the client builds a new one to reconnect.
struct Transport {
  virtual ~Transport() = default;

  virtual bool start(std::string& error) = 0;

  // Sends a request; the future resolves with the reply, a timeout, a
  // cancellation or the server's exit. Over stdio it never blocks on the
  // server, which is what lets the client race two handshakes; Streamable HTTP
  // runs the exchange on the calling thread and returns it resolved.
  virtual ReplyFuture send(RequestSpec spec) = 0;

  // Fire and forget. False if it could not even be queued.
  virtual bool notify(const std::string& method, const util::JsonValue& params) = 0;

  // Shutdown in two halves, so a caller with several servers can begin them
  // all and then wait once: begin_close() returns at once, wait_closed()
  // blocks until the server is gone and every thread is joined.
  virtual void begin_close() = 0;
  virtual void wait_closed() = 0;

  virtual bool alive() const = 0;
  // Which protocol the client settled on. Streamable HTTP needs it for the
  // MCP-Protocol-Version header and for legacy session handling; stdio has no
  // header layer and ignores it.
  virtual void set_protocol(Era era, const std::string& version) {
    (void)era;
    (void)version;
  }
  // What the server said on stderr lately (stdio), for error messages.
  virtual std::string stderr_tail() const { return std::string(); }
  // Why the transport died, once it has.
  virtual std::string exit_reason() const { return std::string(); }

  Reply request(RequestSpec spec) { return send(std::move(spec)).get(); }
  void close() {
    begin_close();
    wait_closed();
  }
};

}  // namespace mcp

#endif  // M8_MCP_TRANSPORT_H
