// The text/event-stream parser behind Streamable HTTP responses. The network
// hands it arbitrary chunks, so each behaviour is also checked fed one byte at
// a time.

#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/mcp/sse.h>

namespace mcp {
namespace {

std::vector<SseEvent> parse_all(const std::string& stream, bool bytewise) {
  std::vector<SseEvent> events;
  SseParser parser;
  const auto on_event = [&](const SseEvent& event) { events.push_back(event); };
  if (bytewise) {
    for (const char c : stream) parser.feed(std::string(1, c), on_event);
  } else {
    parser.feed(stream, on_event);
  }
  parser.finish(on_event);
  return events;
}

class McpSseTest : public ::testing::TestWithParam<bool> {};

TEST_P(McpSseTest, DispatchesOnBlankLinesAndJoinsDataLines) {
  const std::vector<SseEvent> events = parse_all(
      "event: message\ndata: {\"a\":\ndata: 1}\n\ndata: second\n\n", GetParam());
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ("message", events[0].event);
  EXPECT_EQ("{\"a\":\n1}", events[0].data);
  EXPECT_EQ("second", events[1].data);
}

TEST_P(McpSseTest, AcceptsCrLfAndBareCrLineEnds) {
  const std::vector<SseEvent> events =
      parse_all("data: one\r\n\r\ndata: two\r\rdata: three\n\n", GetParam());
  ASSERT_EQ(3u, events.size());
  EXPECT_EQ("one", events[0].data);
  EXPECT_EQ("two", events[1].data);
  EXPECT_EQ("three", events[2].data);
}

TEST_P(McpSseTest, IgnoresCommentsAndEventsWithoutData) {
  const std::vector<SseEvent> events =
      parse_all(": keep-alive\n\nevent: ping\n\nid: 7\ndata: x\n\n", GetParam());
  ASSERT_EQ(1u, events.size());
  EXPECT_EQ("x", events[0].data);
  EXPECT_EQ("7", events[0].id);
  EXPECT_EQ("message", events[0].event);
}

TEST_P(McpSseTest, FlushesAFinalEventAtEndOfStream) {
  const std::vector<SseEvent> events = parse_all("data: last", GetParam());
  ASSERT_EQ(1u, events.size());
  EXPECT_EQ("last", events[0].data);
}

TEST_P(McpSseTest, KeepsOnlyOneLeadingSpaceOfAValue) {
  const std::vector<SseEvent> events = parse_all("data:  two spaces\n\n", GetParam());
  ASSERT_EQ(1u, events.size());
  EXPECT_EQ(" two spaces", events[0].data);
}

INSTANTIATE_TEST_SUITE_P(WholeAndBytewise, McpSseTest, ::testing::Bool());

// A 2025-11-25 server primes a stream with an id and no data; the id still
// becomes the reconnect point, and it persists across later events.
TEST(McpSseResumeTest, KeepsTheLastEventIdEvenFromAnEventWithoutData) {
  std::vector<SseEvent> events;
  SseParser parser;
  const auto on_event = [&](const SseEvent& event) { events.push_back(event); };

  parser.feed("id: prime-1\n\n", on_event);
  EXPECT_TRUE(events.empty());
  EXPECT_EQ("prime-1", parser.last_event_id());

  parser.feed("id: 7\ndata: {}\n\ndata: no id here\n\n", on_event);
  ASSERT_EQ(2u, events.size());
  EXPECT_EQ("7", events[0].id);
  EXPECT_EQ("7", parser.last_event_id());

  // An id with a NUL in it is ignored rather than adopted.
  parser.feed(std::string("id: a\0b\n\n", 9), on_event);
  EXPECT_EQ("7", parser.last_event_id());
}

TEST(McpSseResumeTest, ReadsRetryOnlyWhenItIsAllDigits) {
  SseParser parser;
  const auto ignore = [](const SseEvent&) {};
  EXPECT_EQ(-1, parser.retry_ms());
  parser.feed("retry: 2500\n\n", ignore);
  EXPECT_EQ(2500, parser.retry_ms());
  parser.feed("retry: soon\n\nretry: -5\n\n", ignore);
  EXPECT_EQ(2500, parser.retry_ms());
}

TEST(McpSseLimitTest, DropsAnOversizedEventButKeepsTheNext) {
  std::vector<SseEvent> events;
  SseParser parser(16);
  const auto on_event = [&](const SseEvent& event) { events.push_back(event); };
  parser.feed("data: " + std::string(64, 'x') + "\n\ndata: ok\n\n", on_event);
  parser.finish(on_event);
  ASSERT_EQ(1u, events.size());
  EXPECT_EQ("ok", events[0].data);
  EXPECT_TRUE(parser.overflowed());
}

}  // namespace
}  // namespace mcp
