#include <core/shell_session.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <algorithm>
#include <string>

#if defined(__APPLE__)
#include <libproc.h>
#include <util.h>
#elif defined(__linux__)
#include <climits>
#include <pty.h>
#endif

namespace agent {

namespace {

std::string basename_of(const std::string& path) {
  const size_t slash = path.find_last_of('/');
  return slash == std::string::npos ? path : path.substr(slash + 1);
}

winsize make_winsize(int cols, int rows) {
  winsize ws{};
  ws.ws_col = static_cast<unsigned short>(cols > 0 ? cols : 80);
  ws.ws_row = static_cast<unsigned short>(rows > 0 ? rows : 24);
  return ws;
}

}  // namespace

std::string resolve_shell(const std::string& shell_override) {
  if (not shell_override.empty()) return shell_override;
  if (const char* env = std::getenv("SHELL"); env != nullptr and *env != '\0') {
    return env;
  }
  for (const char* candidate : {"/bin/zsh", "/bin/bash", "/bin/sh"}) {
    if (::access(candidate, X_OK) == 0) return candidate;
  }
  return "/bin/sh";
}

ShellSession::ShellSession() = default;

ShellSession::~ShellSession() {
  mStop.store(true);
  if (mReader.joinable()) mReader.join();
  stop_child();
  if (mMaster >= 0) {
    ::close(mMaster);
    mMaster = -1;
  }
}

bool ShellSession::start(
    int cols, int rows, const std::string& shell_override, std::string* error,
    const std::vector<std::pair<std::string, std::string>>& extra_env) {
  const std::string shell = resolve_shell(shell_override);
  winsize ws = make_winsize(cols, rows);

  int master = -1;
  const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
  if (pid < 0) {
    if (error != nullptr) {
      *error = std::string("forkpty failed: ") + std::strerror(errno);
    }
    return false;
  }

  if (pid == 0) {
    // forkpty already ran setsid(), made the slave our controlling terminal,
    // and dup'd it onto 0/1/2. Interactive programs decide they have a tty; a
    // login shell (argv[0] starting with '-') runs the full rc chain.
    ::setenv("TERM", "xterm-256color", 1);
    ::setenv("COLORTERM", "truecolor", 1);
    ::unsetenv("LINES");
    ::unsetenv("COLUMNS");
    for (const auto& [name, value] : extra_env) {
      ::setenv(name.c_str(), value.c_str(), 1);
    }
    // "-" prefix => login shell (full rc chain); -i forces interactive mode so
    // job control and the interactive rc files are on even if the child's tty
    // detection is fooled.
    const std::string argv0 = "-" + basename_of(shell);
    ::execlp(shell.c_str(), argv0.c_str(), "-i", static_cast<char*>(nullptr));
    ::_exit(127);
  }

  mChild = pid;
  mMaster = master;
  // Non-blocking: the reader's poll timeout, not a stuck read(), is what bounds
  // how long the destructor waits to join.
  const int flags = ::fcntl(mMaster, F_GETFL, 0);
  if (flags >= 0) ::fcntl(mMaster, F_SETFL, flags | O_NONBLOCK);

  mReader = std::thread([this] { reader_loop(); });
  return true;
}

void ShellSession::write_bytes(std::string_view bytes) {
  if (mMaster < 0) return;
  size_t offset = 0;
  while (offset < bytes.size()) {
    const size_t chunk = std::min<size_t>(bytes.size() - offset, 4096);
    const ssize_t n = ::write(mMaster, bytes.data() + offset, chunk);
    if (n > 0) {
      offset += static_cast<size_t>(n);
      continue;
    }
    if (n < 0 and errno == EINTR) continue;
    if (n < 0 and (errno == EAGAIN or errno == EWOULDBLOCK)) {
      pollfd pfd{mMaster, POLLOUT, 0};
      if (::poll(&pfd, 1, 1000) <= 0) return;  // drop rather than spin forever
      continue;
    }
    return;  // hard error, e.g. EIO once the shell is gone
  }
}

void ShellSession::resize(int cols, int rows) {
  if (mMaster < 0) return;
  winsize ws = make_winsize(cols, rows);
  ::ioctl(mMaster, TIOCSWINSZ, &ws);
}

std::string ShellSession::cwd() const {
  if (mChild <= 0) return std::string();
#if defined(__APPLE__)
  proc_vnodepathinfo vpi{};
  const int n =
      ::proc_pidinfo(mChild, PROC_PIDVNODEPATHINFO, 0, &vpi, sizeof(vpi));
  if (n == static_cast<int>(sizeof(vpi))) {
    return std::string(vpi.pvi_cdir.vip_path);
  }
  return std::string();
#elif defined(__linux__)
  char buf[PATH_MAX];
  const std::string link = "/proc/" + std::to_string(mChild) + "/cwd";
  const ssize_t n = ::readlink(link.c_str(), buf, sizeof(buf) - 1);
  if (n > 0) {
    buf[n] = '\0';
    return std::string(buf);
  }
  return std::string();
#else
  return std::string();
#endif
}

void ShellSession::reader_loop() {
  std::string buffer;
  buffer.resize(65536);

  while (not mStop.load()) {
    pollfd pfd{mMaster, POLLIN, 0};
    const int pr = ::poll(&pfd, 1, 100);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;

    if (pfd.revents & POLLIN) {
      const ssize_t n = ::read(mMaster, buffer.data(), buffer.size());
      if (n > 0) {
        if (on_bytes) on_bytes(std::string_view(buffer.data(), n));
        continue;
      }
      if (n < 0 and (errno == EAGAIN or errno == EWOULDBLOCK or errno == EINTR)) {
        continue;
      }
      break;  // n == 0 (EOF) or a hard error
    }
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
  }

  // A natural end (EOF/HUP, not a shutdown request): reap here and notify.
  // On the shutdown path stop_child() does the reaping instead.
  if (not mStop.load()) {
    int status = 0;
    if (::waitpid(mChild, &status, 0) == mChild) {
      mExitStatus.store(status);
      if (on_exit) on_exit(status);
    }
  }
}

void ShellSession::stop_child() {
  if (mChild <= 0) return;
  if (mExitStatus.load() >= 0) return;  // the reader already reaped it

  ::kill(mChild, SIGHUP);
  for (int i = 0; i < 20; ++i) {
    int status = 0;
    const pid_t r = ::waitpid(mChild, &status, WNOHANG);
    if (r == mChild) {
      mExitStatus.store(status);
      return;
    }
    if (r < 0) return;  // ECHILD: already gone
    const timespec ts{0, 10'000'000};  // 10 ms
    ::nanosleep(&ts, nullptr);
  }
  ::kill(mChild, SIGKILL);
  int status = 0;
  if (::waitpid(mChild, &status, 0) == mChild) mExitStatus.store(status);
}

}  // namespace agent
