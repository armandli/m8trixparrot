#include <core/mcp/stdio_transport.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <filesystem>

#include <core/util/text.h>

namespace mcp {

namespace {

// A ceiling on how long progress notifications can keep one request alive.
constexpr auto kMaxRequestLifetime = std::chrono::minutes(30);

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

// Opened close-on-exec, so no other child this process starts inherits it.
std::FILE* open_log(const std::string& path, bool truncate) {
  const int fd = ::open(path.c_str(),
                        O_WRONLY | O_CREAT | O_CLOEXEC |
                            (truncate ? O_TRUNC : O_APPEND),
                        0600);
  if (fd < 0) return nullptr;
  std::FILE* file = ::fdopen(fd, truncate ? "w" : "a");
  if (file == nullptr) ::close(fd);
  return file;
}

std::string clipped(std::string_view text, size_t limit) {
  if (text.size() <= limit) return std::string(text);
  return std::string(text.substr(0, limit)) + "...";
}

}  // namespace

StdioTransport::StdioTransport(StdioConfig config, TransportHandlers handlers)
    : mConfig(std::move(config)), mHandlers(std::move(handlers)) {}

StdioTransport::~StdioTransport() {
  begin_close();
  wait_closed();
}

bool StdioTransport::start(std::string& error) {
  if (mStarted) {
    error = "already started";
    return false;
  }
  if (::pipe(mWake) != 0) {
    error = std::string("could not create a wake pipe: ") + std::strerror(errno);
    return false;
  }
  for (const int fd : mWake) {
    util::set_cloexec(fd);
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  }

  if (not mConfig.log_path.empty()) {
    std::error_code ec;
    const std::filesystem::path path(mConfig.log_path);
    std::filesystem::create_directories(path.parent_path(), ec);
    // One rotation, so a server that logs heavily cannot fill the disk but the
    // previous run's log is still there to read.
    if (std::filesystem::exists(path, ec) and
        std::filesystem::file_size(path, ec) > mConfig.log_max_bytes) {
      std::filesystem::rename(path, mConfig.log_path + ".1", ec);
    }
    mLog = open_log(mConfig.log_path, /*truncate=*/false);
    if (mLog != nullptr) {
      std::fseek(mLog, 0, SEEK_END);
      mLogBytes = static_cast<size_t>(std::max<long>(0, std::ftell(mLog)));
    }
  }

  util::SpawnOptions options;
  options.command = mConfig.command;
  options.args = mConfig.args;
  options.env = mConfig.env;
  options.cwd = mConfig.cwd;
  if (not util::spawn_process(options, mProcess, error)) {
    close_fd(mWake[0]);
    close_fd(mWake[1]);
    return false;
  }
  log("--- started " + mConfig.command + " (pid " + std::to_string(mProcess.pid) +
      ") ---");

  mStarted = true;
  mIo = std::thread([this] { io_loop(); });
  mWorker = std::thread([this] { worker_loop(); });
  return true;
}

void StdioTransport::wake() {
  if (mWake[1] < 0) return;
  const char byte = 1;
  // A full wake pipe already guarantees a wake-up; EAGAIN is fine.
  [[maybe_unused]] const ssize_t ignored = ::write(mWake[1], &byte, 1);
}

void StdioTransport::queue_line(std::string line) {
  line += '\n';
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mOutbox.push_back(std::move(line));
  }
  wake();
}

ReplyFuture StdioTransport::send(RequestSpec spec) {
  auto pending = std::make_shared<Pending>();
  pending->id = spec.id;
  pending->started = Clock::now();
  pending->deadline = spec.deadline;
  pending->timeout = spec.deadline == Clock::time_point::max()
                         ? Clock::duration::zero()
                         : spec.deadline - pending->started;
  pending->cancel = spec.cancel;
  ReplyFuture future = pending->promise.get_future().share();

  const std::string key = id_key(spec.id);
  std::string line = make_request(spec.id, spec.method, spec.params);
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (not mStarted or mDead or mClosing) {
      Reply reply;
      reply.exited = mDead;
      reply.error = mExitReason.empty() ? "the server is not running" : mExitReason;
      pending->promise.set_value(std::move(reply));
      return future;
    }
    mPending[key] = pending;
    mOutbox.push_back(line + "\n");
  }
  wake();
  return future;
}

bool StdioTransport::notify(const std::string& method,
                            const util::JsonValue& params) {
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (not mStarted or mDead or mClosing) return false;
  }
  queue_line(make_notification(method, params));
  return true;
}

void StdioTransport::begin_close() {
  {
    std::lock_guard<std::mutex> lock(mMutex);
    if (mClosing) return;
    mClosing = true;
  }
  mInboxCv.notify_all();
  wake();
}

void StdioTransport::wait_closed() {
  if (mIo.joinable()) mIo.join();
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mDead = true;
  }
  mInboxCv.notify_all();
  if (mWorker.joinable()) mWorker.join();
  close_fd(mWake[0]);
  close_fd(mWake[1]);
  if (mLog != nullptr) {
    std::fclose(mLog);
    mLog = nullptr;
  }
}

bool StdioTransport::alive() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mStarted and not mDead and not mClosing;
}

std::string StdioTransport::stderr_tail() const {
  std::lock_guard<std::mutex> lock(mMutex);
  const size_t keep = std::min<size_t>(mStderrRing.size(), 2000);
  return util::sanitize_utf8(
      std::string_view(mStderrRing).substr(mStderrRing.size() - keep));
}

std::string StdioTransport::exit_reason() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mExitReason;
}

void StdioTransport::log(std::string_view text) {
  if (mLog == nullptr) return;
  if (mLogBytes > mConfig.log_max_bytes) {
    std::fclose(mLog);
    std::error_code ec;
    std::filesystem::rename(mConfig.log_path, mConfig.log_path + ".1", ec);
    mLog = open_log(mConfig.log_path, /*truncate=*/true);
    mLogBytes = 0;
    if (mLog == nullptr) return;
  }
  std::fwrite(text.data(), 1, text.size(), mLog);
  std::fputc('\n', mLog);
  std::fflush(mLog);
  mLogBytes += text.size() + 1;
}

void StdioTransport::record_stderr(std::string_view text) {
  if (mLog != nullptr) {
    std::fwrite(text.data(), 1, text.size(), mLog);
    std::fflush(mLog);
    mLogBytes += text.size();
  }
  std::lock_guard<std::mutex> lock(mMutex);
  mStderrRing.append(text);
  if (mStderrRing.size() > mConfig.stderr_ring_bytes) {
    mStderrRing.erase(0, mStderrRing.size() - mConfig.stderr_ring_bytes);
  }
}

void StdioTransport::fail_all(const std::string& why, bool exited) {
  std::unordered_map<std::string, std::shared_ptr<Pending>> failed;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    failed.swap(mPending);
  }
  for (auto& [key, pending] : failed) {
    Reply reply;
    reply.error = why;
    reply.exited = exited;
    pending->promise.set_value(std::move(reply));
  }
}

void StdioTransport::expire(Clock::time_point now) {
  std::vector<std::pair<std::shared_ptr<Pending>, bool>> done;  // (pending, timed_out)
  {
    std::lock_guard<std::mutex> lock(mMutex);
    for (auto it = mPending.begin(); it != mPending.end();) {
      const std::shared_ptr<Pending>& pending = it->second;
      const bool cancelled = pending->cancel != nullptr and pending->cancel->load();
      const bool late = mInteractions == 0 and now >= pending->deadline;
      if (cancelled or late) {
        done.emplace_back(pending, not cancelled);
        util::JsonValue params = util::JsonValue::object();
        params.set("requestId", pending->id);
        params.set("reason", cancelled ? "cancelled by the client" : "timed out");
        mOutbox.push_back(make_notification("notifications/cancelled", params) + "\n");
        it = mPending.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (auto& [pending, timed_out] : done) {
    Reply reply;
    reply.timed_out = timed_out;
    reply.cancelled = not timed_out;
    reply.error = timed_out ? "the server did not answer in time" : "cancelled";
    pending->promise.set_value(std::move(reply));
  }
}

void StdioTransport::handle_line(std::string_view line) {
  if (mConfig.wire) mConfig.wire->write('<', line);
  RpcMessage message = parse_message(line);
  switch (message.kind) {
    case MessageKind::Result:
    case MessageKind::Error: {
      std::shared_ptr<Pending> pending;
      {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mPending.find(id_key(message.id));
        if (it == mPending.end()) return;  // late reply to something given up on
        pending = it->second;
        mPending.erase(it);
      }
      Reply reply;
      reply.ok = true;
      reply.message = std::move(message);
      pending->promise.set_value(std::move(reply));
      return;
    }
    case MessageKind::Request:
      // Liveness checks are answered here: they need no human and must not
      // queue behind an elicitation on the worker.
      if (message.method == "ping") {
        std::lock_guard<std::mutex> lock(mMutex);
        mOutbox.push_back(make_result(message.id, util::JsonValue::object()) + "\n");
        return;
      }
      {
        std::lock_guard<std::mutex> lock(mMutex);
        mInbox.push_back(std::move(message));
      }
      mInboxCv.notify_one();
      return;
    case MessageKind::Notification:
      if (message.method == "notifications/progress") {
        // We send each request's own id as its progress token.
        const std::string key = id_key(message.params.get("progressToken"));
        std::lock_guard<std::mutex> lock(mMutex);
        if (auto it = mPending.find(key); it != mPending.end() and
                                          it->second->timeout > Clock::duration::zero()) {
          Pending& pending = *it->second;
          pending.deadline = std::min(Clock::now() + pending.timeout,
                                      pending.started + kMaxRequestLifetime);
        }
        return;
      }
      if (mHandlers.on_notification) mHandlers.on_notification(message);
      return;
    case MessageKind::Invalid:
    default:
      // Banners, stray prints: not ours to act on, but worth keeping.
      log("[stdout, not JSON-RPC] " + clipped(line, 400));
      return;
  }
}

bool StdioTransport::read_stdout() {
  char chunk[65536];
  while (true) {
    const ssize_t n = ::read(mProcess.stdout_fd, chunk, sizeof(chunk));
    if (n == 0) return true;
    if (n < 0) return errno != EAGAIN and errno != EWOULDBLOCK and errno != EINTR;
    mReadBuffer.append(chunk, static_cast<size_t>(n));

    size_t start = 0;
    while (true) {
      const size_t newline = mReadBuffer.find('\n', std::max(start, mScanFrom));
      if (newline == std::string::npos) break;
      std::string_view line(mReadBuffer.data() + start, newline - start);
      if (not line.empty() and line.back() == '\r') line.remove_suffix(1);
      if (mDroppingLine) {
        mDroppingLine = false;
      } else if (not line.empty()) {
        handle_line(line);
      }
      start = newline + 1;
      mScanFrom = start;
    }
    mReadBuffer.erase(0, start);
    mScanFrom = mReadBuffer.size();

    if (mReadBuffer.size() > mConfig.max_line_bytes) {
      // Its id is somewhere in the part we will not keep, so whichever request
      // it answered can never be matched: fail them all rather than let one of
      // them wait out its deadline.
      mReadBuffer.clear();
      mScanFrom = 0;
      mDroppingLine = true;
      log("[stdout] dropped a message larger than " +
          std::to_string(mConfig.max_line_bytes) + " bytes");
      fail_all("the server sent a message larger than " +
                   std::to_string(mConfig.max_line_bytes >> 20) +
                   " MiB, which was dropped",
               false);
    }
  }
}

bool StdioTransport::read_stderr() {
  char chunk[16384];
  while (true) {
    const ssize_t n = ::read(mProcess.stderr_fd, chunk, sizeof(chunk));
    if (n == 0) return true;
    if (n < 0) return errno != EAGAIN and errno != EWOULDBLOCK and errno != EINTR;
    record_stderr(std::string_view(chunk, static_cast<size_t>(n)));
  }
}

void StdioTransport::io_loop() {
  util::block_sigpipe_on_this_thread();

  bool stdout_eof = false;
  bool stderr_eof = false;
  bool stdin_closed = false;
  Clock::time_point close_started{};
  Clock::time_point term_sent{};
  bool killed = false;
  bool reaped = false;
  int status = 0;

  while (true) {
    bool closing = false;
    bool took = false;
    {
      std::lock_guard<std::mutex> lock(mMutex);
      closing = mClosing;
      if (mWriting.empty() and not mOutbox.empty() and not stdin_closed) {
        mWriting = std::move(mOutbox.front());
        mOutbox.pop_front();
        took = true;
      }
    }
    // mWriting is this thread's alone: logged outside the lock.
    if (took and mConfig.wire) {
      mConfig.wire->write('>', std::string_view(mWriting).substr(0, mWriting.size() - 1));
    }

    const Clock::time_point now = Clock::now();
    if (closing and close_started == Clock::time_point{}) {
      close_started = now;
      // The polite shutdown: a well-behaved server exits on EOF.
      close_fd(mProcess.stdin_fd);
      stdin_closed = true;
      mWriting.clear();
      fail_all("the MCP client is shutting down", false);
    }
    if (closing) {
      if (not reaped) reaped = util::reap(mProcess.pid, status);
      // Done when the server exited and nothing of its group still holds the
      // pipes open. A launcher like npx can exit while a child it started
      // keeps them, which is why the signals go to the whole group.
      if (reaped and stdout_eof and stderr_eof) break;
      if (term_sent == Clock::time_point{} and
          now - close_started >= mConfig.close_grace) {
        ::kill(-mProcess.pid, SIGTERM);
        term_sent = now;
      } else if (term_sent != Clock::time_point{} and not killed and
                 now - term_sent >= mConfig.term_grace) {
        ::kill(-mProcess.pid, SIGKILL);
        killed = true;
      } else if (killed and now - term_sent >= mConfig.term_grace +
                                                  std::chrono::milliseconds(300)) {
        // SIGKILL cannot be refused; whatever still holds a pipe is outside the
        // group (it double-forked) and is not ours to wait for.
        if (not reaped) {
          ::waitpid(mProcess.pid, &status, 0);
          reaped = true;
        }
        break;
      }
    }

    pollfd fds[4];
    nfds_t count = 0;
    fds[count++] = pollfd{mWake[0], POLLIN, 0};
    const int stdout_slot = stdout_eof ? -1 : static_cast<int>(count);
    if (not stdout_eof) fds[count++] = pollfd{mProcess.stdout_fd, POLLIN, 0};
    const int stderr_slot = stderr_eof ? -1 : static_cast<int>(count);
    if (not stderr_eof) fds[count++] = pollfd{mProcess.stderr_fd, POLLIN, 0};
    const int stdin_slot =
        (mWriting.empty() or stdin_closed) ? -1 : static_cast<int>(count);
    if (stdin_slot >= 0) fds[count++] = pollfd{mProcess.stdin_fd, POLLOUT, 0};

    ::poll(fds, count, closing ? 20 : 100);

    if (fds[0].revents != 0) {
      char drain[64];
      while (::read(mWake[0], drain, sizeof(drain)) > 0) {
      }
    }
    if (stdout_slot >= 0 and fds[stdout_slot].revents != 0) {
      stdout_eof = read_stdout();
    }
    if (stderr_slot >= 0 and fds[stderr_slot].revents != 0) {
      stderr_eof = read_stderr();
    }
    if (stdin_slot >= 0 and fds[stdin_slot].revents != 0) {
      const ssize_t n =
          ::write(mProcess.stdin_fd, mWriting.data(), mWriting.size());
      if (n > 0) {
        mWriting.erase(0, static_cast<size_t>(n));
      } else if (n < 0 and errno != EAGAIN and errno != EINTR) {
        // EPIPE: the server stopped reading for good. Its stdout will tell us
        // whether it also exited.
        stdin_closed = true;
        mWriting.clear();
        std::lock_guard<std::mutex> lock(mMutex);
        mOutbox.clear();
      }
    }

    expire(Clock::now());

    if (stdout_eof and not closing) {
      // The server closed its stdout: it is exiting, or gone already.
      for (int i = 0; i < 50 and not reaped; ++i) {
        reaped = util::reap(mProcess.pid, status);
        if (not reaped) ::usleep(10000);
      }
      if (not reaped) {
        ::kill(-mProcess.pid, SIGKILL);
        ::waitpid(mProcess.pid, &status, 0);
        reaped = true;
      }
      read_stderr();
      std::string reason = "the MCP server '" + mConfig.server + "' " +
                           util::describe_exit(status);
      const std::string tail = stderr_tail();
      if (not tail.empty()) {
        const size_t keep = std::min<size_t>(tail.size(), 600);
        reason += "; its last output: " + tail.substr(tail.size() - keep);
      }
      {
        std::lock_guard<std::mutex> lock(mMutex);
        mDead = true;
        mExitReason = reason;
      }
      log("--- " + util::describe_exit(status) + " ---");
      fail_all(reason, true);
      break;
    }
  }

  if (not reaped and mProcess.pid > 0) {
    ::kill(-mProcess.pid, SIGKILL);
    ::waitpid(mProcess.pid, &status, 0);
  }
  close_fd(mProcess.stdin_fd);
  close_fd(mProcess.stdout_fd);
  close_fd(mProcess.stderr_fd);
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mDead = true;
    if (mExitReason.empty()) mExitReason = "the MCP server was shut down";
  }
  fail_all("the MCP server is not running", true);
  mInboxCv.notify_all();
}

void StdioTransport::worker_loop() {
  while (true) {
    RpcMessage request;
    {
      std::unique_lock<std::mutex> lock(mMutex);
      mInboxCv.wait(lock, [&] { return not mInbox.empty() or mDead or mClosing; });
      if (mInbox.empty()) return;
      request = std::move(mInbox.front());
      mInbox.pop_front();
      if (mInteractions++ == 0) mInteractionStarted = Clock::now();
    }

    util::JsonValue result;
    RpcError error;
    if (mHandlers.on_request) {
      mHandlers.on_request(request, result, error);
    } else {
      error.code = kMethodNotFound;
      error.message = "m8 does not support " + request.method;
    }

    {
      std::lock_guard<std::mutex> lock(mMutex);
      if (--mInteractions == 0) {
        // Give every waiting request back the time the human took.
        const Clock::duration paused = Clock::now() - mInteractionStarted;
        for (auto& [key, pending] : mPending) {
          if (pending->deadline != Clock::time_point::max()) {
            pending->deadline += paused;
          }
        }
      }
      if (not mDead and not mClosing) {
        mOutbox.push_back((error.code != 0
                               ? make_error(request.id, error.code, error.message)
                               : make_result(request.id, result)) +
                          "\n");
      }
    }
    wake();
  }
}

}  // namespace mcp
