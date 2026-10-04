#include <http_test_server.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace m8test {

namespace {

std::string lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool send_all(int fd, std::string_view data) {
  // A client that hung up must not kill the test with SIGPIPE (Linux has no
  // SO_NOSIGPIPE).
#if defined(MSG_NOSIGNAL)
  constexpr int kFlags = MSG_NOSIGNAL;
#else
  constexpr int kFlags = 0;
#endif
  while (not data.empty()) {
    const ssize_t n = ::send(fd, data.data(), data.size(), kFlags);
    if (n <= 0) return false;
    data.remove_prefix(static_cast<size_t>(n));
  }
  return true;
}

const char* reason(int status) {
  switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 302: return "Found";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default: return "Status";
  }
}

}  // namespace

std::string HttpRequest::header(std::string_view name) const {
  const std::string wanted = lower(name);
  for (const auto& [key, value] : headers) {
    if (lower(key) == wanted) return value;
  }
  return std::string();
}

HttpTestServer::HttpTestServer(Handler handler) : mHandler(std::move(handler)) {
  mListen = ::socket(AF_INET, SOCK_STREAM, 0);
  const int yes = 1;
  ::setsockopt(mListen, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#if defined(SO_NOSIGPIPE)
  ::setsockopt(mListen, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  ::bind(mListen, reinterpret_cast<sockaddr*>(&address), sizeof(address));
  socklen_t length = sizeof(address);
  ::getsockname(mListen, reinterpret_cast<sockaddr*>(&address), &length);
  mPort = ntohs(address.sin_port);
  ::listen(mListen, 64);
  mAcceptor = std::thread([this] { accept_loop(); });
}

HttpTestServer::~HttpTestServer() {
  mStopping.store(true);
  ::shutdown(mListen, SHUT_RDWR);
  ::close(mListen);
  if (mAcceptor.joinable()) mAcceptor.join();
  std::vector<std::thread> connections;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    for (const int fd : mOpen) ::shutdown(fd, SHUT_RDWR);
    connections.swap(mConnections);
  }
  for (std::thread& thread : connections) {
    if (thread.joinable()) thread.join();
  }
}

std::string HttpTestServer::url(const std::string& path) const {
  return "http://127.0.0.1:" + std::to_string(mPort) + path;
}

std::vector<HttpRequest> HttpTestServer::requests() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mRequests;
}

size_t HttpTestServer::request_count() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mRequests.size();
}

void HttpTestServer::accept_loop() {
  while (not mStopping.load()) {
    pollfd listening{mListen, POLLIN, 0};
    if (::poll(&listening, 1, 100) <= 0) continue;
    const int fd = ::accept(mListen, nullptr, nullptr);
    if (fd < 0) continue;
#if defined(SO_NOSIGPIPE)
    const int yes = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes));
#endif
    std::lock_guard<std::mutex> lock(mMutex);
    mOpen.push_back(fd);
    mConnections.emplace_back([this, fd] { serve(fd); });
  }
}

void HttpTestServer::serve(int fd) {
  // HTTP/1.1 keep-alive: serve requests on this connection until the client
  // closes it or a reply is streamed (streams end with the connection).
  std::string buffer;
  while (not mStopping.load()) {
    size_t header_end;
    while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
      char chunk[8192];
      const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
      if (n <= 0) goto done;
      buffer.append(chunk, static_cast<size_t>(n));
    }

    {
      HttpRequest request;
      const std::string head = buffer.substr(0, header_end);
      buffer.erase(0, header_end + 4);

      size_t line_end = head.find("\r\n");
      const std::string request_line = head.substr(0, line_end);
      const size_t first_space = request_line.find(' ');
      const size_t second_space = request_line.find(' ', first_space + 1);
      request.method = request_line.substr(0, first_space);
      std::string target = request_line.substr(first_space + 1, second_space - first_space - 1);
      if (const size_t question = target.find('?'); question != std::string::npos) {
        request.query = target.substr(question + 1);
        target.resize(question);
      }
      request.path = target;

      size_t at = line_end == std::string::npos ? head.size() : line_end + 2;
      while (at < head.size()) {
        size_t end = head.find("\r\n", at);
        if (end == std::string::npos) end = head.size();
        const std::string line = head.substr(at, end - at);
        if (const size_t colon = line.find(':'); colon != std::string::npos) {
          std::string value = line.substr(colon + 1);
          value.erase(0, value.find_first_not_of(' '));
          request.headers.emplace_back(line.substr(0, colon), value);
        }
        at = end + 2;
      }

      const size_t length = static_cast<size_t>(std::atol(request.header("Content-Length").c_str()));
      while (buffer.size() < length) {
        char chunk[8192];
        const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
        if (n <= 0) goto done;
        buffer.append(chunk, static_cast<size_t>(n));
      }
      request.body = buffer.substr(0, length);
      buffer.erase(0, length);

      {
        std::lock_guard<std::mutex> lock(mMutex);
        mRequests.push_back(request);
      }
      const HttpReply reply = mHandler(request);

      std::string out = "HTTP/1.1 " + std::to_string(reply.status) + " " +
                        reason(reply.status) + "\r\n";
      bool has_type = false;
      for (const auto& [key, value] : reply.headers) {
        out += key + ": " + value + "\r\n";
        has_type = has_type or lower(key) == "content-type";
      }
      if (not has_type and not reply.body.empty()) {
        out += "Content-Type: application/json\r\n";
      }
      if (reply.chunks.empty() and not reply.stream) {
        out += "Content-Length: " + std::to_string(reply.body.size()) + "\r\n\r\n";
        out += reply.body;
        if (not send_all(fd, out)) goto done;
        continue;
      }
      out += "Connection: close\r\n\r\n";
      if (not send_all(fd, out)) goto done;
      for (const std::string& piece : reply.chunks) {
        if (reply.delay.count() > 0) std::this_thread::sleep_for(reply.delay);
        if (mStopping.load()) goto done;
        if (not send_all(fd, piece)) {
          ++mAborted;
          goto done;
        }
      }
      if (reply.stream) {
        while (std::optional<std::string> piece = reply.stream()) {
          if (mStopping.load()) goto done;
          if (not send_all(fd, *piece)) {
            ++mAborted;
            goto done;
          }
        }
      }
      goto done;
    }
  }
done:
  {
    std::lock_guard<std::mutex> lock(mMutex);
    mOpen.erase(std::remove(mOpen.begin(), mOpen.end(), fd), mOpen.end());
  }
  ::close(fd);
}

}  // namespace m8test
