#ifndef M8_MCP_SSE_H
#define M8_MCP_SSE_H

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>

namespace mcp {

// One Server-Sent Event: an `event:` name (default "message"), its joined
// `data:` lines, and the `id:` if any.
struct SseEvent {
  std::string event = "message";
  std::string data;
  std::string id;
};

// Incremental parser for a text/event-stream body, fed in whatever chunks the
// network delivers (a chunk may end mid-line, even mid-CRLF). Follows the
// WHATWG event-stream rules: CR, LF and CRLF all end a line; a line starting
// with ':' is a comment (servers send them as keep-alives); multiple `data:`
// lines join with '\n'; a blank line dispatches; an event with no data is not
// dispatched.
struct SseParser {
  using Handler = std::function<void(const SseEvent&)>;

  // An event whose data would exceed this is dropped (and `overflowed` set)
  // rather than buffered without bound.
  explicit SseParser(size_t max_event_bytes = 32u << 20);

  void feed(std::string_view bytes, const Handler& on_event);
  // End of stream: a final event not followed by a blank line is dispatched.
  void finish(const Handler& on_event);

  bool overflowed() const { return mOverflowed; }

  // What a reconnecting client sends as Last-Event-ID: the most recent `id:`,
  // kept across events — and recorded even from an event with no data, which
  // is how a 2025-11-25 server primes the client for a reconnect.
  const std::string& last_event_id() const { return mLastEventId; }
  // The server's `retry:` (ms) — how long to wait before reconnecting; -1
  // until it sends one.
  long retry_ms() const { return mRetryMs; }

private:
  void line(std::string_view text, const Handler& on_event);
  void dispatch(const Handler& on_event);

  size_t mMaxEventBytes;
  std::string mLine;
  SseEvent mEvent;
  bool mHasData = false;
  bool mDropping = false;
  bool mOverflowed = false;
  bool mSawCr = false;       // the previous chunk ended with CR
  bool mStartOfStream = true;  // for the optional UTF-8 BOM
  std::string mIdBuffer;     // WHATWG's "last event ID buffer"
  std::string mLastEventId;
  long mRetryMs = -1;
};

}  // namespace mcp

#endif  // M8_MCP_SSE_H
