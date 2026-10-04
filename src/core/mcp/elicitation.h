#ifndef M8_MCP_ELICITATION_H
#define M8_MCP_ELICITATION_H

#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <core/mcp/protocol.h>
#include <core/util/json_value.h>

namespace mcp {

// ---------------------------------------------------------------------------
// Elicitation: a server asking the person at the keyboard for something in
// the middle of a call. This turns one request into a run of plain questions
// — one per form field and then a review of the answers, or a consent to open
// a link — so any UI that can ask one question at a time can host it. The
// spec's client duties live here: say which server asks, offer decline and
// cancel, let the answers be reviewed and changed before they are sent, and
// for a link show it whole with its host picked out, never fetch it, and open
// it only on a yes.
// ---------------------------------------------------------------------------

struct Question {
  std::string heading;   // which server asks, from which config, for which agent
  std::string message;   // the server's own words
  std::string detail;    // the field or the link; may run to several lines
  std::string emphasis;  // a part of `detail` to highlight (a link's host)
  std::string problem;   // why the last answer was not taken
  std::string progress;  // "field 2 of 3"
  std::string initial;   // what the answer box starts with
  std::string keys;      // how to answer
};

struct Response {
  enum struct Kind : uint8_t { Answered, Declined, Cancelled };
  Kind kind = Kind::Cancelled;
  std::string text;
};

using AskFunction = std::function<Response(const Question&)>;
using OpenUrlFunction = std::function<bool(const std::string& url, std::string& error)>;

// One form field as the requested schema describes it: the spec allows a flat
// object of strings (with length limits and a format), numbers, integers,
// booleans, and single- or multi-select enums.
struct FormField {
  std::string name;
  std::string title;
  std::string description;
  std::string type;  // "string" | "number" | "integer" | "boolean" | "array"
  bool required = false;
  std::string format;  // "email" | "uri" | "date" | "date-time" | ""
  std::optional<int64_t> min_length;
  std::optional<int64_t> max_length;
  std::optional<double> minimum;
  std::optional<double> maximum;
  // An enum's (value, title); the title is the value when there is none.
  std::vector<std::pair<std::string, std::string>> choices;
  std::optional<int64_t> min_items;  // multi-select
  std::optional<int64_t> max_items;
  util::JsonValue default_value;
};

// The fields of a requested schema, in the server's order. `error` is set for
// a schema that is not a flat object of primitives.
std::vector<FormField> form_fields(const util::JsonValue& schema, std::string& error);

// What a person typed for a field, checked and converted to the JSON the
// server asked for: numbers, yes/no, a choice by value, title or number, a
// comma-separated list for a multi-select. nullopt with why in `error`.
std::optional<util::JsonValue> coerce_answer(const FormField& field, std::string_view text,
                                             std::string& error);

// A value as it would be typed back in: a default or an answer to edit.
std::string answer_text(const FormField& field, const util::JsonValue& value);

// The field's line in a question: its name, what it takes, and its limits.
std::string describe_field(const FormField& field);

// Runs one elicitation to its end, asking through `ask`. `opened` remembers
// the links the person agreed to open, so a server asking about the same link
// again (to wait until they are done there) is not mistaken for a new one.
ElicitationResult run_elicitation(const ElicitationRequest& request, const AskFunction& ask,
                                  const OpenUrlFunction& open_url,
                                  std::set<std::string>* opened = nullptr);

}  // namespace mcp

#endif  // M8_MCP_ELICITATION_H
