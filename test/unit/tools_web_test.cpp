// What `webfetch` retrieves, how it renders it, and what it refuses.
//
// Every test here talks to a loopback HTTP server rather than the network, so
// the suite is offline and deterministic. The server exists because
// Content-Type is what the tool dispatches on, and a file:// URL carries none.

#include <string>

#include <gtest/gtest.h>

#include <core/tools.hpp>
#include <loopback_server.hpp>
#include <tool_test_env.hpp>

namespace agent::test {
namespace {

struct WebFetchTest : ToolTest {};

// The body of a fetch, with the "[fetched URL (type)]" header line removed.
std::string body_of(const ToolResult& result) {
  const size_t newline = result.output.find('\n');
  if (newline == std::string::npos) return std::string();
  return result.output.substr(newline + 1);
}

TEST_F(WebFetchTest, PlainTextIsReturnedAsIsBehindAFetchedHeaderLine) {
  LoopbackServer server(200, "text/plain", "just some text\n");

  const ToolResult result =
      WebFetchTool().execute(args({{"url", str(server.url("/notes.txt"))}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(result.output,
            "[fetched " + server.url("/notes.txt") + " (text/plain)]\n" +
                "just some text\n");
}

TEST_F(WebFetchTest, HtmlIsRenderedToMarkdown) {
  LoopbackServer server(200, "text/html",
                        "<html><body>"
                        "<h1>Title</h1>"
                        "<p>A paragraph.</p>"
                        "<ul><li>first</li><li>second</li></ul>"
                        "</body></html>");

  const ToolResult result =
      WebFetchTool().execute(args({{"url", str(server.url("/page"))}}));

  ASSERT_TRUE(result.ok);
  const std::string body = body_of(result);
  EXPECT_NE(body.find("# Title"), std::string::npos);
  EXPECT_NE(body.find("A paragraph."), std::string::npos);
  EXPECT_NE(body.find("- first"), std::string::npos);
  EXPECT_NE(body.find("- second"), std::string::npos);
  // Tags themselves never survive.
  EXPECT_EQ(body.find("<h1>"), std::string::npos);
}

// A relative href is resolved against the URL that was actually fetched, so
// the markdown link is usable on its own.
TEST_F(WebFetchTest, RelativeLinksAreResolvedAgainstTheFetchedUrl) {
  LoopbackServer server(200, "text/html",
                        "<a href=\"/other\">elsewhere</a>");

  const ToolResult result =
      WebFetchTool().execute(args({{"url", str(server.url("/dir/page"))}}));

  ASSERT_TRUE(result.ok);
  EXPECT_NE(body_of(result).find("[elsewhere](" + server.url("/other") + ")"),
            std::string::npos);
}

TEST_F(WebFetchTest, HtmlEntitiesAreDecoded) {
  LoopbackServer server(200, "text/html", "<p>a &amp; b &lt; c</p>");

  const ToolResult result =
      WebFetchTool().execute(args({{"url", str(server.url())}}));

  ASSERT_TRUE(result.ok);
  EXPECT_NE(body_of(result).find("a & b < c"), std::string::npos);
}

// The charset parameter is stripped before the media type is matched, so
// "text/html; charset=utf-8" still takes the HTML path.
TEST_F(WebFetchTest, ACharsetParameterIsStrippedBeforeDispatch) {
  LoopbackServer server(200, "text/html; charset=utf-8", "<h1>Heading</h1>");

  const ToolResult result =
      WebFetchTool().execute(args({{"url", str(server.url())}}));

  ASSERT_TRUE(result.ok);
  EXPECT_NE(body_of(result).find("# Heading"), std::string::npos);
}

TEST_F(WebFetchTest, JsonIsTreatedAsTextAndPassedThroughUnrendered) {
  LoopbackServer server(200, "application/json", "{\"key\":\"value\"}");

  const ToolResult result =
      WebFetchTool().execute(args({{"url", str(server.url())}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(body_of(result), "{\"key\":\"value\"}");
}

// With no Content-Type at all the body is sniffed: readable bytes are text.
TEST_F(WebFetchTest, WithNoContentTypeTheBodyIsSniffed) {
  LoopbackServer server(200, "", "plain enough\n");

  const ToolResult result =
      WebFetchTool().execute(args({{"url", str(server.url())}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(body_of(result), "plain enough\n");
}

TEST_F(WebFetchTest, WithNoContentTypeABodyContainingNulIsRefused) {
  LoopbackServer server(200, "", std::string("bytes\0here", 10));

  const ToolResult result =
      WebFetchTool().execute(args({{"url", str(server.url())}}));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("returned unsupported content (binary)"),
            std::string::npos);
}

TEST_F(WebFetchTest, AnImageContentTypeIsRefusedAsUnsupported) {
  LoopbackServer server(200, "image/png", "\x89PNG");

  const ToolResult result =
      WebFetchTool().execute(args({{"url", str(server.url())}}));

  EXPECT_FALSE(result.ok);
  EXPECT_NE(result.error.find("unsupported content type image/png"),
            std::string::npos);
}

// A non-2xx response is an error rather than content, but the status is named
// so a caller can tell a 404 from a 500.
TEST_F(WebFetchTest, ANonSuccessStatusIsAnErrorThatNamesTheStatus) {
  LoopbackServer server(404, "text/html", "<h1>Not Found</h1>");

  const ToolResult result =
      WebFetchTool().execute(args({{"url", str(server.url("/missing"))}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error,
            "webfetch: " + server.url("/missing") + " returned HTTP 404");
  EXPECT_TRUE(result.output.empty());
}

TEST_F(WebFetchTest, AnUnreachableHostIsReportedAsACurlError) {
  // Port 1 on loopback has nothing listening.
  const ToolResult result =
      WebFetchTool().execute(args({{"url", str("http://127.0.0.1:1/")}}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error.rfind("webfetch: ", 0), 0u);
}

TEST_F(WebFetchTest, MissingUrlIsAnError) {
  const ToolResult result = WebFetchTool().execute(args({}));

  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.error, "webfetch: missing required string argument 'url'");
}

// Deliberate, not an oversight: webfetch applies no scheme or host
// restriction, so a URL naming loopback or a cloud metadata endpoint is
// fetched like any other. src/core/tools.hpp says so explicitly and puts the
// responsibility on whatever drives the tool. Every test above relies on this;
// this one states it, so that adding a host check is understood as a behavior
// change rather than a bug fix.
TEST_F(WebFetchTest, LoopbackAddressesAreFetchedBecauseThereIsNoHostRestriction) {
  LoopbackServer server(200, "text/plain", "reached loopback");

  const ToolResult result =
      WebFetchTool().execute(args({{"url", str(server.url())}}));

  EXPECT_TRUE(result.ok);
  EXPECT_EQ(body_of(result), "reached loopback");
}

// Not tested: the 10MB download cap and its "[download stopped at 10MB]" note.
// Exercising it means serving more than 10MB over loopback, which costs more
// run time than the assertion is worth. The cap lives at kMaxDownloadBytes in
// src/core/tools_web.cpp.

}  // namespace
}  // namespace agent::test
