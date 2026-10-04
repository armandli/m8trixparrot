#ifndef M8_MCP_HTTP_TRANSPORT_H
#define M8_MCP_HTTP_TRANSPORT_H

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <core/mcp/transport.h>

namespace mcp {

struct ServerConfig;  // config.h

struct HttpConfig {
  std::string server;  // the configured name, for messages
  std::string url;     // the MCP endpoint
  // Sent with every request: the configured headers, already expanded.
  std::vector<std::pair<std::string, std::string>> headers;
  // An Authorization value for each request ("Bearer ..."), or "" for none:
  // where OAuth plugs in. Not consulted when `headers` has an Authorization.
  // Called from any thread that makes a request.
  std::function<std::string()> authorization;

  std::chrono::milliseconds connect_timeout{10000};
  // For what the transport sends on its own: notifications, answers to a
  // server's requests, and (legacy) the session DELETE on close.
  std::chrono::milliseconds message_timeout{10000};
  std::chrono::milliseconds close_timeout{1000};
  size_t max_body_bytes = 64u << 20;
  size_t max_event_bytes = 32u << 20;
};

// MCP over Streamable HTTP, in both of its shapes.
//
// 2026-07-28: every request is its own POST, its metadata mirrored into
// headers (MCP-Protocol-Version, Mcp-Method, Mcp-Name, Mcp-Param-*). The answer
// is one JSON body or an SSE stream of related notifications that ends with
// the response; closing that stream is what cancels the request. There are no
// sessions and no GET stream.
//
// 2025-03-26 to 2025-11-25, after `initialize`: the server may hand out an
// Mcp-Session-Id that every later request carries, ended with a DELETE on
// close. A 404 on a request carrying it means the server forgot the session —
// the request never ran, the transport reports itself dead, and the client
// starts a new session and sends it again. A server may send requests
// (elicitation, ping) on a POST's stream; they are answered from a worker with
// a POST of their own, and the requests waiting meanwhile do not time out
// while a person answers. Cancelling takes an explicit notifications/cancelled.
// A stream the server closes before answering is resumed with a GET carrying
// Last-Event-ID, after the server's `retry` delay (2025-11-25's polling).
//
// A request runs on the thread that sends it — send() returns a future that is
// already resolved — on libcurl's multi interface, polled in short slices so a
// cancel, a deadline or close() ends it promptly. Redirects are not followed:
// a server that moved should say so, rather than m8 quietly sending its
// headers, credentials included, somewhere else.
struct HttpTransport : Transport {
  HttpTransport(HttpConfig config, TransportHandlers handlers);
  ~HttpTransport() override;

  HttpTransport(const HttpTransport&) = delete;
  HttpTransport& operator=(const HttpTransport&) = delete;

  // Checks the URL and starts the worker; no network I/O.
  bool start(std::string& error) override;
  ReplyFuture send(RequestSpec spec) override;
  // POSTs it on the calling thread, bounded by message_timeout.
  bool notify(const std::string& method, const util::JsonValue& params) override;
  void begin_close() override;
  void wait_closed() override;
  bool alive() const override;
  void set_protocol(Era era, const std::string& version) override;
  std::string exit_reason() const override;

  std::string session_id() const;

private:
  struct Lane;
  struct Exchange;
  enum struct Stop : uint8_t { None, Answered, Cancelled, Deadline, Closing };
  enum struct Verb : uint8_t { Post, Get, Delete };

  Reply exchange(const RequestSpec& spec);
  // A notification or an answer to the server: a POST expecting 202.
  bool post_message(const std::string& body, const std::string& method,
                    std::string& error);
  void cancel_on_wire(const util::JsonValue& id, const std::string& reason);

  // One HTTP transfer. `closable` false lets the session DELETE run after
  // close() has stopped everything else.
  Stop transfer(Exchange& exchange, Verb verb, const std::string& body,
                const std::vector<std::pair<std::string, std::string>>& extra,
                bool closable, std::string& error);
  std::unique_ptr<Lane> acquire_lane();
  void release_lane(std::unique_ptr<Lane> lane);

  // The headers every request carries for the era in force.
  std::vector<std::pair<std::string, std::string>> protocol_headers(
      const std::string& method, bool initialize) const;

  void on_event(Exchange& exchange, const std::string& data);
  bool late(const Exchange& exchange) const;
  void worker_loop();
  void delete_session();

  HttpConfig mConfig;
  TransportHandlers mHandlers;
  bool mStarted = false;

  mutable std::mutex mMutex;
  Era mEra = Era::Unknown;
  std::string mVersion;
  std::string mSessionId;
  bool mClosing = false;
  bool mDead = false;
  std::string mExitReason;
  int mInFlight = 0;
  std::condition_variable mIdle;
  std::vector<Lane*> mActive;  // woken by begin_close()
  std::vector<std::unique_ptr<Lane>> mIdleLanes;
  // While a person answers a server's request, nothing times out; the pause
  // is credited to every request that was waiting through it.
  int mInteractions = 0;
  Clock::time_point mInteractionStarted;
  Clock::duration mPaused{};

  std::deque<RpcMessage> mInbox;  // legacy server requests
  std::condition_variable mInboxCv;
  std::thread mWorker;
  std::thread mCloser;
};

// The transport for an `http` server entry, already expanded; what the
// registry builds when the app installs no factory of its own.
std::unique_ptr<Transport> make_http_transport(const ServerConfig& expanded,
                                               TransportHandlers handlers);

}  // namespace mcp

#endif  // M8_MCP_HTTP_TRANSPORT_H
