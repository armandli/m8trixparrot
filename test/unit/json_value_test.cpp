// util::JsonValue: the mutable JSON tree behind MCP schema rewriting, argument
// fixing and config editing. The properties that matter to those callers are
// tested here — insertion order survives, edits stay in place, nothing throws,
// and whatever goes out is a parseable document.

#include <string>

#include <gtest/gtest.h>

#include <core/util/json_value.h>

namespace util {
namespace {

JsonValue parsed(const std::string& text) {
  std::string error;
  const std::optional<JsonValue> value = JsonValue::parse(text, &error);
  EXPECT_TRUE(value.has_value()) << error;
  return value.value_or(JsonValue());
}

TEST(JsonValueTest, RoundTripsEveryType) {
  const std::string text =
      R"({"n":null,"t":true,"f":false,"i":-42,"d":2.5,"s":"x\"y","a":[1,"two",[3]],"o":{"k":{}}})";
  EXPECT_EQ(text, parsed(text).dump());
}

TEST(JsonValueTest, ObjectsKeepInsertionOrder) {
  JsonValue value = parsed(R"({"zeta":1,"alpha":2,"mid":3})");
  value.set("beta", 4);
  EXPECT_EQ(R"({"zeta":1,"alpha":2,"mid":3,"beta":4})", value.dump());
}

// A rewritten member stays where the user had it, so an edited config file
// reads like the one they wrote.
TEST(JsonValueTest, SetReplacesInPlace) {
  JsonValue value = parsed(R"({"a":1,"b":2,"c":3})");
  value.set("b", "two");
  EXPECT_EQ(R"({"a":1,"b":"two","c":3})", value.dump());
  EXPECT_TRUE(value.erase("a"));
  EXPECT_FALSE(value.erase("missing"));
  EXPECT_EQ(R"({"b":"two","c":3})", value.dump());
}

TEST(JsonValueTest, IndexingBuildsNestedObjectsFromNull) {
  JsonValue root;
  root["mcpServers"]["fs"]["command"] = "npx";
  root["mcpServers"]["fs"]["args"].push_back("-y");
  EXPECT_EQ(R"({"mcpServers":{"fs":{"command":"npx","args":["-y"]}}})",
            root.dump());
}

TEST(JsonValueTest, ReadsOfTheWrongTypeFallBack) {
  const JsonValue value = parsed(R"({"s":"text","i":7,"d":7.0,"h":7.5})");
  EXPECT_EQ("text", value.get("s").as_string());
  EXPECT_EQ("", value.get("i").as_string());
  EXPECT_EQ(7, value.get("i").as_int());
  EXPECT_EQ(7, value.get("d").as_int());   // a whole double is an int
  EXPECT_EQ(-1, value.get("h").as_int(-1));  // a fraction is not
  EXPECT_DOUBLE_EQ(7.5, value.get("h").as_double());
  EXPECT_TRUE(value.get("absent").is_null());
  EXPECT_TRUE(value.get("s").get("deeper").is_null());
  EXPECT_EQ(0u, value.get("s").items().size());
}

TEST(JsonValueTest, LargeUnsignedIntegersBecomeDoubles) {
  const JsonValue value = parsed("18446744073709551615");
  EXPECT_TRUE(value.is_double());
  EXPECT_TRUE(parsed("9223372036854775807").is_int());
}

TEST(JsonValueTest, MalformedInputIsReportedNotThrown) {
  std::string error;
  EXPECT_FALSE(JsonValue::parse("{\"a\":", &error).has_value());
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(JsonValue::parse("", &error).has_value());
}

// A string built from process output can hold any bytes; the document written
// out must still parse, or a later Ollama request body would come out empty.
TEST(JsonValueTest, InvalidUtf8IsRepairedOnTheWayOut) {
  JsonValue value = JsonValue::object();
  value.set("text", std::string("ok \xff bad"));
  const std::string out = value.dump();
  ASSERT_FALSE(out.empty());
  EXPECT_TRUE(JsonValue::parse(out).has_value()) << out;
}

TEST(JsonValueTest, PrettyOutputParsesBackEqual) {
  const JsonValue value =
      parsed(R"({"mcpServers":{"a":{"command":"x","args":["1","2"]}},"n":1})");
  const std::string pretty = value.dump_pretty();
  EXPECT_NE(std::string::npos, pretty.find('\n'));
  EXPECT_EQ(value, parsed(pretty));
}

TEST(JsonValueTest, EqualityIgnoresKeyOrderAndNumberSpelling) {
  EXPECT_EQ(parsed(R"({"a":1,"b":[2.0]})"), parsed(R"({"b":[2],"a":1.0})"));
  EXPECT_NE(parsed(R"({"a":1})"), parsed(R"({"a":"1"})"));
}

}  // namespace
}  // namespace util
