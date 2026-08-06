#include <loopback_server.h>

#include <cerrno>
#include <cstring>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <utility>

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

}  // namespace

LoopbackServer::LoopbackServer(int status, std::string content_type,
                               std::string body) {
  std::string response = "HTTP/1.1 " + std::to_string(status) + " " +
                         reason_for(status) + "\r\n";
  if (not content_type.empty()) {
    response += "Content-Type: " + content_type + "\r\n";
  }
  response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  response += "Connection: close\r\n\r\n";
  response += body;
  mResponse = std::move(response);

  mListenFd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (mListenFd < 0) return;

  const int reuse = 1;
  ::setsockopt(mListenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
  address.sin_port = 0;  // Any free port; the kernel picks.
  if (::bind(mListenFd, reinterpret_cast<sockaddr*>(&address),
             sizeof(address)) != 0) {
    ::close(mListenFd);
    mListenFd = -1;
    return;
  }
  if (::listen(mListenFd, 4) != 0) {
    ::close(mListenFd);
    mListenFd = -1;
    return;
  }

  sockaddr_in bound{};
  socklen_t bound_size = sizeof(bound);
  if (::getsockname(mListenFd, reinterpret_cast<sockaddr*>(&bound),
                    &bound_size) == 0) {
    mPort = ::ntohs(bound.sin_port);
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
}

std::string LoopbackServer::url(const std::string& path) const {
  return "http://127.0.0.1:" + std::to_string(mPort) + path;
}

void LoopbackServer::serve() {
  while (not mStopping) {
    const int fd = ::accept(mListenFd, nullptr, nullptr);
    if (fd < 0) return;  // Listening socket closed, or the run is over.

    // Drain the request. It is never parsed — every path gets the same canned
    // response — but curl won't read the reply until its own write completes.
    char buffer[4096];
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
    (void)got;

    size_t sent = 0;
    while (sent < mResponse.size()) {
      const ssize_t n = ::send(fd, mResponse.data() + sent,
                               mResponse.size() - sent, MSG_NOSIGNAL);
      if (n <= 0) break;
      sent += static_cast<size_t>(n);
    }
    ::close(fd);
  }
}

}  // namespace agent::test
