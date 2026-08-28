#ifndef JSON_UTIL_H
#define JSON_UTIL_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <simdjson.h>

namespace agent {

// A JSON value held as its verbatim text, for the places where the schema is
// genuinely dynamic (ollama's `format` is a string or a schema object, `think`
// is a bool or a string, `keep_alive` is a string or a number). simdjson has
// no mutable DOM, so a dynamic value is carried as the JSON source itself and
// spliced in unescaped when a request body is built.
//
// An empty `text` means "unset" and the field is omitted from the request.
struct RawJson {
  std::string text;

  bool empty() const { return text.empty(); }

  // Builders for the common cases, so callers never hand-write escaping.
  static RawJson of_string(std::string_view value);
  static RawJson of_bool(bool value);
  static RawJson of_int(int64_t value);
  static RawJson of_double(double value);
  // `json` is spliced in verbatim — it must already be valid JSON.
  static RawJson of_raw(std::string json);
};

// Incremental JSON writer over simdjson's string_builder. The builder itself
// does no separator bookkeeping, so this tracks whether each open object or
// array has had an element yet and emits commas accordingly.
struct JsonWriter {
  JsonWriter();

  JsonWriter& begin_object();
  JsonWriter& end_object();
  JsonWriter& begin_array();
  JsonWriter& end_array();

  // Writes a separator (if needed) then `"key":`. The next call supplies the
  // value.
  JsonWriter& key(std::string_view name);

  JsonWriter& value(std::string_view v);
  JsonWriter& value(const char* v);
  JsonWriter& value(bool v);
  JsonWriter& value(int64_t v);
  JsonWriter& value(double v);
  JsonWriter& value(const std::vector<std::string>& v);
  JsonWriter& value(const RawJson& v);
  JsonWriter& null_value();

  // key + value in one call.
  template <typename T>
  JsonWriter& field(std::string_view name, const T& v) {
    key(name);
    return value(v);
  }

  // Serialized text. Empty if the builder hit an error (only possible on
  // invalid UTF-8 input).
  std::string str();

private:
  // Emits a comma before the next element, unless it is the first at this
  // depth or it is the value half of a `"key":` that was just written.
  void separate();

  simdjson::builder::string_builder mBuilder;
  std::vector<bool> mHasElement;  // Per open container: has an element yet?
  bool mExpectValue = false;      // A key was just written; its value is next.
};

// ---------------------------------------------------------------------------
// Read helpers.
//
// Each returns `fallback` when the key is absent or holds the wrong type,
// matching nlohmann's `value(key, default)` semantics that this code relied on.
// ---------------------------------------------------------------------------

std::string string_field(simdjson::ondemand::object& obj, std::string_view key,
                         std::string fallback = "");

int64_t int_field(simdjson::ondemand::object& obj, std::string_view key,
                  int64_t fallback = 0);

bool bool_field(simdjson::ondemand::object& obj, std::string_view key,
                bool fallback = false);

std::vector<std::string> string_array_field(simdjson::ondemand::object& obj,
                                            std::string_view key);

// The verbatim JSON text of `key`, for values whose shape isn't known
// statically. Returns `fallback` when absent.
std::string raw_field(simdjson::ondemand::object& obj, std::string_view key,
                      std::string fallback = "");

}  // namespace agent

#endif  // JSON_UTIL_H
