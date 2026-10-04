#ifndef M8_TEST_HTTP_TEST_SERVER_H
#define M8_TEST_HTTP_TEST_SERVER_H

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace m8test {

struct HttpRequest {
  std::string method;
  std::string path;   // without the query
  std::string query;  // after '?', undecoded
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;

  // First header with this name, compared without case; "" when absent.
  std::string header(std::string_view name) const;
};

struct HttpReply {
  int status = 200;
  std::vector<std::pair<std::string, std::string>> headers;
  std::string body;
  // When set, the body is written in these pieces with `delay` between them
  // and the connection closed after (a streamed text/event-stream response).
  std::vector<std::string> chunks;
  std::chrono::milliseconds delay{0};
  // Or written piece by piece as this returns them, until it returns nullopt;
  // it may block, to stream something only once it has happened.
  std::function<std::optional<std::string>()> stream;
};

// A real HTTP/1.1 server on 127.0.0.1 for tests, unlike LoopbackServer:
// requests are parsed (method, path, headers, Content-Length body) and
// recorded, each is answered by a handler, replies can carry any headers and
// be streamed, and every connection gets its own thread so concurrent and
// long-lived requests behave as they would against a real server.
struct HttpTestServer {
  using Handler = std::function<HttpReply(const HttpRequest&)>;

  explicit HttpTestServer(Handler handler);
  ~HttpTestServer();

  HttpTestServer(const HttpTestServer&) = delete;
  HttpTestServer& operator=(const HttpTestServer&) = delete;

  std::string url(const std::string& path = "") const;
  int port() const { return mPort; }

  std::vector<HttpRequest> requests() const;
  size_t request_count() const;
  // Streamed replies the client stopped reading before they ended.
  int aborted_streams() const { return mAborted.load(); }

private:
  void accept_loop();
  void serve(int fd);

  Handler mHandler;
  int mListen = -1;
  int mPort = 0;
  std::atomic<bool> mStopping{false};
  std::atomic<int> mAborted{0};
  std::thread mAcceptor;

  mutable std::mutex mMutex;
  std::vector<HttpRequest> mRequests;
  std::vector<std::thread> mConnections;
  std::vector<int> mOpen;
};

}  // namespace m8test

#endif  // M8_TEST_HTTP_TEST_SERVER_H
