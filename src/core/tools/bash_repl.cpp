#include <core/tools/bash_repl.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <core/util/uuid.h>

namespace tools {

namespace {

namespace sc = std::chrono;

// bash reports a parse error against the file it sourced, so its message would
// otherwise hand the model a temp path it can neither read nor act on. The
// tool's own name is what the model can actually reason about.
void relabel(std::string& text, const std::string& path) {
  if (path.empty()) return;
  for (size_t at = text.find(path); at != std::string::npos;
       at = text.find(path, at + 9)) {
    text.replace(at, path.size(), "bash_repl");
  }
}

// How long to give the shell to come back after a SIGINT before concluding it
// is wedged and replacing it. A command that ignores SIGINT is the case this
// bounds; an ordinary one dies immediately.
constexpr int64_t kInterruptGraceMs = 2000;

// The teardown ladder, matching ShellSession's: SIGHUP, poll, SIGKILL.
constexpr int kStopPolls = 20;
constexpr int kStopPollMs = 10;

int64_t now_ms() {
  return sc::duration_cast<sc::milliseconds>(
             sc::steady_clock::now().time_since_epoch())
      .count();
}

// Writes every byte or gives up. Protocol lines are short, so a partial write
// is only ever an EINTR away rather than a full pipe.
bool write_all(int fd, const std::string& text) {
  size_t written = 0;
  while (written < text.size()) {
    const ssize_t n =
        ::write(fd, text.data() + written, text.size() - written);
    if (n > 0) {
      written += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 and (errno == EINTR or errno == EAGAIN)) continue;
    return false;
  }
  return true;
}

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

}  // namespace

BashReplSession::BashReplSession() {
  mToken = util::generate_uuid_v4();
  mMarker = "\n__M8_END_" + mToken + "__";

  std::error_code ec;
  const std::filesystem::path dir = std::filesystem::temp_directory_path(ec);
  mCommandPath =
      (ec ? std::filesystem::path("/tmp") : dir) / ("m8-bash-repl-" + mToken);
}

BashReplSession::~BashReplSession() {
  stop();
  std::error_code ec;
  if (not mCommandPath.empty()) std::filesystem::remove(mCommandPath, ec);
}

bool BashReplSession::start(std::string& error) {
  int to_child[2] = {-1, -1};
  int from_child[2] = {-1, -1};

  if (::pipe(to_child) != 0) {
    error = "could not create the shell's input pipe";
    return false;
  }
  if (::pipe(from_child) != 0) {
    ::close(to_child[0]);
    ::close(to_child[1]);
    error = "could not create the shell's output pipe";
    return false;
  }

  const pid_t pid = ::fork();
  if (pid < 0) {
    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);
    error = "could not fork the shell";
    return false;
  }

  if (pid == 0) {
    // Child. Its own process group, so a timeout can signal the command
    // without touching anything else this process is running.
    ::setpgid(0, 0);

    ::dup2(to_child[0], STDIN_FILENO);
    ::dup2(from_child[1], STDOUT_FILENO);
    ::dup2(from_child[1], STDERR_FILENO);

    ::close(to_child[0]);
    ::close(to_child[1]);
    ::close(from_child[0]);
    ::close(from_child[1]);

    // --norc/--noprofile: the shell should behave the same on every machine.
    // -s: read commands from stdin. The environment comes through fork.
    ::execlp("bash", "bash", "--norc", "--noprofile", "-s",
             static_cast<char*>(nullptr));
    ::_exit(127);
  }

  // Parent.
  ::close(to_child[0]);
  ::close(from_child[1]);
  mChild = pid;
  mStdin = to_child[1];
  mStdout = from_child[0];
  mPending.clear();

  // Racing the child's own setpgid; doing it on both sides means neither
  // ordering loses.
  ::setpgid(pid, pid);

  const int flags = ::fcntl(mStdout, F_GETFL, 0);
  if (flags >= 0) ::fcntl(mStdout, F_SETFL, flags | O_NONBLOCK);

  // `trap ':' INT` is what lets the timeout path work: without a trap, a
  // non-interactive bash dies on SIGINT and we would lose the session every
  // time a command ran long. `trap '' INT` would be worse still — children
  // inherit an ignored signal, so the command itself would become
  // uninterruptible.
  //
  // The sync marker that follows leaves the stream at a known point, so a
  // command's output can never be preceded by the shell's own startup noise.
  const std::string prime =
      "trap ':' INT\n"
      "printf '\\n__M8_END_" + mToken + "__%d__%s\\n' 0 \"$PWD\"\n";
  if (not write_all(mStdin, prime)) {
    error = "could not write to the shell";
    stop();
    return false;
  }

  int code = 0;
  std::string cwd;
  bool died = false;
  if (not read_until_marker(5000, code, cwd, died)) {
    error = died ? "the shell exited before it was ready"
                 : "the shell did not respond when starting";
    stop();
    return false;
  }
  mPending.clear();  // Discard anything the shell said while starting.
  return true;
}

void BashReplSession::stop() {
  if (mChild <= 0) {
    close_fd(mStdin);
    close_fd(mStdout);
    return;
  }

  // Closing stdin is the polite way to end a shell reading from a pipe.
  close_fd(mStdin);
  ::kill(-mChild, SIGHUP);

  for (int i = 0; i < kStopPolls; ++i) {
    int status = 0;
    const pid_t reaped = ::waitpid(mChild, &status, WNOHANG);
    if (reaped == mChild or (reaped < 0 and errno == ECHILD)) {
      mChild = -1;
      close_fd(mStdout);
      return;
    }
    ::usleep(kStopPollMs * 1000);
  }

  ::kill(-mChild, SIGKILL);
  int status = 0;
  ::waitpid(mChild, &status, 0);
  mChild = -1;
  close_fd(mStdout);
}

void BashReplSession::restart() {
  stop();
  mPending.clear();
}

bool BashReplSession::read_until_marker(int64_t deadline_ms, int& exit_code,
                                        std::string& cwd, bool& shell_died) {
  shell_died = false;
  const int64_t started = now_ms();

  while (true) {
    // The marker may straddle two reads, so rescan a tail rather than only the
    // bytes that just arrived.
    const size_t pos = mPending.find(mMarker);
    if (pos != std::string::npos) {
      const size_t fields = pos + mMarker.size();
      const size_t line_end = mPending.find('\n', fields);
      if (line_end != std::string::npos) {
        const std::string tail =
            mPending.substr(fields, line_end - fields);
        const size_t split = tail.find("__");
        exit_code = std::atoi(tail.substr(0, split).c_str());
        cwd = split == std::string::npos ? std::string()
                                         : tail.substr(split + 2);
        mPending.erase(pos, line_end + 1 - pos);
        return true;
      }
    }

    int wait_ms = -1;
    if (deadline_ms >= 0) {
      const int64_t left = deadline_ms - (now_ms() - started);
      if (left <= 0) return false;
      wait_ms = static_cast<int>(left);
    }

    struct pollfd pfd{};
    pfd.fd = mStdout;
    pfd.events = POLLIN;
    const int ready = ::poll(&pfd, 1, wait_ms);
    if (ready < 0) {
      if (errno == EINTR) continue;
      shell_died = true;
      return false;
    }
    if (ready == 0) return false;  // Deadline.

    char buffer[4096];
    const ssize_t n = ::read(mStdout, buffer, sizeof(buffer));
    if (n > 0) {
      mPending.append(buffer, static_cast<size_t>(n));
      continue;
    }
    if (n == 0) {  // EOF: the shell is gone.
      shell_died = true;
      return false;
    }
    if (errno == EINTR or errno == EAGAIN) continue;
    shell_died = true;
    return false;
  }
}

BashReplSession::Outcome BashReplSession::run(const std::string& command,
                                              int64_t timeout_seconds) {
  Outcome outcome;

  if (mChild <= 0) {
    std::string error;
    if (not start(error)) {
      outcome.error = error;
      return outcome;
    }
  }

  // The command goes to a file the shell sources, rather than down the pipe as
  // text. An unterminated quote or a dangling heredoc then becomes a parse
  // error inside that file, where `source` reports it and returns non-zero —
  // whereas piping the same text would make the shell treat the marker line
  // that follows as a continuation of the command and hang the session for
  // good. It also means no escaping, and a protocol line whose length does not
  // depend on the command's.
  {
    std::ofstream out(mCommandPath, std::ios::binary | std::ios::trunc);
    if (not out) {
      outcome.error = "bash_repl: could not stage the command";
      return outcome;
    }
    out << command << "\n";
    if (not out) {
      outcome.error = "bash_repl: could not stage the command";
      return outcome;
    }
  }

  // $? is source's status: printf's arguments are expanded before it runs.
  const std::string line = "source " + mCommandPath +
                           "\nprintf '\\n__M8_END_" + mToken +
                           "__%d__%s\\n' \"$?\" \"$PWD\"\n";
  if (not write_all(mStdin, line)) {
    stop();
    outcome.error = "bash_repl: the shell stopped accepting commands";
    return outcome;
  }

  const int64_t deadline =
      timeout_seconds > 0 ? timeout_seconds * 1000 : -1;

  bool died = false;
  if (read_until_marker(deadline, outcome.exit_code, outcome.cwd, died)) {
    outcome.output = std::move(mPending);
    mPending.clear();
    relabel(outcome.output, mCommandPath);
    return outcome;
  }

  if (died) {
    // The command ran `exit`, or the shell crashed. Whatever it printed first
    // is still worth handing back.
    outcome.output = std::move(mPending);
    mPending.clear();
    relabel(outcome.output, mCommandPath);
    stop();
    outcome.restarted = true;
    return outcome;
  }

  // Timed out. SIGINT the process group: the foreground command dies, and the
  // shell survives on the trap installed at startup, so everything the session
  // has built up is still there.
  outcome.timed_out = true;
  ::kill(-mChild, SIGINT);

  if (read_until_marker(kInterruptGraceMs, outcome.exit_code, outcome.cwd,
                        died)) {
    outcome.output = std::move(mPending);
    mPending.clear();
    relabel(outcome.output, mCommandPath);
    return outcome;
  }

  // It would not take the interrupt. Replacing the shell is the only way out,
  // and it costs the session's state — which the caller reports rather than
  // hiding.
  outcome.output = std::move(mPending);
  mPending.clear();
  relabel(outcome.output, mCommandPath);
  stop();
  outcome.restarted = true;
  return outcome;
}

}  // namespace tools
