#include <core/json_util.hpp>

namespace agent {

namespace {

// simdjson's builder escapes strings but has no standalone "escape into a
// std::string" entry point, so a one-element builder does the work.
std::string quoted(std::string_view value) {
  simdjson::builder::string_builder sb;
  sb.escape_and_append_with_quotes(value);
  std::string_view out;
  if (sb.view().get(out)) return "\"\"";
  return std::string(out);
}

}  // namespace

RawJson RawJson::of_string(std::string_view value) {
  return RawJson{quoted(value)};
}

RawJson RawJson::of_bool(bool value) {
  return RawJson{value ? "true" : "false"};
}

RawJson RawJson::of_int(int64_t value) {
  return RawJson{std::to_string(value)};
}

RawJson RawJson::of_double(double value) {
  simdjson::builder::string_builder sb;
  sb.append(value);
  std::string_view out;
  if (sb.view().get(out)) return RawJson{"0"};
  return RawJson{std::string(out)};
}

RawJson RawJson::of_raw(std::string json) { return RawJson{std::move(json)}; }

JsonWriter::JsonWriter() = default;

void JsonWriter::separate() {
  // The value of a `"key":` pair is not a new element of the enclosing
  // object — the key already claimed that slot.
  if (expect_value_) {
    expect_value_ = false;
    return;
  }
  if (has_element_.empty()) return;
  if (has_element_.back()) {
    builder_.append_comma();
  } else {
    has_element_.back() = true;
  }
}

JsonWriter& JsonWriter::begin_object() {
  separate();
  builder_.start_object();
  has_element_.push_back(false);
  return *this;
}

JsonWriter& JsonWriter::end_object() {
  builder_.end_object();
  if (not has_element_.empty()) has_element_.pop_back();
  return *this;
}

JsonWriter& JsonWriter::begin_array() {
  separate();
  builder_.start_array();
  has_element_.push_back(false);
  return *this;
}

JsonWriter& JsonWriter::end_array() {
  builder_.end_array();
  if (not has_element_.empty()) has_element_.pop_back();
  return *this;
}

JsonWriter& JsonWriter::key(std::string_view name) {
  separate();
  builder_.escape_and_append_with_quotes(name);
  builder_.append_colon();
  expect_value_ = true;
  return *this;
}

JsonWriter& JsonWriter::value(std::string_view v) {
  separate();
  builder_.escape_and_append_with_quotes(v);
  return *this;
}

JsonWriter& JsonWriter::value(const char* v) {
  return value(std::string_view(v));
}

JsonWriter& JsonWriter::value(bool v) {
  separate();
  builder_.append_raw(v ? "true" : "false");
  return *this;
}

JsonWriter& JsonWriter::value(int64_t v) {
  separate();
  builder_.append(v);
  return *this;
}

JsonWriter& JsonWriter::value(double v) {
  separate();
  builder_.append(v);
  return *this;
}

JsonWriter& JsonWriter::value(const std::vector<std::string>& v) {
  separate();
  builder_.start_array();
  bool first = true;
  for (const auto& item : v) {
    if (not first) builder_.append_comma();
    first = false;
    builder_.escape_and_append_with_quotes(item);
  }
  builder_.end_array();
  return *this;
}

JsonWriter& JsonWriter::value(const RawJson& v) {
  if (v.empty()) return null_value();
  separate();
  builder_.append_raw(std::string_view(v.text));
  return *this;
}

JsonWriter& JsonWriter::null_value() {
  separate();
  builder_.append_null();
  return *this;
}

std::string JsonWriter::str() {
  std::string_view out;
  if (builder_.view().get(out)) return std::string();
  return std::string(out);
}

// ---------------------------------------------------------------------------

std::string string_field(simdjson::ondemand::object& obj, std::string_view key,
                         std::string fallback) {
  std::string_view out;
  if (obj[key].get_string().get(out)) return fallback;
  return std::string(out);
}

int64_t int_field(simdjson::ondemand::object& obj, std::string_view key,
                  int64_t fallback) {
  int64_t out = 0;
  if (obj[key].get_int64().get(out)) return fallback;
  return out;
}

std::vector<std::string> string_array_field(simdjson::ondemand::object& obj,
                                            std::string_view key) {
  std::vector<std::string> values;

  simdjson::ondemand::array array;
  if (obj[key].get_array().get(array)) return values;

  for (auto element : array) {
    std::string_view item;
    if (element.get_string().get(item)) continue;
    values.emplace_back(item);
  }
  return values;
}

std::string raw_field(simdjson::ondemand::object& obj, std::string_view key,
                      std::string fallback) {
  simdjson::ondemand::value value;
  if (obj[key].get(value)) return fallback;

  std::string_view raw;
  if (simdjson::to_json_string(value).get(raw)) return fallback;
  return std::string(raw);
}

}  // namespace agent
