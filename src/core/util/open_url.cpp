#include <core/util/open_url.h>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <optional>

#include <core/util/process.h>
#include <core/util/url.h>

namespace util {

namespace {

#if defined(__APPLE__)
constexpr const char* kOpener = "open";
#else
constexpr const char* kOpener = "xdg-open";
#endif

// How long to wait for the opener to say whether it worked; `open` answers in
// well under a second, and one that is still running has done its part.
constexpr auto kOpenerWait = std::chrono::seconds(5);

void close_fd(int& fd) {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

}  // namespace

std::string open_url_problem(const std::string& url) {
  // Control characters have no business in a link a person is asked to open.
  for (const char c : url) {
    if (static_cast<unsigned char>(c) < 0x20 or c == 0x7F) {
      return "the link contains control characters";
    }
  }
  std::string why;
  const std::optional<Url> parsed = parse_url(url, &why);
  if (not parsed) return "not a usable link: " + why;
  if (parsed->scheme != "http" and parsed->scheme != "https") {
    return "m8 opens only http and https links, not " + parsed->scheme;
  }
  return std::string();
}

bool open_url(const std::string& url, std::string& error) {
  if (std::string problem = open_url_problem(url); not problem.empty()) {
    error = std::move(problem);
    return false;
  }

  SpawnOptions options;
  options.command = kOpener;
  options.args = {url};
  options.env = child_environment(/*inherit_all=*/true, {});
  SpawnedProcess process;
  if (not spawn_process(options, process, error)) return false;
  close_fd(process.stdin_fd);

  // Read what it says (its complaint, if it fails) until it exits.
  std::string complaint;
  int status = 0;
  bool exited = false;
  const auto deadline = std::chrono::steady_clock::now() + kOpenerWait;
  while (std::chrono::steady_clock::now() < deadline) {
    pollfd fds[2] = {{process.stdout_fd, POLLIN, 0}, {process.stderr_fd, POLLIN, 0}};
    ::poll(fds, 2, 100);
    char buffer[1024];
    for (int* fd : {&process.stdout_fd, &process.stderr_fd}) {
      while (*fd >= 0) {
        const ssize_t n = ::read(*fd, buffer, sizeof(buffer));
        if (n <= 0) break;
        if (fd == &process.stderr_fd and complaint.size() < 2048) {
          complaint.append(buffer, static_cast<size_t>(n));
        }
      }
    }
    if (reap(process.pid, status)) {
      exited = true;
      break;
    }
  }
  close_fd(process.stdout_fd);
  close_fd(process.stderr_fd);

  if (not exited) return true;  // still at it: it has the URL
  if (WIFEXITED(status) and WEXITSTATUS(status) == 0) return true;
  while (not complaint.empty() and (complaint.back() == '\n' or complaint.back() == ' ')) {
    complaint.pop_back();
  }
  error = std::string(kOpener) + " " + describe_exit(status) +
          (complaint.empty() ? std::string() : ": " + complaint);
  return false;
}

}  // namespace util
