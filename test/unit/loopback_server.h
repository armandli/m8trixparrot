#ifndef LOOPBACK_SERVER_H
#define LOOPBACK_SERVER_H

#include <atomic>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

namespace agent::test {

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

  ~LoopbackServer();

  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  // e.g. url("/page.html") -> "http://127.0.0.1:54321/page.html"
  std::string url(const std::string& path = "/") const;

 protected:
  void listen_and_serve();
  void serve();

  int mListenFd = -1;
  int mPort = 0;
  std::atomic<bool> mStopping{false};
  std::thread mThread;
  std::vector<std::string> mResponses;  // Full HTTP response strings.
  std::size_t mIndex = 0;               // Touched only by the serve thread.
};

}  // namespace agent::test

#endif  // LOOPBACK_SERVER_H
