#ifndef M8_MCP_STDIO_TRANSPORT_H
#define M8_MCP_STDIO_TRANSPORT_H

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <core/mcp/transport.h>
#include <core/mcp/wire_log.h>
#include <core/util/process.h>

namespace mcp {

struct StdioConfig {
  std::string server;  // the configured name, for messages
  std::string command;  // already resolved to a path
  std::vector<std::string> args;
  util::EnvList env;  // the child's whole environment
  std::string cwd;
  std::string log_path;  // where stderr goes; empty for nowhere but the ring
  std::shared_ptr<WireLog> wire;  // M8_MCP_DEBUG's message log, if on

  size_t max_line_bytes = 32u << 20;
  size_t stderr_ring_bytes = 64u << 10;
  size_t log_max_bytes = 1u << 20;
  // The shutdown ladder: close stdin, wait, SIGTERM the group, wait, SIGKILL.
  std::chrono::milliseconds close_grace{1500};
  std::chrono::milliseconds term_grace{1000};
};

// MCP over a child's stdin/stdout: one JSON-RPC message per line.
//
// One I/O thread owns every descriptor. Callers only queue lines and wait on
// futures, so a server that stops reading its stdin stalls nothing but its own
// requests — a blocking write() under a lock would have stalled every caller
// and the shutdown with them. The loop also owns the deadlines (a timed-out
// request is cancelled on the wire with notifications/cancelled), stderr
// (kept in a ring for error messages and written to a log), and the shutdown
// ladder. Server-to-client requests, which can wait on a human, run on a
// separate worker so the loop keeps reading replies meanwhile.
struct StdioTransport : Transport {
  StdioTransport(StdioConfig config, TransportHandlers handlers);
  ~StdioTransport() override;

  StdioTransport(const StdioTransport&) = delete;
  StdioTransport& operator=(const StdioTransport&) = delete;

  bool start(std::string& error) override;
  ReplyFuture send(RequestSpec spec) override;
  bool notify(const std::string& method, const util::JsonValue& params) override;
  void begin_close() override;
  void wait_closed() override;
  bool alive() const override;
  std::string stderr_tail() const override;
  std::string exit_reason() const override;

  pid_t pid() const { return mProcess.pid; }

private:
  struct Pending {
    util::JsonValue id;
    std::promise<Reply> promise;
    Clock::time_point started;
    Clock::time_point deadline;
    Clock::duration timeout{};  // what progress notifications extend by
    const std::atomic<bool>* cancel = nullptr;
  };

  void io_loop();
  void worker_loop();
  void wake();
  void queue_line(std::string line);  // takes mMutex

  // Each reads until the pipe is empty; true at end of stream.
  bool read_stdout();
  bool read_stderr();
  void handle_line(std::string_view line);
  void expire(Clock::time_point now);
  void fail_all(const std::string& why, bool exited);
  void record_stderr(std::string_view text);
  void log(std::string_view text);

  StdioConfig mConfig;
  TransportHandlers mHandlers;
  util::SpawnedProcess mProcess;
  bool mStarted = false;

  mutable std::mutex mMutex;
  std::unordered_map<std::string, std::shared_ptr<Pending>> mPending;
  std::deque<std::string> mOutbox;
  bool mClosing = false;
  bool mDead = false;
  std::string mExitReason;
  std::string mStderrRing;
  // While a server-to-client request is with a human, nothing times out; the
  // pause is added back to every deadline afterwards.
  int mInteractions = 0;
  Clock::time_point mInteractionStarted;

  std::deque<RpcMessage> mInbox;
  std::condition_variable mInboxCv;
  std::thread mWorker;

  std::thread mIo;
  int mWake[2] = {-1, -1};

  // I/O thread only.
  std::string mReadBuffer;
  size_t mScanFrom = 0;
  bool mDroppingLine = false;
  std::string mWriting;
  std::FILE* mLog = nullptr;
  size_t mLogBytes = 0;
};

}  // namespace mcp

#endif  // M8_MCP_STDIO_TRANSPORT_H
