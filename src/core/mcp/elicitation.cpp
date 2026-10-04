#include <core/mcp/elicitation.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <locale>
#include <sstream>

#include <core/util/open_url.h>
#include <core/util/url.h>

namespace mcp {

namespace {

using util::JsonValue;

constexpr const char* kFieldKeys = "Enter answers \xc2\xb7 Ctrl+D declines \xc2\xb7 Esc cancels";

std::string lower(std::string_view text) {
  std::string out(text);
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

std::string trim(std::string_view text) {
  while (not text.empty() and std::isspace(static_cast<unsigned char>(text.front()))) {
    text.remove_prefix(1);
  }
  while (not text.empty() and std::isspace(static_cast<unsigned char>(text.back()))) {
    text.remove_suffix(1);
  }
  return std::string(text);
}

// JSON Schema counts string length in characters, not bytes.
int64_t code_points(std::string_view text) {
  int64_t count = 0;
  for (const char c : text) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++count;
  }
  return count;
}

bool has_space(std::string_view text) {
  return std::any_of(text.begin(), text.end(),
                     [](unsigned char c) { return std::isspace(c) != 0; });
}

std::string number_text(double value) {
  if (std::floor(value) == value and std::fabs(value) < 1e15) {
    return std::to_string(static_cast<int64_t>(value));
  }
  std::ostringstream out;
  out.imbue(std::locale::classic());
  out << value;
  return out.str();
}

// Not strtod: a locale with a decimal comma would read "3.5" as 3.
std::optional<double> parse_number(std::string_view text) {
  std::istringstream in{std::string(text)};
  in.imbue(std::locale::classic());
  double value = 0;
  in >> value;
  if (in.fail() or not in.eof() or not std::isfinite(value)) return std::nullopt;
  return value;
}

bool digits(std::string_view text, size_t from, size_t count) {
  if (from + count > text.size()) return false;
  return std::all_of(text.begin() + static_cast<std::ptrdiff_t>(from),
                     text.begin() + static_cast<std::ptrdiff_t>(from + count),
                     [](unsigned char c) { return std::isdigit(c) != 0; });
}

int number_at(std::string_view text, size_t from, size_t count) {
  int value = 0;
  std::from_chars(text.data() + from, text.data() + from + count, value);
  return value;
}

bool valid_date(std::string_view text) {
  if (text.size() != 10 or text[4] != '-' or text[7] != '-') return false;
  if (not digits(text, 0, 4) or not digits(text, 5, 2) or not digits(text, 8, 2)) return false;
  const int year = number_at(text, 0, 4);
  const int month = number_at(text, 5, 2);
  const int day = number_at(text, 8, 2);
  static constexpr int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 or month > 12 or day < 1) return false;
  const bool leap = (year % 4 == 0 and year % 100 != 0) or year % 400 == 0;
  return day <= kDays[month - 1] + (month == 2 and leap ? 1 : 0);
}

// RFC 3339: 2026-07-28T14:05:00Z, with optional fractional seconds and an
// offset in place of the Z.
bool valid_date_time(std::string_view text) {
  if (text.size() < 20 or not valid_date(text.substr(0, 10))) return false;
  if (text[10] != 'T' and text[10] != 't') return false;
  if (not digits(text, 11, 2) or text[13] != ':' or not digits(text, 14, 2) or
      text[16] != ':' or not digits(text, 17, 2)) {
    return false;
  }
  if (number_at(text, 11, 2) > 23 or number_at(text, 14, 2) > 59 or
      number_at(text, 17, 2) > 60) {
    return false;
  }
  size_t at = 19;
  if (at < text.size() and text[at] == '.') {
    const size_t start = ++at;
    while (at < text.size() and std::isdigit(static_cast<unsigned char>(text[at]))) ++at;
    if (at == start) return false;
  }
  const std::string_view zone = text.substr(at);
  if (zone == "Z" or zone == "z") return true;
  return zone.size() == 6 and (zone[0] == '+' or zone[0] == '-') and digits(zone, 1, 2) and
         zone[3] == ':' and digits(zone, 4, 2) and number_at(zone, 1, 2) <= 23 and
         number_at(zone, 4, 2) <= 59;
}

bool valid_email(std::string_view text) {
  if (has_space(text)) return false;
  const size_t at = text.find('@');
  if (at == std::string_view::npos or at == 0 or text.find('@', at + 1) != std::string_view::npos) {
    return false;
  }
  const std::string_view domain = text.substr(at + 1);
  return domain.size() >= 3 and domain.find('.') != std::string_view::npos and
         domain.front() != '.' and domain.back() != '.';
}

bool valid_uri(std::string_view text) {
  if (has_space(text) or text.empty() or not std::isalpha(static_cast<unsigned char>(text[0]))) {
    return false;
  }
  const size_t colon = text.find(':');
  if (colon == std::string_view::npos or colon + 1 >= text.size()) return false;
  return std::all_of(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(colon),
                     [](unsigned char c) {
                       return std::isalnum(c) or c == '+' or c == '-' or c == '.';
                     });
}

std::string label(const FormField& field) {
  return field.title.empty() ? field.name : field.title;
}

std::string choice_list(const FormField& field) {
  std::string out;
  for (size_t i = 0; i < field.choices.size(); ++i) {
    if (i > 0) out += "  ";
    out += std::to_string(i + 1) + ") " + field.choices[i].second;
  }
  return out;
}

std::optional<std::string> match_choice(const FormField& field, std::string_view text) {
  for (const auto& [value, title] : field.choices) {
    if (text == value) return value;
  }
  for (const auto& [value, title] : field.choices) {
    if (text == title) return value;
  }
  const std::string wanted = lower(text);
  for (const auto& [value, title] : field.choices) {
    if (wanted == lower(value) or wanted == lower(title)) return value;
  }
  if (digits(text, 0, text.size()) and not text.empty() and text.size() < 6) {
    const int index = number_at(text, 0, text.size());
    if (index >= 1 and static_cast<size_t>(index) <= field.choices.size()) {
      return field.choices[static_cast<size_t>(index) - 1].first;
    }
  }
  return std::nullopt;
}

std::string range_text(const std::optional<double>& low, const std::optional<double>& high) {
  if (low and high) return "from " + number_text(*low) + " to " + number_text(*high);
  if (low) return "at least " + number_text(*low);
  if (high) return "at most " + number_text(*high);
  return std::string();
}

std::vector<std::pair<std::string, std::string>> parse_choices(const JsonValue& schema) {
  std::vector<std::pair<std::string, std::string>> choices;
  const JsonValue& names = schema.get("enumNames");  // the pre-2025-11-25 titles
  const std::vector<JsonValue>& values = schema.get("enum").items();
  for (size_t i = 0; i < values.size(); ++i) {
    if (not values[i].is_string()) continue;
    const std::string title = i < names.size() ? names.items()[i].as_string() : std::string();
    choices.emplace_back(values[i].as_string(), title.empty() ? values[i].as_string() : title);
  }
  for (const char* key : {"oneOf", "anyOf"}) {
    for (const JsonValue& option : schema.get(key).items()) {
      if (not option.get("const").is_string()) continue;
      const std::string value = option.get("const").as_string();
      const std::string title = option.get("title").as_string();
      choices.emplace_back(value, title.empty() ? value : title);
    }
  }
  return choices;
}

ElicitationResult outcome(Response::Kind kind) {
  ElicitationResult result;
  result.action = kind == Response::Kind::Declined ? "decline" : "cancel";
  return result;
}

std::string heading_for(const ElicitationRequest& request) {
  std::string heading = request.server.empty() ? "an MCP server" : request.server;
  if (not request.scope.empty()) heading += " (" + request.scope + ")";
  heading += request.mode == "url" ? " asks you to open a link" : " asks";
  if (not request.agent_label.empty()) heading += ", for " + request.agent_label;
  return heading;
}

ElicitationResult run_form(const ElicitationRequest& request, const AskFunction& ask) {
  const std::string heading = heading_for(request);
  std::string error;
  const std::vector<FormField> fields = form_fields(request.requested_schema, error);
  if (not error.empty()) {
    Question question;
    question.heading = heading;
    question.message = request.message;
    question.problem = "m8 cannot show this form (" + error + "), so it is declined";
    question.keys = "Enter or Esc to dismiss";
    const Response response = ask(question);
    return outcome(response.kind == Response::Kind::Cancelled ? Response::Kind::Cancelled
                                                             : Response::Kind::Declined);
  }

  // The spec asks clients to start each field at its default.
  std::vector<JsonValue> values;
  for (const FormField& field : fields) values.push_back(field.default_value);

  while (true) {
    for (size_t i = 0; i < fields.size(); ++i) {
      const FormField& field = fields[i];
      Question question;
      question.heading = heading;
      question.message = request.message;
      question.detail = describe_field(field);
      if (fields.size() > 1) {
        question.progress =
            "field " + std::to_string(i + 1) + " of " + std::to_string(fields.size());
      }
      question.initial = values[i].is_null() ? std::string() : answer_text(field, values[i]);
      question.keys = kFieldKeys;
      while (true) {
        const Response response = ask(question);
        if (response.kind != Response::Kind::Answered) return outcome(response.kind);
        const std::string text = trim(response.text);
        if (text.empty()) {
          if (field.required) {
            question.problem = "this one is required";
            question.initial.clear();
            continue;
          }
          values[i] = JsonValue();  // left out
          break;
        }
        std::string why;
        std::optional<JsonValue> value = coerce_answer(field, text, why);
        if (not value) {
          question.problem = why;
          question.initial = response.text;
          continue;
        }
        values[i] = std::move(*value);
        break;
      }
    }

    // Shown whole before anything is sent: the person may change it.
    Question review;
    review.heading = heading;
    review.message = request.message;
    review.progress = "review";
    for (size_t i = 0; i < fields.size(); ++i) {
      if (not review.detail.empty()) review.detail += "\n";
      review.detail += label(fields[i]) + ": " +
                       (values[i].is_null() ? std::string("(left out)")
                                            : answer_text(fields[i], values[i]));
    }
    if (fields.empty()) review.detail = "(nothing to fill in)";
    review.keys = "Enter sends it \xc2\xb7 e changes it \xc2\xb7 Ctrl+D declines \xc2\xb7 Esc cancels";
    while (true) {
      const Response response = ask(review);
      if (response.kind != Response::Kind::Answered) return outcome(response.kind);
      const std::string word = lower(trim(response.text));
      if (word.empty() or word == "y" or word == "yes" or word == "send" or word == "s") {
        ElicitationResult result;
        result.action = "accept";
        result.content = JsonValue::object();
        for (size_t i = 0; i < fields.size(); ++i) {
          if (not values[i].is_null()) result.content.set(fields[i].name, values[i]);
        }
        return result;
      }
      if (word == "e" or word == "edit" or word == "change" or word == "c") break;
      if (word == "n" or word == "no" or word == "d" or word == "decline") {
        return outcome(Response::Kind::Declined);
      }
      review.problem = "Enter sends it; e changes it";
    }
  }
}

ElicitationResult run_url(const ElicitationRequest& request, const AskFunction& ask,
                          const OpenUrlFunction& open_url, std::set<std::string>* opened) {
  const std::string& url = request.url;
  Question question;
  question.heading = heading_for(request);
  question.message = request.message;

  const std::string problem = util::open_url_problem(url);
  if (not problem.empty()) {
    question.detail = "link: " + url;
    question.problem = problem + "; it was not opened";
    question.keys = "Enter or Esc to dismiss";
    const Response response = ask(question);
    return outcome(response.kind == Response::Kind::Cancelled ? Response::Kind::Cancelled
                                                             : Response::Kind::Declined);
  }

  // The whole link, then the site it really goes to, on its own line: the
  // part a look-alike link hopes nobody reads.
  const util::Url parsed = *util::parse_url(url);
  question.detail = "link: " + url + "\nsite: " + parsed.host;
  question.emphasis = parsed.host;
  if (parsed.scheme != "https") {
    question.detail += "\n\xe2\x9a\xa0 not https: what you send there can be read on the way";
  }
  if (parsed.host.find("xn--") != std::string::npos) {
    question.detail +=
        "\n\xe2\x9a\xa0 the site's name is punycode (xn--): it may imitate a name you know";
  }
  if (parsed.has_userinfo) {
    question.detail += "\n\xe2\x9a\xa0 the link puts a name before '@': the site is what follows it";
  }

  const bool again = opened != nullptr and opened->count(url) > 0;
  if (again) {
    // The server asks about the same link until the person is done there.
    question.problem = "You opened this link already; the server is waiting for you to finish there.";
    question.keys = "Enter once you are done \xc2\xb7 Ctrl+D declines \xc2\xb7 Esc cancels";
    const Response response = ask(question);
    if (response.kind != Response::Kind::Answered) return outcome(response.kind);
    ElicitationResult result;
    result.action = "accept";
    return result;
  }

  question.keys = "y opens it in your browser \xc2\xb7 Enter or n declines \xc2\xb7 Esc cancels";
  while (true) {
    const Response response = ask(question);
    if (response.kind != Response::Kind::Answered) return outcome(response.kind);
    const std::string word = lower(trim(response.text));
    if (word.empty() or word == "n" or word == "no") return outcome(Response::Kind::Declined);
    if (word != "y" and word != "yes" and word != "o" and word != "open") {
      question.problem = "y opens it; Enter declines";
      continue;
    }
    std::string error;
    if (not open_url or not open_url(url, error)) {
      // No browser here (an SSH session, say): the person can still go there.
      Question manual = question;
      manual.problem = "Could not open a browser" + (error.empty() ? std::string() : " (" + error + ")") +
                       ". Open the link yourself.";
      manual.keys = "Enter once you have opened it \xc2\xb7 Ctrl+D declines \xc2\xb7 Esc cancels";
      const Response manual_response = ask(manual);
      if (manual_response.kind != Response::Kind::Answered) return outcome(manual_response.kind);
    }
    if (opened != nullptr) opened->insert(url);
    ElicitationResult result;
    result.action = "accept";
    return result;
  }
}

}  // namespace

std::vector<FormField> form_fields(const JsonValue& schema, std::string& error) {
  std::vector<FormField> fields;
  if (not schema.is_object()) {
    error = "the requested schema is not an object";
    return fields;
  }
  const std::string type = schema.get("type").string_or("object");
  if (type != "object") {
    error = "the requested schema is a " + type + ", not an object";
    return fields;
  }
  std::set<std::string> required;
  for (const JsonValue& name : schema.get("required").items()) {
    if (name.is_string()) required.insert(name.as_string());
  }
  for (const JsonValue::Member& member : schema.get("properties").members()) {
    const JsonValue& property = member.value;
    if (not property.is_object()) {
      error = "field '" + member.key + "' is not described";
      return {};
    }
    FormField field;
    field.name = member.key;
    field.title = property.get("title").as_string();
    field.description = property.get("description").as_string();
    field.required = required.count(member.key) > 0;
    field.default_value = property.get("default");
    const JsonValue& declared = property.get("type");
    if (declared.is_string()) {
      field.type = declared.as_string();
    } else {
      for (const JsonValue& option : declared.items()) {
        if (option.is_string() and option.as_string() != "null") {
          field.type = option.as_string();
          break;
        }
      }
    }
    if (field.type.empty() and (property.contains("enum") or property.contains("oneOf"))) {
      field.type = "string";
    }

    if (field.type == "string") {
      field.format = property.get("format").as_string();
      if (property.get("minLength").is_number()) field.min_length = property.get("minLength").as_int();
      if (property.get("maxLength").is_number()) field.max_length = property.get("maxLength").as_int();
      field.choices = parse_choices(property);
    } else if (field.type == "number" or field.type == "integer") {
      if (property.get("minimum").is_number()) field.minimum = property.get("minimum").as_double();
      if (property.get("maximum").is_number()) field.maximum = property.get("maximum").as_double();
    } else if (field.type == "boolean") {
      // nothing more
    } else if (field.type == "array") {
      field.choices = parse_choices(property.get("items"));
      if (field.choices.empty()) {
        error = "field '" + member.key + "' is a list without choices";
        return {};
      }
      if (property.get("minItems").is_number()) field.min_items = property.get("minItems").as_int();
      if (property.get("maxItems").is_number()) field.max_items = property.get("maxItems").as_int();
    } else {
      error = "field '" + member.key + "' is " +
              (field.type.empty() ? std::string("of no type") : "a " + field.type) +
              ", which forms do not hold";
      return {};
    }
    fields.push_back(std::move(field));
  }
  return fields;
}

std::optional<JsonValue> coerce_answer(const FormField& field, std::string_view raw,
                                       std::string& error) {
  const std::string text = trim(raw);

  if (field.type == "boolean") {
    const std::string word = lower(text);
    if (word == "y" or word == "yes" or word == "true" or word == "1" or word == "on") return JsonValue(true);
    if (word == "n" or word == "no" or word == "false" or word == "0" or word == "off") return JsonValue(false);
    error = "answer yes or no";
    return std::nullopt;
  }

  if (field.type == "integer" or field.type == "number") {
    const std::optional<double> number = parse_number(text);
    const bool whole = number and std::floor(*number) == *number;
    if (not number or (field.type == "integer" and not whole)) {
      error = field.type == "integer" ? "a whole number, like 42" : "a number, like 3.5";
      return std::nullopt;
    }
    if ((field.minimum and *number < *field.minimum) or (field.maximum and *number > *field.maximum)) {
      error = "it must be " + range_text(field.minimum, field.maximum);
      return std::nullopt;
    }
    if (whole and std::fabs(*number) < 9007199254740992.0) {
      return JsonValue(static_cast<int64_t>(*number));
    }
    return JsonValue(*number);
  }

  if (field.type == "array") {
    JsonValue values = JsonValue::array();
    std::set<std::string> seen;
    size_t at = 0;
    while (at <= text.size()) {
      const size_t comma = std::min(text.find(',', at), text.size());
      const std::string item = trim(std::string_view(text).substr(at, comma - at));
      at = comma + 1;
      if (item.empty()) continue;
      const std::optional<std::string> value = match_choice(field, item);
      if (not value) {
        error = "'" + item + "' is not one of: " + choice_list(field);
        return std::nullopt;
      }
      if (seen.insert(*value).second) values.push_back(*value);
    }
    const auto count = static_cast<int64_t>(values.size());
    if ((field.min_items and count < *field.min_items) or (field.max_items and count > *field.max_items)) {
      error = "pick " + range_text(field.min_items ? std::optional<double>(*field.min_items) : std::nullopt,
                                   field.max_items ? std::optional<double>(*field.max_items) : std::nullopt);
      return std::nullopt;
    }
    return values;
  }

  // A string: a choice, or text with limits and maybe a format.
  if (not field.choices.empty()) {
    if (const std::optional<std::string> value = match_choice(field, text)) return JsonValue(*value);
    error = "pick one of: " + choice_list(field);
    return std::nullopt;
  }
  const int64_t length = code_points(text);
  if ((field.min_length and length < *field.min_length) or
      (field.max_length and length > *field.max_length)) {
    error = "it must be " +
            range_text(field.min_length ? std::optional<double>(*field.min_length) : std::nullopt,
                       field.max_length ? std::optional<double>(*field.max_length) : std::nullopt) +
            " characters long";
    return std::nullopt;
  }
  if (field.format == "email" and not valid_email(text)) {
    error = "that is not an email address";
    return std::nullopt;
  }
  if (field.format == "uri" and not valid_uri(text)) {
    error = "that is not a link (like https://example.com)";
    return std::nullopt;
  }
  if (field.format == "date" and not valid_date(text)) {
    error = "a date, written YYYY-MM-DD";
    return std::nullopt;
  }
  if (field.format == "date-time" and not valid_date_time(text)) {
    error = "a date and time, written YYYY-MM-DDTHH:MM:SSZ";
    return std::nullopt;
  }
  return JsonValue(text);
}

std::string answer_text(const FormField& field, const JsonValue& value) {
  const auto title_of = [&](const std::string& choice) {
    for (const auto& [option, title] : field.choices) {
      if (option == choice) return title;
    }
    return choice;
  };
  if (value.is_bool()) return value.as_bool() ? "yes" : "no";
  if (value.is_number()) return number_text(value.as_double());
  if (value.is_array()) {
    std::string out;
    for (const JsonValue& item : value.items()) {
      if (not out.empty()) out += ", ";
      out += title_of(item.as_string());
    }
    return out;
  }
  if (value.is_string()) return title_of(value.as_string());
  return std::string();
}

std::string describe_field(const FormField& field) {
  std::string out = label(field) + (field.required ? " (required)" : " (optional)");
  if (not field.description.empty() and field.description != field.title) {
    out += "\n" + field.description;
  }
  std::string kind;
  if (field.type == "boolean") {
    kind = "yes or no";
  } else if (field.type == "number" or field.type == "integer") {
    kind = field.type == "integer" ? "a whole number" : "a number";
    const std::string range = range_text(field.minimum, field.maximum);
    if (not range.empty()) kind += ", " + range;
  } else if (field.type == "array") {
    kind = "any of: " + choice_list(field) + " \xe2\x80\x94 numbers or names, with commas between";
    const std::string range =
        range_text(field.min_items ? std::optional<double>(*field.min_items) : std::nullopt,
                   field.max_items ? std::optional<double>(*field.max_items) : std::nullopt);
    if (not range.empty()) kind += "; pick " + range;
  } else if (not field.choices.empty()) {
    kind = "one of: " + choice_list(field) + " \xe2\x80\x94 a number or a name";
  } else {
    kind = "text";
    if (field.format == "email") kind = "an email address";
    if (field.format == "uri") kind = "a link";
    if (field.format == "date") kind = "a date, YYYY-MM-DD";
    if (field.format == "date-time") kind = "a date and time, YYYY-MM-DDTHH:MM:SSZ";
    const std::string range =
        range_text(field.min_length ? std::optional<double>(*field.min_length) : std::nullopt,
                   field.max_length ? std::optional<double>(*field.max_length) : std::nullopt);
    if (not range.empty()) kind += ", " + range + " characters";
  }
  return out + "\n" + kind;
}

ElicitationResult run_elicitation(const ElicitationRequest& request, const AskFunction& ask,
                                  const OpenUrlFunction& open_url, std::set<std::string>* opened) {
  if (request.mode == "url") return run_url(request, ask, open_url, opened);
  if (request.mode != "form" and not request.mode.empty()) {
    ElicitationResult result;
    result.action = "decline";  // a mode m8 never declared
    return result;
  }
  return run_form(request, ask);
}

}  // namespace mcp
