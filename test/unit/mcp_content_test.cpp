// What a server's results look like by the time the model reads them.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include <core/mcp/content.h>
#include <core/util/text.h>

namespace mcp {
namespace {

using util::JsonValue;

JsonValue json(const std::string& text) {
  std::optional<JsonValue> value = JsonValue::parse(text);
  EXPECT_TRUE(value.has_value()) << text;
  return value.value_or(JsonValue());
}

bool has(const std::string& text, const std::string& needle) {
  return text.find(needle) != std::string::npos;
}

std::string file_contents(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::stringstream out;
  out << in.rdbuf();
  return out.str();
}

TEST(McpContentTest, TextBlocksAreJoinedAndErrorsFlagged) {
  const Rendered rendered = render_tool_result(json(
      R"({"content":[{"type":"text","text":"first"},{"type":"text","text":"second"}],"isError":true})"));
  EXPECT_EQ("first\nsecond", rendered.text);
  EXPECT_TRUE(rendered.is_error);
}

TEST(McpContentTest, ImagesAreSavedAndNamedByPath) {
  // "hello" in base64, labelled as a PNG.
  const Rendered rendered = render_tool_result(
      json(R"({"content":[{"type":"image","data":"aGVsbG8=","mimeType":"image/png"}]})"));
  ASSERT_EQ(1u, rendered.saved_files.size());
  EXPECT_TRUE(has(rendered.text, "image/png")) << rendered.text;
  EXPECT_TRUE(has(rendered.text, rendered.saved_files[0])) << rendered.text;
  EXPECT_EQ("hello", file_contents(rendered.saved_files[0]));
  std::filesystem::remove(rendered.saved_files[0]);
}

TEST(McpContentTest, StructuredContentStandsInOnlyWithoutText) {
  const Rendered alone =
      render_tool_result(json(R"({"content":[],"structuredContent":{"temp":22.5}})"));
  EXPECT_TRUE(has(alone.text, "22.5")) << alone.text;

  const Rendered mirrored = render_tool_result(json(
      R"({"content":[{"type":"text","text":"22.5 degrees"}],"structuredContent":{"temp":22.5}})"));
  EXPECT_EQ("22.5 degrees", mirrored.text);
}

TEST(McpContentTest, LinksAndEmbeddedResourcesRender) {
  const Rendered rendered = render_tool_result(json(
      R"({"content":[{"type":"resource_link","uri":"file:///p/main.rs","name":"main.rs","mimeType":"text/x-rust"},{"type":"resource","resource":{"uri":"file:///p/a.txt","mimeType":"text/plain","text":"inside"}}]})"));
  EXPECT_TRUE(has(rendered.text, "file:///p/main.rs")) << rendered.text;
  EXPECT_TRUE(has(rendered.text, "inside")) << rendered.text;
}

TEST(McpContentTest, InvalidUtf8IsRepaired) {
  JsonValue result = JsonValue::object();
  JsonValue block = JsonValue::object();
  block.set("type", "text");
  block.set("text", std::string("bad \xff byte"));
  result["content"].push_back(block);
  const Rendered rendered = render_tool_result(result);
  EXPECT_TRUE(util::is_valid_utf8(rendered.text));
}

TEST(McpContentTest, LongOutputSpillsToAFileWithANote) {
  JsonValue result = JsonValue::object();
  JsonValue block = JsonValue::object();
  block.set("type", "text");
  block.set("text", std::string(40000, 'x'));
  result["content"].push_back(block);
  RenderOptions options;
  options.max_bytes = 1000;
  const Rendered rendered = render_tool_result(result, options);
  EXPECT_TRUE(rendered.truncated);
  EXPECT_LT(rendered.text.size(), 2000u);
  ASSERT_FALSE(rendered.overflow_path.empty());
  EXPECT_TRUE(has(rendered.text, rendered.overflow_path));
  std::filesystem::remove(rendered.overflow_path);
}

TEST(McpContentTest, ResourceContentsAndPromptsRender) {
  const Rendered contents = render_resource_contents(json(
      R"({"contents":[{"uri":"mem://a","mimeType":"text/plain","text":"alpha"}]})"));
  EXPECT_TRUE(has(contents.text, "alpha"));

  EXPECT_EQ("Review this code:\nint x;", render_prompt_messages(json(
      R"({"messages":[{"role":"user","content":{"type":"text","text":"Review this code:\nint x;"}}]})")));

  const std::string labelled = render_prompt_messages(json(
      R"({"messages":[{"role":"user","content":{"type":"text","text":"Q"}},{"role":"assistant","content":{"type":"text","text":"A"}}]})"));
  EXPECT_TRUE(has(labelled, "[assistant]")) << labelled;
}

}  // namespace
}  // namespace mcp
