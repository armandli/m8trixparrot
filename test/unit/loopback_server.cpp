#include <loopback_server.h>

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <string>

namespace agent::test {

namespace {

// The reason phrase matters to nobody here, but curl wants a well-formed
// status line, so give it one.
const char* reason_for(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 404: return "Not Found";
    case 500: return "Internal Server Error";
    default: return "Unknown";
  }
}

std::string http_response(int status, const std::string& content_type,
                          const std::string& body) {
  std::string response = "HTTP/1.1 " + std::to_string(status) + " " +
                         reason_for(status) + "\r\n";
  if (not content_type.empty()) {
    response += "Content-Type: " + content_type + "\r\n";
  }
  response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  response += "Connection: close\r\n\r\n";
  response += body;
  return response;
}

}  // namespace

LoopbackServer::LoopbackServer(int status, std::string content_type,
                               std::string body) {
  mResponses.push_back(http_response(status, content_type, body));
  listen_and_serve();
}

LoopbackServer::LoopbackServer(std::vector<std::string> json_bodies)
    : LoopbackServer(LoopbackOptions{std::move(json_bodies), {}, false}) {}

LoopbackServer::LoopbackServer(LoopbackOptions options)
    : mDelay(options.delay), mConcurrent(options.concurrent) {
  for (const std::string& body : options.json_bodies) {
    mResponses.push_back(http_response(200, "application/json", body));
  }
  if (mResponses.empty()) {
    mResponses.push_back(http_response(200, "application/json", "{}"));
  }
  listen_and_serve();
}

void LoopbackServer::listen_and_serve() {
  mListenFd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (mListenFd < 0) return;

  const int reuse = 1;
  ::setsockopt(mListenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;  // Any free port; the kernel picks.
  if (::bind(mListenFd, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
    ::close(mListenFd);
    mListenFd = -1;
    return;
  }
  if (::listen(mListenFd, 8) != 0) {
    ::close(mListenFd);
    mListenFd = -1;
    return;
  }

  sockaddr_in bound{};
  socklen_t bound_size = sizeof(bound);
  if (::getsockname(mListenFd, reinterpret_cast<sockaddr*>(&bound),
                    &bound_size) == 0) {
    mPort = ntohs(bound.sin_port);
  }

  mThread = std::thread([this] { serve(); });
}

LoopbackServer::~LoopbackServer() {
  mStopping = true;
  // Shutting the listening socket down is what wakes the accept() in the
  // serve thread; closing alone can leave it blocked.
  if (mListenFd >= 0) {
    ::shutdown(mListenFd, SHUT_RDWR);
    ::close(mListenFd);
    mListenFd = -1;
  }
  if (mThread.joinable()) mThread.join();

  // After the accept loop is done, nothing else can be appended.
  for (std::thread& handler : mHandlers) {
    if (handler.joinable()) handler.join();
  }
}

std::string LoopbackServer::url(const std::string& path) const {
  return "http://127.0.0.1:" + std::to_string(mPort) + path;
}

void LoopbackServer::serve() {
  while (not mStopping) {
    const int fd = ::accept(mListenFd, nullptr, nullptr);
    if (fd < 0) return;  // Listening socket closed, or the run is over.

    // Claimed here rather than in the handler so connections keep getting the
    // canned bodies in the order they arrived, even when they are served in
    // parallel.
    const std::size_t index = mIndex.fetch_add(1);
    if (not mConcurrent) {
      handle(fd, index);
      continue;
    }
    std::lock_guard<std::mutex> lock(mHandlersMutex);
    mHandlers.emplace_back([this, fd, index] { handle(fd, index); });
  }
}

void LoopbackServer::handle(int fd, std::size_t response_index) {
  const std::size_t in_flight = mInFlight.fetch_add(1) + 1;
  std::size_t seen = mMaxConcurrent.load();
  while (in_flight > seen and
         not mMaxConcurrent.compare_exchange_weak(seen, in_flight)) {
    // compare_exchange_weak refreshed `seen`; re-test against it.
  }

  // Drain the request. It is never parsed — the path is ignored — but curl
  // won't read the reply until its own write completes.
  char buffer[4096];
  const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
  (void)got;

  // Held open, not slept before accepting, so the overlap is real.
  if (mDelay.count() > 0) std::this_thread::sleep_for(mDelay);

  const std::string& response =
      mResponses[std::min(response_index, mResponses.size() - 1)];

  size_t sent = 0;
  while (sent < response.size()) {
    const ssize_t n = ::send(fd, response.data() + sent, response.size() - sent,
                             MSG_NOSIGNAL);
    if (n <= 0) break;
    sent += static_cast<size_t>(n);
  }
  ::close(fd);
  mInFlight.fetch_sub(1);
}

}  // namespace agent::test
