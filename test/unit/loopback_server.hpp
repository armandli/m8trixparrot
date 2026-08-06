#ifndef LOOPBACK_SERVER_H
#define LOOPBACK_SERVER_H

#include <atomic>
#include <string>
#include <thread>

namespace agent::test {

// A one-response HTTP server on 127.0.0.1, for testing WebFetchTool without a
// network.
//
// A local file would be simpler, but curl reports no Content-Type for a
// file:// URL, and Content-Type is exactly what webfetch dispatches on
// (rendering_for(), src/core/tools_web.cpp) — so HTML rendering, the largest
// part of the tool, would be unreachable. Serving real HTTP is what makes the
// header observable.
//
// The port is assigned by the kernel (bind to port 0), so concurrent test runs
// can't collide. Each accepted connection gets the same canned response and is
// then closed; the request is drained but never parsed, so any path works.
struct LoopbackServer {
  // `content_type` empty means the response carries no Content-Type header at
  // all, which is how the "no header to go on" sniffing path is reached.
  LoopbackServer(int status, std::string content_type, std::string body);
  ~LoopbackServer();

  LoopbackServer(const LoopbackServer&) = delete;
  LoopbackServer& operator=(const LoopbackServer&) = delete;

  // e.g. url("/page.html") -> "http://127.0.0.1:54321/page.html"
  std::string url(const std::string& path = "/") const;

protected:
  void serve();

  int mListenFd = -1;
  int mPort = 0;
  std::atomic<bool> mStopping{false};
  std::thread mThread;
  std::string mResponse;
};

}  // namespace agent::test

#endif  // LOOPBACK_SERVER_H
