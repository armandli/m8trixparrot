#include <core/util/json_value.h>

#include <cmath>
#include <utility>

#include <simdjson.h>

#include <core/util/text.h>

namespace util {

namespace {

const JsonValue& shared_null() {
  static const JsonValue null;
  return null;
}

const std::vector<JsonValue>& empty_items() {
  static const std::vector<JsonValue> empty;
  return empty;
}

const std::vector<JsonValue::Member>& empty_members() {
  static const std::vector<JsonValue::Member> empty;
  return empty;
}

JsonValue from_element(simdjson::dom::element element) {
  switch (element.type()) {
    case simdjson::dom::element_type::ARRAY: {
      JsonValue out = JsonValue::array();
      // Named local avoids UB: .value_unsafe() on a temporary simdjson_result
      // returns a dangling reference on GCC/Linux (Apple Clang tolerates it).
      simdjson::dom::array arr;
      if (!element.get_array().get(arr)) {
        for (simdjson::dom::element child : arr) {
          out.push_back(from_element(child));
        }
      }
      return out;
    }
    case simdjson::dom::element_type::OBJECT: {
      JsonValue out = JsonValue::object();
      std::vector<JsonValue::Member>& members = out.members();
      simdjson::dom::object obj;
      if (element.get_object().get(obj)) return out;
      for (simdjson::dom::key_value_pair field : obj) {
        // Appended rather than set(): set() is a linear search, which would make
        // a large object quadratic. A repeated key (which JSON only discourages)
        // is kept twice, and find() answers with the first.
        members.push_back(JsonValue::Member{std::string(field.key),
                                            from_element(field.value)});
      }
      return out;
    }
    case simdjson::dom::element_type::INT64:
      return JsonValue(element.get_int64().value_unsafe());
    case simdjson::dom::element_type::UINT64:
      return JsonValue(element.get_uint64().value_unsafe());
    case simdjson::dom::element_type::DOUBLE:
      return JsonValue(element.get_double().value_unsafe());
    case simdjson::dom::element_type::STRING:
      return JsonValue(std::string_view(element.get_string().value_unsafe()));
    case simdjson::dom::element_type::BOOL:
      return JsonValue(element.get_bool().value_unsafe());
    case simdjson::dom::element_type::NULL_VALUE:
    default:
      return JsonValue();
  }
}

// Strings from a parser are already valid; strings built from process output
// may not be, and JsonWriter turns one bad byte into an empty document.
std::string_view safe(std::string_view text, std::string& scratch) {
  if (is_valid_utf8(text)) return text;
  scratch = sanitize_utf8(text);
  return scratch;
}

}  // namespace

JsonValue::JsonValue() = default;
JsonValue::JsonValue(std::nullptr_t) {}
JsonValue::JsonValue(bool value) : mType(Type::Bool), mBool(value) {}
JsonValue::JsonValue(double value) : mType(Type::Double), mDouble(value) {}
JsonValue::JsonValue(const char* value)
    : mType(Type::String), mString(value != nullptr ? value : "") {}
JsonValue::JsonValue(std::string value)
    : mType(Type::String), mString(std::move(value)) {}
JsonValue::JsonValue(std::string_view value)
    : mType(Type::String), mString(value) {}

JsonValue::JsonValue(const JsonValue& other) = default;
JsonValue::JsonValue(JsonValue&& other) noexcept = default;
JsonValue& JsonValue::operator=(const JsonValue& other) = default;
JsonValue& JsonValue::operator=(JsonValue&& other) noexcept = default;
JsonValue::~JsonValue() = default;

JsonValue JsonValue::array() {
  JsonValue out;
  out.mType = Type::Array;
  return out;
}

JsonValue JsonValue::object() {
  JsonValue out;
  out.mType = Type::Object;
  return out;
}

bool JsonValue::as_bool(bool fallback) const {
  return mType == Type::Bool ? mBool : fallback;
}

int64_t JsonValue::as_int(int64_t fallback) const {
  if (mType == Type::Int) return mInt;
  if (mType == Type::Double and std::isfinite(mDouble) and
      mDouble == std::floor(mDouble) and mDouble >= -9.2e18 and
      mDouble <= 9.2e18) {
    return static_cast<int64_t>(mDouble);
  }
  return fallback;
}

double JsonValue::as_double(double fallback) const {
  if (mType == Type::Double) return mDouble;
  if (mType == Type::Int) return static_cast<double>(mInt);
  return fallback;
}

const std::string& JsonValue::as_string() const {
  static const std::string empty;
  return mType == Type::String ? mString : empty;
}

std::string JsonValue::string_or(std::string fallback) const {
  return mType == Type::String ? mString : std::move(fallback);
}

const std::vector<JsonValue>& JsonValue::items() const {
  return mType == Type::Array ? mArray : empty_items();
}

std::vector<JsonValue>& JsonValue::items() {
  if (mType == Type::Null) mType = Type::Array;
  return mArray;
}

void JsonValue::push_back(JsonValue value) {
  if (mType == Type::Null) mType = Type::Array;
  if (mType != Type::Array) return;
  mArray.push_back(std::move(value));
}

const std::vector<JsonValue::Member>& JsonValue::members() const {
  return mType == Type::Object ? mObject : empty_members();
}

std::vector<JsonValue::Member>& JsonValue::members() {
  if (mType == Type::Null) mType = Type::Object;
  return mObject;
}

const JsonValue* JsonValue::find(std::string_view key) const {
  if (mType != Type::Object) return nullptr;
  for (const Member& member : mObject) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}

JsonValue* JsonValue::find(std::string_view key) {
  if (mType != Type::Object) return nullptr;
  for (Member& member : mObject) {
    if (member.key == key) return &member.value;
  }
  return nullptr;
}

const JsonValue& JsonValue::get(std::string_view key) const {
  const JsonValue* found = find(key);
  return found != nullptr ? *found : shared_null();
}

JsonValue& JsonValue::operator[](std::string_view key) {
  if (mType == Type::Null) mType = Type::Object;
  if (JsonValue* found = find(key)) return *found;
  mObject.push_back(Member{std::string(key), JsonValue()});
  return mObject.back().value;
}

void JsonValue::set(std::string_view key, JsonValue value) {
  if (mType == Type::Null) mType = Type::Object;
  if (mType != Type::Object) return;
  if (JsonValue* found = find(key)) {
    *found = std::move(value);
    return;
  }
  mObject.push_back(Member{std::string(key), std::move(value)});
}

bool JsonValue::erase(std::string_view key) {
  if (mType != Type::Object) return false;
  for (auto it = mObject.begin(); it != mObject.end(); ++it) {
    if (it->key == key) {
      mObject.erase(it);
      return true;
    }
  }
  return false;
}

size_t JsonValue::size() const {
  if (mType == Type::Array) return mArray.size();
  if (mType == Type::Object) return mObject.size();
  return 0;
}

std::optional<JsonValue> JsonValue::parse(std::string_view text,
                                          std::string* error) {
  simdjson::dom::parser parser;
  const simdjson::padded_string padded(text);
  simdjson::dom::element root;
  const simdjson::error_code code = parser.parse(padded).get(root);
  if (code) {
    if (error != nullptr) *error = simdjson::error_message(code);
    return std::nullopt;
  }
  return from_element(root);
}

void JsonValue::write(JsonWriter& writer) const {
  std::string scratch;
  switch (mType) {
    case Type::Null:
      writer.null_value();
      break;
    case Type::Bool:
      writer.value(mBool);
      break;
    case Type::Int:
      writer.value(mInt);
      break;
    case Type::Double:
      if (std::isfinite(mDouble)) {
        writer.value(mDouble);
      } else {
        writer.null_value();
      }
      break;
    case Type::String:
      writer.value(safe(mString, scratch));
      break;
    case Type::Array:
      writer.begin_array();
      for (const JsonValue& item : mArray) item.write(writer);
      writer.end_array();
      break;
    case Type::Object:
      writer.begin_object();
      for (const Member& member : mObject) {
        writer.key(safe(member.key, scratch));
        member.value.write(writer);
      }
      writer.end_object();
      break;
  }
}

std::string JsonValue::dump() const {
  JsonWriter writer;
  write(writer);
  return writer.str();
}

std::string JsonValue::dump_pretty() const {
  // One member per line, as session files are formatted: these are files a
  // person opens in an editor, and FracturedJson's table packing would put
  // three servers on one line.
  simdjson::fractured_json_options options;
  options.indent_spaces = 2;
  options.enable_table_format = false;
  options.enable_compact_multiline = false;
  options.max_inline_length = 0;
  options.max_inline_complexity = 0;
  return simdjson::fractured_json_string(dump(), options);
}

bool JsonValue::operator==(const JsonValue& other) const {
  if (is_number() and other.is_number()) {
    if (mType == Type::Int and other.mType == Type::Int) {
      return mInt == other.mInt;
    }
    return as_double() == other.as_double();
  }
  if (mType != other.mType) return false;
  switch (mType) {
    case Type::Null:
      return true;
    case Type::Bool:
      return mBool == other.mBool;
    case Type::String:
      return mString == other.mString;
    case Type::Array:
      return mArray == other.mArray;
    case Type::Object: {
      if (mObject.size() != other.mObject.size()) return false;
      // Key order is presentation, not content.
      for (const Member& member : mObject) {
        const JsonValue* theirs = other.find(member.key);
        if (theirs == nullptr or not(*theirs == member.value)) return false;
      }
      return true;
    }
    default:
      return false;
  }
}

}  // namespace util
