#include <core/mcp/sse.h>

namespace mcp {

SseParser::SseParser(size_t max_event_bytes)
    : mMaxEventBytes(max_event_bytes) {}

void SseParser::feed(std::string_view bytes, const Handler& on_event) {
  if (mStartOfStream and not bytes.empty()) {
    if (bytes.rfind("\xEF\xBB\xBF", 0) == 0) bytes.remove_prefix(3);
    mStartOfStream = false;
  }

  for (const char c : bytes) {
    // A CR already ended its line; an LF right after it is the second half of
    // a CRLF, possibly delivered in the next chunk, not a blank line.
    if (mSawCr) {
      mSawCr = false;
      if (c == '\n') continue;
    }
    if (c == '\r' or c == '\n') {
      if (c == '\r') mSawCr = true;
      line(mLine, on_event);
      mLine.clear();
      continue;
    }
    if (mLine.size() < mMaxEventBytes) {
      mLine += c;
    } else {
      // A single line past the limit: give up on this event entirely.
      mDropping = true;
      mOverflowed = true;
    }
  }
}

void SseParser::finish(const Handler& on_event) {
  if (not mLine.empty()) {
    line(mLine, on_event);
    mLine.clear();
  }
  dispatch(on_event);
}

void SseParser::line(std::string_view text, const Handler& on_event) {
  if (text.empty()) {
    dispatch(on_event);
    return;
  }
  if (text.front() == ':') return;  // comment / keep-alive

  std::string_view field = text;
  std::string_view value;
  if (const size_t colon = text.find(':'); colon != std::string_view::npos) {
    field = text.substr(0, colon);
    value = text.substr(colon + 1);
    if (not value.empty() and value.front() == ' ') value.remove_prefix(1);
  }

  if (field == "data") {
    if (mDropping) return;
    if (mEvent.data.size() + value.size() + 1 > mMaxEventBytes) {
      mDropping = true;
      mOverflowed = true;
      return;
    }
    if (mHasData) mEvent.data += '\n';
    mEvent.data.append(value);
    mHasData = true;
  } else if (field == "event") {
    mEvent.event = std::string(value);
  } else if (field == "id") {
    mEvent.id = std::string(value);
  }
  // `retry:` and unknown fields are ignored: 2026-07-28 streams do not resume.
}

void SseParser::dispatch(const Handler& on_event) {
  if (mHasData and not mDropping) {
    if (mEvent.event.empty()) mEvent.event = "message";
    on_event(mEvent);
  }
  mEvent = SseEvent{};
  mHasData = false;
  mDropping = false;
}

}  // namespace mcp
