#ifndef LOOPBACK_SERVER_H
#define LOOPBACK_SERVER_H

#include <atomic>
#include <chrono>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace m8test {

// For the cases that are about how a client schedules requests rather than what
// it does with one. `delay` holds each connection open before replying, so
// requests actually overlap in time, and `concurrent` serves each on its own
// thread — without it the accept loop handles one at a time and every client,
// however parallel, looks perfectly serial from the outside.
struct LoopbackOptions {
  std::vector<std::string> json_bodies;
  std::chrono::milliseconds delay{0};
  bool concurrent = false;
};

// A tiny HTTP server on 127.0.0.1 for testing HTTP clients without a network.
//
// A local file would be simpler, but curl reports no Content-Type for a
// file:// URL, and Content-Type is exactly what webfetch dispatches on
// (rendering_for(), src/core/tools_web.cpp) — so HTML rendering, the largest
// part of the tool, would be unreachable. Serving real HTTP is what makes the
// header observable.
//
// The port is assigned by the kernel (bind to port 0), so concurrent test runs
// can't collide. The request is drained but never parsed, so any path works.
struct LoopbackServer {
  // One canned response for every connection.
  LoopbackServer(int status, std::string content_type, std::string body);

  // A sequence of 200 / application/json responses: connection i gets body i,
  // and every connection past the end repeats the last body. For driving a
  // fake Ollama through a multi-call agent loop.
  explicit LoopbackServer(std::vector<std::string> json_bodies);

  explicit LoopbackServer(LoopbackOptions options);

  ~LoopbackServer();

  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  // e.g. url("/page.html") -> "http://127.0.0.1:54321/page.html"
  std::string url(const std::string& path = "/") const;

  // The most requests that were ever being served at the same moment. The
  // point of the concurrent mode: it is what a cap on in-flight requests is
  // actually measured against.
  std::size_t max_concurrent() const { return mMaxConcurrent.load(); }

protected:
  void listen_and_serve();
  void serve();
  void handle(int fd, std::size_t response_index);

  int mListenFd = -1;
  int mPort = 0;
  std::atomic<bool> mStopping{false};
  std::thread mThread;
  std::vector<std::string> mResponses;  // Full HTTP response strings.
  std::atomic<std::size_t> mIndex{0};

  std::chrono::milliseconds mDelay{0};
  bool mConcurrent = false;
  std::atomic<std::size_t> mInFlight{0};
  std::atomic<std::size_t> mMaxConcurrent{0};
  std::mutex mHandlersMutex;
  std::vector<std::thread> mHandlers;
};

}  // namespace m8test

#endif  // LOOPBACK_SERVER_H
