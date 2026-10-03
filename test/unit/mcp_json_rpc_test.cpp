// JSON-RPC framing for MCP: classifying what arrives, building what leaves.

#include <string>

#include <gtest/gtest.h>

#include <core/mcp/json_rpc.h>
#include <core/mcp/protocol.h>

namespace mcp {
namespace {

TEST(McpJsonRpcTest, ClassifiesEveryMessageKind) {
  EXPECT_EQ(MessageKind::Request,
            parse_message(R"({"jsonrpc":"2.0","id":1,"method":"ping"})").kind);
  EXPECT_EQ(MessageKind::Notification,
            parse_message(R"({"jsonrpc":"2.0","method":"notifications/x"})").kind);
  EXPECT_EQ(MessageKind::Result,
            parse_message(R"({"jsonrpc":"2.0","id":"a","result":{}})").kind);
  EXPECT_EQ(MessageKind::Error,
            parse_message(R"({"jsonrpc":"2.0","id":2,"error":{"code":-32601,"message":"no"}})")
                .kind);
  EXPECT_EQ(MessageKind::Invalid, parse_message("not json").kind);
  EXPECT_EQ(MessageKind::Invalid, parse_message("[1,2]").kind);
  EXPECT_EQ(MessageKind::Invalid, parse_message(R"({"jsonrpc":"2.0","id":3})").kind);
}

TEST(McpJsonRpcTest, ParsesErrorFields) {
  const RpcMessage message = parse_message(
      R"({"jsonrpc":"2.0","id":1,"error":{"code":-32022,"message":"Unsupported protocol version","data":{"supported":["2025-11-25"]}}})");
  ASSERT_EQ(MessageKind::Error, message.kind);
  EXPECT_EQ(kUnsupportedProtocolVersion, message.error.code);
  EXPECT_EQ("2025-11-25",
            message.error.data.get("supported").items().front().as_string());
  EXPECT_TRUE(is_modern_error(message.error.code));
  EXPECT_FALSE(is_modern_error(kMethodNotFound));
}

// stdio frames messages by newline, so whatever we send must be one line even
// when a value holds newlines.
TEST(McpJsonRpcTest, BuiltMessagesAreSingleLine) {
  util::JsonValue params = util::JsonValue::object();
  params.set("text", "line one\nline two");
  const std::string request = make_request(util::JsonValue(7), "tools/call", params);
  EXPECT_EQ(std::string::npos, request.find('\n'));
  const RpcMessage parsed = parse_message(request);
  EXPECT_EQ(MessageKind::Request, parsed.kind);
  EXPECT_EQ("tools/call", parsed.method);
  EXPECT_EQ("line one\nline two", parsed.params.get("text").as_string());
}

TEST(McpJsonRpcTest, ResultsAndErrorsEchoTheId) {
  const std::string result = make_result(util::JsonValue("abc"), util::JsonValue());
  EXPECT_EQ(R"({"jsonrpc":"2.0","id":"abc","result":{}})", result);
  const std::string error = make_error(util::JsonValue(4), kMethodNotFound, "nope");
  EXPECT_EQ(R"({"jsonrpc":"2.0","id":4,"error":{"code":-32601,"message":"nope"}})",
            error);
  EXPECT_EQ(R"({"jsonrpc":"2.0","method":"notifications/initialized"})",
            make_notification("notifications/initialized", util::JsonValue()));
}

TEST(McpJsonRpcTest, IdKeysKeepNumbersAndStringsApart) {
  EXPECT_NE(id_key(util::JsonValue(7)), id_key(util::JsonValue("7")));
  EXPECT_EQ(id_key(util::JsonValue(7)), id_key(util::JsonValue(7.0)));
}

TEST(McpProtocolTest, ParsesToolsAndCapabilities) {
  const std::optional<util::JsonValue> tools = util::JsonValue::parse(
      R"([{"name":"echo","description":"Echoes","inputSchema":{"type":"object"},"annotations":{"readOnlyHint":true,"title":"Echo"}},{"description":"no name"}])");
  ASSERT_TRUE(tools);
  const std::vector<ToolInfo> parsed = parse_tools(*tools);
  ASSERT_EQ(1u, parsed.size());
  EXPECT_EQ("echo", parsed[0].name);
  EXPECT_EQ("Echo", parsed[0].title);  // legacy title location
  EXPECT_EQ(true, parsed[0].annotations.read_only.value_or(false));

  const std::optional<util::JsonValue> capabilities = util::JsonValue::parse(
      R"({"tools":{"listChanged":true},"resources":{},"prompts":{"listChanged":false}})");
  ASSERT_TRUE(capabilities);
  const ServerCapabilities caps = parse_capabilities(*capabilities);
  EXPECT_TRUE(caps.tools);
  EXPECT_TRUE(caps.tools_list_changed);
  EXPECT_TRUE(caps.resources);
  EXPECT_FALSE(caps.resources_subscribe);
  EXPECT_TRUE(caps.prompts);
  EXPECT_FALSE(caps.prompts_list_changed);
}

TEST(McpProtocolTest, KnowsTheLegacyVersions) {
  for (const char* version : {"2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"}) {
    EXPECT_TRUE(is_known_legacy_version(version)) << version;
  }
  EXPECT_FALSE(is_known_legacy_version(kModernVersion));
  EXPECT_FALSE(is_known_legacy_version("1.0"));
}

}  // namespace
}  // namespace mcp
