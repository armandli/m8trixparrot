// Lowering MCP tool schemas into what Ollama's Go types accept, plus the
// argument fixing and x-mcp-header handling that sit beside it.
//
// The corpus test is the one that matters most: every schema in
// test/unit/data/mcp_schemas — real servers' and deliberately broken ones —
// must lower to something ollama_shape_problem() accepts, because a single one
// that does not would fail every /api/chat request it rides along on.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/mcp/schema.h>

namespace mcp {
namespace {

using util::JsonValue;

JsonValue json(const std::string& text) {
  std::string error;
  std::optional<JsonValue> value = JsonValue::parse(text, &error);
  EXPECT_TRUE(value.has_value()) << error << " in " << text;
  return value.value_or(JsonValue());
}

const JsonValue& property(const JsonValue& parameters, const std::string& name) {
  return parameters.get("properties").get(name);
}

TEST(McpSchemaCorpusTest, EverySchemaLowersToAnOllamaSafeShape) {
  size_t checked = 0;
  for (const auto& entry :
       std::filesystem::directory_iterator(M8_MCP_SCHEMA_DIR)) {
    if (entry.path().extension() != ".json") continue;
    std::ifstream in(entry.path());
    std::stringstream text;
    text << in.rdbuf();
    const JsonValue schema = json(text.str());

    const LoweredSchema lowered = lower_schema(schema);
    EXPECT_EQ("", ollama_shape_problem(lowered.parameters))
        << entry.path().filename() << " lowered to " << lowered.parameters.dump();
    EXPECT_EQ("object", lowered.parameters.get("type").as_string())
        << entry.path().filename();
    // The schema we send must itself be a parseable document.
    const std::string tool = tool_schema_json("mcp__t__x", "d", lowered.parameters);
    EXPECT_TRUE(JsonValue::parse(tool).has_value()) << entry.path().filename();
    ++checked;
  }
  EXPECT_GE(checked, 10u);
}

TEST(McpSchemaTest, ShapeCheckerCatchesWhatOllamaRejects) {
  EXPECT_NE("", ollama_shape_problem(json(R"({"type":["object","null"]})")));
  EXPECT_NE("", ollama_shape_problem(json(R"({"type":"object","properties":{"a":true}})")));
  EXPECT_NE("", ollama_shape_problem(json(R"({"type":"object","required":[1]})")));
  EXPECT_NE("", ollama_shape_problem(json(
                    R"({"type":"object","properties":{"a":{"description":{"x":1}}}})")));
  EXPECT_EQ("", ollama_shape_problem(json(
                    R"({"type":"object","properties":{"a":{"type":["string","integer"]}}})")));
}

TEST(McpSchemaTest, InlinesLocalRefsAndKeepsSiblingDescriptions) {
  const JsonValue parameters =
      lower_schema(json(R"({"type":"object","$defs":{"Point":{"type":"object","properties":{"x":{"type":"number"}},"required":["x"]}},"properties":{"at":{"$ref":"#/$defs/Point","description":"Where"}}})"))
          .parameters;
  const JsonValue& at = property(parameters, "at");
  EXPECT_EQ("object", at.get("type").as_string());
  EXPECT_EQ("number", at.get("properties").get("x").get("type").as_string());
  EXPECT_EQ("Where", at.get("description").as_string());
  EXPECT_FALSE(parameters.contains("$defs"));
}

TEST(McpSchemaTest, RecursiveRefsStopInsteadOfLooping) {
  const LoweredSchema lowered =
      lower_schema(json(R"({"type":"object","properties":{"n":{"$ref":"#"}}})"));
  EXPECT_EQ("", ollama_shape_problem(lowered.parameters));
}

TEST(McpSchemaTest, ExternalRefsAreNotFetchedButNoted) {
  const LoweredSchema lowered = lower_schema(
      json(R"({"type":"object","properties":{"c":{"$ref":"https://example.com/s.json"}}})"));
  const std::string description =
      property(lowered.parameters, "c").get("description").as_string();
  EXPECT_NE(std::string::npos, description.find("example.com")) << description;
  ASSERT_FALSE(lowered.notes.empty());
}

TEST(McpSchemaTest, NullableUnionsBecomeThePlainType) {
  const JsonValue parameters = lower_schema(json(
      R"({"type":"object","properties":{"a":{"type":["string","null"]},"b":{"anyOf":[{"type":"integer"},{"type":"null"}]}}})"))
                                   .parameters;
  EXPECT_EQ("string", property(parameters, "a").get("type").as_string());
  EXPECT_EQ("integer", property(parameters, "b").get("type").as_string());
}

TEST(McpSchemaTest, SimpleAlternativesFoldIntoATypeUnion) {
  const JsonValue parameters = lower_schema(json(
      R"({"type":"object","properties":{"v":{"oneOf":[{"type":"string"},{"type":"number"}]}}})"))
                                   .parameters;
  const JsonValue& type = property(parameters, "v").get("type");
  ASSERT_TRUE(type.is_array()) << parameters.dump();
  EXPECT_EQ(2u, type.size());
}

TEST(McpSchemaTest, StructuredAlternativesStayAsAnyOf) {
  const JsonValue parameters = lower_schema(json(
      R"({"type":"object","properties":{"v":{"oneOf":[{"type":"object","properties":{"a":{"type":"string"}}},{"type":"string"}]}}})"))
                                   .parameters;
  EXPECT_EQ(2u, property(parameters, "v").get("anyOf").size()) << parameters.dump();
}

TEST(McpSchemaTest, AllOfMerges) {
  const JsonValue parameters =
      lower_schema(json(
                       R"({"allOf":[{"type":"object","properties":{"a":{"type":"string"}},"required":["a"]},{"properties":{"b":{"type":"integer"}},"required":["b"]}]})"))
          .parameters;
  EXPECT_TRUE(parameters.get("properties").contains("a"));
  EXPECT_TRUE(parameters.get("properties").contains("b"));
  EXPECT_EQ(2u, parameters.get("required").size());
}

TEST(McpSchemaTest, RootAlternativesOfferTheUnionRequiringTheCommonPart) {
  const JsonValue parameters =
      lower_schema(json(
                       R"({"oneOf":[{"type":"object","properties":{"id":{"type":"string"},"mode":{"type":"string"}},"required":["id","mode"]},{"type":"object","properties":{"url":{"type":"string"},"mode":{"type":"string"}},"required":["url","mode"]}]})"))
          .parameters;
  EXPECT_EQ(3u, parameters.get("properties").size()) << parameters.dump();
  ASSERT_EQ(1u, parameters.get("required").size());
  EXPECT_EQ("mode", parameters.get("required").items()[0].as_string());
}

TEST(McpSchemaTest, BooleanSchemasResolve) {
  const JsonValue parameters = lower_schema(json(
      R"({"type":"object","properties":{"any":true,"never":false},"required":["any","never"]})"))
                                   .parameters;
  EXPECT_TRUE(parameters.get("properties").contains("any"));
  EXPECT_FALSE(parameters.get("properties").contains("never"));
  ASSERT_EQ(1u, parameters.get("required").size());
}

TEST(McpSchemaTest, DroppedConstraintsAreFoldedIntoTheDescription) {
  const JsonValue parameters = lower_schema(json(
      R"({"type":"object","properties":{"n":{"type":"integer","description":"Count","minimum":1,"maximum":10,"default":3},"d":{"type":"string","format":"date-time"},"k":{"const":"fixed"}}})"))
                                   .parameters;
  const std::string n = property(parameters, "n").get("description").as_string();
  EXPECT_NE(std::string::npos, n.find("Count")) << n;
  EXPECT_NE(std::string::npos, n.find("min 1")) << n;
  EXPECT_NE(std::string::npos, n.find("max 10")) << n;
  EXPECT_NE(std::string::npos, n.find("default 3")) << n;
  EXPECT_NE(std::string::npos,
            property(parameters, "d").get("description").as_string().find("date-time"));
  EXPECT_EQ("fixed", property(parameters, "k").get("enum").items()[0].as_string());
  EXPECT_FALSE(property(parameters, "n").contains("minimum"));
}

TEST(McpSchemaTest, SignaturesReadLikeCalls) {
  const JsonValue parameters = lower_schema(json(
      R"({"type":"object","properties":{"path":{"type":"string"},"recursive":{"type":"boolean"},"mode":{"enum":["r","w"]},"tags":{"type":"array","items":{"type":"string"}}},"required":["path","mode"]})"))
                                   .parameters;
  EXPECT_EQ(R"(fs_list(path: string, recursive?: boolean, mode: "r"|"w", tags?: string[]))",
            tool_signature("fs_list", parameters));
}

// ─────────────────────────────── arguments ──────────────────────────────────

TEST(McpArgumentsTest, AcceptsWhatSmallModelsSend) {
  std::string error;
  EXPECT_EQ(0u, arguments_object("", error).size());
  EXPECT_EQ(0u, arguments_object("null", error).size());
  EXPECT_EQ(1u, arguments_object(R"("{\"a\":1}")", error).size());
  EXPECT_TRUE(error.empty()) << error;
  arguments_object("[1,2]", error);
  EXPECT_FALSE(error.empty());
}

TEST(McpArgumentsTest, CoercesTowardTheSchema) {
  const JsonValue parameters = lower_schema(json(
      R"({"type":"object","properties":{"n":{"type":"integer"},"f":{"type":"number"},"b":{"type":"boolean"},"o":{"type":"object","properties":{"x":{"type":"integer"}}},"a":{"type":"array","items":{"type":"string"}},"s":{"type":"string"},"keep":{"type":"string"}}})"))
                                   .parameters;
  std::vector<std::string> fixes;
  const JsonValue fixed = fix_arguments(
      json(R"({"n":"5","f":"2.5","b":"TRUE","o":"{\"x\":\"7\"}","a":"[\"p\",3]","s":42,"keep":"text","extra":"left alone"})"),
      parameters, &fixes);
  EXPECT_EQ(5, fixed.get("n").as_int());
  EXPECT_TRUE(fixed.get("n").is_int());
  EXPECT_DOUBLE_EQ(2.5, fixed.get("f").as_double());
  EXPECT_EQ(true, fixed.get("b").as_bool());
  EXPECT_EQ(7, fixed.get("o").get("x").as_int());
  EXPECT_EQ("3", fixed.get("a").items()[1].as_string());
  EXPECT_EQ("42", fixed.get("s").as_string());
  EXPECT_EQ("text", fixed.get("keep").as_string());
  EXPECT_EQ("left alone", fixed.get("extra").as_string());
  EXPECT_FALSE(fixes.empty());
}

// ────────────────────────────── x-mcp-header ────────────────────────────────

TEST(McpHeaderParamTest, CollectsAnnotationsReachableThroughProperties) {
  std::vector<HeaderParam> params;
  EXPECT_EQ("", collect_header_params(
                    json(R"({"type":"object","properties":{"region":{"type":"string","x-mcp-header":"Region"},"opts":{"type":"object","properties":{"tenant":{"type":"integer","x-mcp-header":"Tenant"}}},"query":{"type":"string"}}})"),
                    params));
  ASSERT_EQ(2u, params.size());
  EXPECT_EQ("Region", params[0].name);
  EXPECT_EQ((std::vector<std::string>{"opts", "tenant"}), params[1].path);

  const auto headers = header_values(
      params, json(R"({"region":"us-west1","opts":{"tenant":42},"query":"x"})"));
  ASSERT_EQ(2u, headers.size());
  EXPECT_EQ("Mcp-Param-Region", headers[0].first);
  EXPECT_EQ("us-west1", headers[0].second);
  EXPECT_EQ("42", headers[1].second);
  // Absent and null values produce no header.
  EXPECT_TRUE(header_values(params, json(R"({"region":null})")).empty());
}

TEST(McpHeaderParamTest, InvalidAnnotationsDisqualifyTheTool) {
  std::vector<HeaderParam> params;
  // On a number.
  EXPECT_NE("", collect_header_params(
                    json(R"({"properties":{"n":{"type":"number","x-mcp-header":"N"}}})"),
                    params));
  // Not a token.
  EXPECT_NE("", collect_header_params(
                    json(R"({"properties":{"s":{"type":"string","x-mcp-header":"Bad Name"}}})"),
                    params));
  // Used twice, ignoring case.
  EXPECT_NE("", collect_header_params(
                    json(R"({"properties":{"a":{"type":"string","x-mcp-header":"R"},"b":{"type":"string","x-mcp-header":"r"}}})"),
                    params));
  // Inside items: not statically reachable.
  EXPECT_NE("", collect_header_params(
                    json(R"({"properties":{"a":{"type":"array","items":{"type":"string","x-mcp-header":"A"}}}})"),
                    params));
  EXPECT_TRUE(params.empty());
}

// The examples table from the 2026-07-28 Streamable HTTP page.
TEST(McpHeaderParamTest, EncodesValuesTheWayTheSpecShows) {
  EXPECT_EQ("us-west1", encode_header_value("us-west1"));
  EXPECT_EQ("=?base64?SGVsbG8sIOS4lueVjA==?=",
            encode_header_value("Hello, \xe4\xb8\x96\xe7\x95\x8c"));
  EXPECT_EQ("=?base64?IHBhZGRlZCA=?=", encode_header_value(" padded "));
  EXPECT_EQ("=?base64?bGluZTEKbGluZTI=?=", encode_header_value("line1\nline2"));
  EXPECT_EQ("=?base64?PT9iYXNlNjQ/bGl0ZXJhbD89?=",
            encode_header_value("=?base64?literal?="));
}

}  // namespace
}  // namespace mcp
