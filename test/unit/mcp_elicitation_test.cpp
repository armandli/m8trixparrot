// The elicitation engine, driven by a scripted person: forms field by field
// with defaults, coercion and validation, a review before anything is sent,
// decline and cancel at any point, and link consent that never opens a link
// without a yes.

#include <deque>
#include <set>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <core/mcp/elicitation.h>
#include <core/util/json_value.h>

namespace mcp {
namespace {

using util::JsonValue;

JsonValue json(std::string_view text) { return JsonValue::parse(text).value_or(JsonValue()); }

// Answers questions from a script, keeping every question it was asked.
struct ScriptedPerson {
  std::deque<Response> script;
  std::vector<Question> asked;

  Response operator()(const Question& question) {
    asked.push_back(question);
    if (script.empty()) return Response{};  // ran out: cancel
    Response next = script.front();
    script.pop_front();
    return next;
  }

  AskFunction ask() {
    return [this](const Question& question) { return (*this)(question); };
  }
};

Response says(const std::string& text) { return Response{Response::Kind::Answered, text}; }
Response declines() { return Response{Response::Kind::Declined, ""}; }
Response cancels() { return Response{Response::Kind::Cancelled, ""}; }

ElicitationRequest form(const std::string& schema, const std::string& message = "Details?") {
  ElicitationRequest request;
  request.server = "shop";
  request.scope = "project";
  request.agent_label = "root";
  request.message = message;
  request.requested_schema = json(schema);
  return request;
}

const OpenUrlFunction kNoBrowser = [](const std::string&, std::string& error) {
  error = "no display";
  return false;
};

TEST(McpElicitationTest, AFormIsAskedFieldByFieldThenReviewed) {
  ScriptedPerson person;
  person.script = {says("Ada"), says("36"), says("yes"), says("")};
  const ElicitationResult result = run_elicitation(
      form(R"({"type":"object","properties":{
        "name":{"type":"string","title":"Name","minLength":2},
        "age":{"type":"integer","minimum":0,"maximum":150},
        "subscribe":{"type":"boolean"}},"required":["name"]})"),
      person.ask(), kNoBrowser);

  EXPECT_EQ(result.action, "accept");
  EXPECT_EQ(result.content, json(R"({"name":"Ada","age":36,"subscribe":true})"));
  ASSERT_EQ(person.asked.size(), 4u);
  // Who asks, from where, for whom — on every question.
  EXPECT_EQ(person.asked[0].heading, "shop (project) asks, for root");
  EXPECT_EQ(person.asked[0].message, "Details?");
  EXPECT_EQ(person.asked[0].progress, "field 1 of 3");
  EXPECT_NE(person.asked[0].detail.find("Name (required)"), std::string::npos);
  EXPECT_NE(person.asked[1].detail.find("a whole number, from 0 to 150"), std::string::npos);
  // The review shows everything before it is sent.
  EXPECT_EQ(person.asked[3].progress, "review");
  EXPECT_NE(person.asked[3].detail.find("Name: Ada"), std::string::npos);
  EXPECT_NE(person.asked[3].detail.find("subscribe: yes"), std::string::npos);
}

TEST(McpElicitationTest, BadAnswersAreAskedAgainWithWhy) {
  ScriptedPerson person;
  person.script = {says("x"), says("Al"), says("not an email"), says("al@example.com"), says("")};
  const ElicitationResult result = run_elicitation(
      form(R"({"type":"object","properties":{
        "name":{"type":"string","minLength":2,"maxLength":10},
        "email":{"type":"string","format":"email"}},"required":["name","email"]})"),
      person.ask(), kNoBrowser);
  EXPECT_EQ(result.action, "accept");
  ASSERT_EQ(person.asked.size(), 5u);
  EXPECT_NE(person.asked[1].problem.find("2 to 10"), std::string::npos) << person.asked[1].problem;
  EXPECT_EQ(person.asked[1].initial, "x");  // what they typed, to fix
  EXPECT_NE(person.asked[3].problem.find("email"), std::string::npos);
}

TEST(McpElicitationTest, DefaultsArePrefilledAndAnEmptyAnswerLeavesAnOptionalFieldOut) {
  ScriptedPerson person;
  person.script = {says("blue"), says(""), says("")};
  const ElicitationResult result = run_elicitation(
      form(R"({"type":"object","properties":{
        "color":{"type":"string","enum":["red","blue"],"default":"red"},
        "note":{"type":"string"}}})"),
      person.ask(), kNoBrowser);
  EXPECT_EQ(result.action, "accept");
  EXPECT_EQ(result.content, json(R"({"color":"blue"})"));
  EXPECT_EQ(person.asked[0].initial, "red");
  EXPECT_NE(person.asked[2].detail.find("note: (left out)"), std::string::npos);
}

TEST(McpElicitationTest, ARequiredFieldCannotBeLeftEmpty) {
  ScriptedPerson person;
  person.script = {says(""), says("ok"), says("")};
  const ElicitationResult result = run_elicitation(
      form(R"({"type":"object","properties":{"x":{"type":"string"}},"required":["x"]})"),
      person.ask(), kNoBrowser);
  EXPECT_EQ(result.action, "accept");
  EXPECT_EQ(person.asked[1].problem, "this one is required");
}

TEST(McpElicitationTest, TheReviewCanGoBackAndChangeAnswers) {
  ScriptedPerson person;
  person.script = {says("Ada"), says("e"), says("Grace"), says("y")};
  const ElicitationResult result = run_elicitation(
      form(R"({"type":"object","properties":{"name":{"type":"string"}}})"), person.ask(),
      kNoBrowser);
  EXPECT_EQ(result.action, "accept");
  EXPECT_EQ(result.content, json(R"({"name":"Grace"})"));
  EXPECT_EQ(person.asked[2].initial, "Ada");  // the edit pass starts from the answer
}

TEST(McpElicitationTest, DeclineAndCancelEndItAtAnyPoint) {
  const std::string schema = R"({"type":"object","properties":{"a":{"type":"string"},"b":{"type":"string"}}})";
  {
    ScriptedPerson person;
    person.script = {says("1"), declines()};
    EXPECT_EQ(run_elicitation(form(schema), person.ask(), kNoBrowser).action, "decline");
  }
  {
    ScriptedPerson person;
    person.script = {cancels()};
    EXPECT_EQ(run_elicitation(form(schema), person.ask(), kNoBrowser).action, "cancel");
  }
  {
    ScriptedPerson person;
    person.script = {says("1"), says("2"), says("no")};
    const ElicitationResult result = run_elicitation(form(schema), person.ask(), kNoBrowser);
    EXPECT_EQ(result.action, "decline");
    EXPECT_TRUE(result.content.is_null());
  }
}

TEST(McpElicitationTest, ChoicesTakeAValueATitleOrANumber) {
  const std::string schema = R"({"type":"object","properties":{
    "size":{"type":"string","oneOf":[{"const":"s","title":"Small"},{"const":"l","title":"Large"}]},
    "legacy":{"type":"string","enum":["x1","x2"],"enumNames":["Option one","Option two"]},
    "toppings":{"type":"array","items":{"type":"string","enum":["cheese","ham","olives"]},
                "minItems":1,"maxItems":2}}})";
  ScriptedPerson person;
  person.script = {says("large"), says("2"), says("cheese, 3, cheese"), says("")};
  const ElicitationResult result = run_elicitation(form(schema), person.ask(), kNoBrowser);
  EXPECT_EQ(result.action, "accept");
  EXPECT_EQ(result.content, json(R"({"size":"l","legacy":"x2","toppings":["cheese","olives"]})"));
  EXPECT_NE(person.asked[0].detail.find("1) Small  2) Large"), std::string::npos);
  // Titles are what the review shows.
  EXPECT_NE(person.asked[3].detail.find("size: Large"), std::string::npos);
  EXPECT_NE(person.asked[3].detail.find("legacy: Option two"), std::string::npos);
}

TEST(McpElicitationTest, CoercionFollowsTheFieldType) {
  std::string error;
  std::vector<FormField> fields = form_fields(json(R"({"type":"object","properties":{
    "n":{"type":"number","minimum":0.5},
    "i":{"type":"integer"},
    "d":{"type":"string","format":"date"},
    "t":{"type":"string","format":"date-time"},
    "u":{"type":"string","format":"uri"},
    "many":{"type":"array","items":{"anyOf":[{"const":"a","title":"A"},{"const":"b","title":"B"}]},
            "maxItems":1}}})"), error);
  ASSERT_TRUE(error.empty()) << error;
  ASSERT_EQ(fields.size(), 6u);

  EXPECT_EQ(coerce_answer(fields[0], "2.5", error), JsonValue(2.5));
  EXPECT_EQ(coerce_answer(fields[0], "3", error), JsonValue(3));
  EXPECT_FALSE(coerce_answer(fields[0], "0.1", error));
  EXPECT_FALSE(coerce_answer(fields[0], "three", error));
  EXPECT_EQ(coerce_answer(fields[1], "-7", error), JsonValue(-7));
  EXPECT_FALSE(coerce_answer(fields[1], "1.5", error));
  EXPECT_TRUE(coerce_answer(fields[2], "2024-02-29", error));
  EXPECT_FALSE(coerce_answer(fields[2], "2023-02-29", error));
  EXPECT_FALSE(coerce_answer(fields[2], "2024-13-01", error));
  EXPECT_TRUE(coerce_answer(fields[3], "2026-07-28T14:05:00Z", error));
  EXPECT_TRUE(coerce_answer(fields[3], "2026-07-28T14:05:00.25+02:00", error));
  EXPECT_FALSE(coerce_answer(fields[3], "2026-07-28 14:05", error));
  EXPECT_TRUE(coerce_answer(fields[4], "https://example.com/x", error));
  EXPECT_FALSE(coerce_answer(fields[4], "not a uri", error));
  EXPECT_EQ(coerce_answer(fields[5], "B", error), json(R"(["b"])"));
  EXPECT_FALSE(coerce_answer(fields[5], "a, b", error));  // max 1
  EXPECT_FALSE(coerce_answer(fields[5], "c", error));
}

TEST(McpElicitationTest, AFormThatIsNotFlatIsDeclinedWithWhy) {
  ScriptedPerson person;
  person.script = {says("")};
  const ElicitationResult result = run_elicitation(
      form(R"({"type":"object","properties":{"address":{"type":"object","properties":{}}}})"),
      person.ask(), kNoBrowser);
  EXPECT_EQ(result.action, "decline");
  ASSERT_EQ(person.asked.size(), 1u);
  EXPECT_NE(person.asked[0].problem.find("address"), std::string::npos);
}

// ──────────────────────────────── links ─────────────────────────────────────

ElicitationRequest link(const std::string& url) {
  ElicitationRequest request;
  request.server = "bank";
  request.mode = "url";
  request.message = "Connect your account";
  request.url = url;
  return request;
}

TEST(McpElicitationTest, ALinkIsOpenedOnlyOnAYesAndShownWholeFirst) {
  std::vector<std::string> opened_urls;
  const OpenUrlFunction browser = [&](const std::string& url, std::string&) {
    opened_urls.push_back(url);
    return true;
  };
  {
    ScriptedPerson person;
    person.script = {says("")};  // Enter declines
    EXPECT_EQ(run_elicitation(link("https://bank.example/connect?x=1"), person.ask(), browser).action,
              "decline");
    EXPECT_TRUE(opened_urls.empty());
    ASSERT_EQ(person.asked.size(), 1u);
    EXPECT_NE(person.asked[0].detail.find("link: https://bank.example/connect?x=1"), std::string::npos);
    EXPECT_NE(person.asked[0].detail.find("site: bank.example"), std::string::npos);
    EXPECT_EQ(person.asked[0].emphasis, "bank.example");
    EXPECT_EQ(person.asked[0].heading, "bank asks you to open a link");
  }
  ScriptedPerson person;
  person.script = {says("y")};
  std::set<std::string> opened;
  EXPECT_EQ(run_elicitation(link("https://bank.example/connect"), person.ask(), browser, &opened).action,
            "accept");
  EXPECT_EQ(opened_urls, std::vector<std::string>{"https://bank.example/connect"});

  // Asked again about the same link: the server is waiting for them to finish.
  ScriptedPerson again;
  again.script = {says("")};
  EXPECT_EQ(run_elicitation(link("https://bank.example/connect"), again.ask(), browser, &opened).action,
            "accept");
  EXPECT_EQ(opened_urls.size(), 1u);
  EXPECT_NE(again.asked[0].problem.find("already"), std::string::npos);
}

TEST(McpElicitationTest, SuspiciousLinksCarryWarnings) {
  ScriptedPerson person;
  person.script = {says("n")};
  run_elicitation(link("http://xn--bnk-sna.example@evil.example/"), person.ask(), kNoBrowser);
  const std::string& detail = person.asked[0].detail;
  EXPECT_NE(detail.find("not https"), std::string::npos) << detail;
  EXPECT_NE(detail.find("before '@'"), std::string::npos) << detail;
  EXPECT_NE(detail.find("site: evil.example"), std::string::npos) << detail;

  ScriptedPerson puny;
  puny.script = {says("n")};
  run_elicitation(link("https://xn--pple-43d.com/"), puny.ask(), kNoBrowser);
  EXPECT_NE(puny.asked[0].detail.find("punycode"), std::string::npos);
}

TEST(McpElicitationTest, ALinkThatIsNotHttpIsNeverOpened) {
  bool opened = false;
  const OpenUrlFunction browser = [&](const std::string&, std::string&) { return opened = true; };
  ScriptedPerson person;
  person.script = {says("y")};
  EXPECT_EQ(run_elicitation(link("javascript:alert(1)"), person.ask(), browser).action, "decline");
  EXPECT_FALSE(opened);
  EXPECT_FALSE(person.asked[0].problem.empty());
}

TEST(McpElicitationTest, WithoutABrowserThePersonOpensTheLinkThemselves) {
  ScriptedPerson person;
  person.script = {says("y"), says("")};
  EXPECT_EQ(run_elicitation(link("https://bank.example/c"), person.ask(), kNoBrowser).action, "accept");
  ASSERT_EQ(person.asked.size(), 2u);
  EXPECT_NE(person.asked[1].problem.find("no display"), std::string::npos) << person.asked[1].problem;
  EXPECT_NE(person.asked[1].problem.find("yourself"), std::string::npos);
}

}  // namespace
}  // namespace mcp
