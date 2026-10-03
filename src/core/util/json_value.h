#ifndef M8_UTIL_JSON_VALUE_H
#define M8_UTIL_JSON_VALUE_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <core/util/json_util.h>

namespace util {

// A small mutable JSON tree.
//
// Everything else in the code base reads JSON with simdjson On-Demand and writes
// it with JsonWriter, which is right for fixed request and response shapes. MCP
// is the first place where JSON has to be *changed* rather than read once or
// written once: a server's tool schema is rewritten into a shape Ollama
// accepts, a model's arguments are coerced before they go back out, and a
// user's config file is edited while every key m8 does not know about survives.
// simdjson has no mutable document, hence this type: parsed with simdjson's DOM
// parser, written with JsonWriter.
//
// Objects keep insertion order (a rewritten config file reads like the one the
// user wrote) and look keys up linearly; the objects this handles are small.
// Nothing here throws: typed reads take a fallback, and a lookup of a missing
// key returns a shared null.
struct JsonValue {
  enum struct Type : uint8_t { Null, Bool, Int, Double, String, Array, Object };

  // One object member. Defined after the class, because it holds a JsonValue.
  struct Member;

  JsonValue();
  JsonValue(std::nullptr_t);  // NOLINT: implicit by design, like the rest.
  JsonValue(bool value);      // NOLINT
  JsonValue(double value);    // NOLINT
  JsonValue(const char* value);       // NOLINT
  JsonValue(std::string value);       // NOLINT
  JsonValue(std::string_view value);  // NOLINT

  // Every integer type, so `JsonValue(size)` is never ambiguous. Values above
  // INT64_MAX are carried as doubles, as JSON parsers commonly do.
  template <typename T,
            std::enable_if_t<std::is_integral_v<T> and
                                 not std::is_same_v<T, bool>,
                             int> = 0>
  JsonValue(T value) {  // NOLINT
    if constexpr (std::is_unsigned_v<T> and sizeof(T) >= sizeof(int64_t)) {
      if (value > static_cast<T>(INT64_MAX)) {
        mType = Type::Double;
        mDouble = static_cast<double>(value);
        return;
      }
    }
    mType = Type::Int;
    mInt = static_cast<int64_t>(value);
  }

  JsonValue(const JsonValue& other);
  JsonValue(JsonValue&& other) noexcept;
  JsonValue& operator=(const JsonValue& other);
  JsonValue& operator=(JsonValue&& other) noexcept;
  ~JsonValue();

  static JsonValue array();
  static JsonValue object();

  Type type() const { return mType; }
  bool is_null() const { return mType == Type::Null; }
  bool is_bool() const { return mType == Type::Bool; }
  bool is_int() const { return mType == Type::Int; }
  bool is_double() const { return mType == Type::Double; }
  bool is_number() const { return mType == Type::Int or mType == Type::Double; }
  bool is_string() const { return mType == Type::String; }
  bool is_array() const { return mType == Type::Array; }
  bool is_object() const { return mType == Type::Object; }

  // Typed reads. Each returns `fallback` for a value of another type, so a
  // caller only distinguishes "usable" from "not".
  bool as_bool(bool fallback = false) const;
  // An Int, or a Double holding a whole number.
  int64_t as_int(int64_t fallback = 0) const;
  double as_double(double fallback = 0.0) const;
  // The string, or "" for anything else.
  const std::string& as_string() const;
  std::string string_or(std::string fallback) const;

  // ── arrays ──
  // Empty unless an Array.
  const std::vector<JsonValue>& items() const;
  // Turns a Null into an empty Array first; any other type is left alone and a
  // reference to an internal empty vector is never handed out for it.
  std::vector<JsonValue>& items();
  void push_back(JsonValue value);

  // ── objects ──
  const std::vector<Member>& members() const;
  std::vector<Member>& members();  // Null becomes an empty Object first.
  const JsonValue* find(std::string_view key) const;
  JsonValue* find(std::string_view key);
  bool contains(std::string_view key) const { return find(key) != nullptr; }
  // The member, or a shared null when absent (or when this is not an Object).
  const JsonValue& get(std::string_view key) const;
  // The member, inserted as null if absent. Null becomes an Object first.
  JsonValue& operator[](std::string_view key);
  // Replaces in place when the key exists, so a rewritten member keeps its
  // position; appends otherwise.
  void set(std::string_view key, JsonValue value);
  bool erase(std::string_view key);

  // Item count for an Array, member count for an Object, 0 otherwise.
  size_t size() const;

  // nullopt on malformed input, with simdjson's reason in `error`.
  static std::optional<JsonValue> parse(std::string_view text,
                                        std::string* error = nullptr);

  // Compact JSON. Strings and keys that are not valid UTF-8 are repaired on the
  // way out (see util::sanitize_utf8), and non-finite doubles become null, so
  // the result is always a parseable document.
  std::string dump() const;
  // Two-space indented, one member per line, for files people edit.
  std::string dump_pretty() const;
  void write(JsonWriter& writer) const;

  // Deep comparison. Numbers compare by value across Int and Double.
  bool operator==(const JsonValue& other) const;
  bool operator!=(const JsonValue& other) const { return not(*this == other); }

private:
  Type mType = Type::Null;
  bool mBool = false;
  int64_t mInt = 0;
  double mDouble = 0.0;
  std::string mString;
  std::vector<JsonValue> mArray;
  std::vector<Member> mObject;
};

struct JsonValue::Member {
  std::string key;
  JsonValue value;
};

}  // namespace util

#endif  // M8_UTIL_JSON_VALUE_H
