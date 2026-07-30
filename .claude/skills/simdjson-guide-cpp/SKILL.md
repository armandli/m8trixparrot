---
name: simdjson-guide-cpp
description: Expert reference guide for the simdjson C++ library (repo https://github.com/simdjson/simdjson) covering BOTH reading/parsing JSON and writing/serializing JSON. Auto-activates whenever simdjson is used in C++ — building an ondemand::parser, iterating a document, extracting fields, streaming NDJSON with iterate_many, serializing structs with simdjson::builder::string_builder or simdjson::to_json, custom tag_invoke (de)serialization, or wiring simdjson into a CMake build. Use when reading or emitting JSON with simdjson, choosing between the On-Demand and DOM APIs, fixing padding or dangling string_view issues, or optimizing a hot JSON path. Do NOT use for other C++ JSON libraries (nlohmann/json, RapidJSON, Boost.JSON, jsoncpp, glaze) or for JSON handling in non-C++ languages.
---

# simdjson C++ Guide (Read + Write)

simdjson parses JSON using SIMD instructions. Since v4.x it also **writes** JSON via `simdjson::builder`. Both halves ship in the same header.

Upstream: https://github.com/simdjson/simdjson · Include: `#include "simdjson.h"` (pulls in `dom.h`, `ondemand.h`, `builder.h`, `convert.h`) · Namespaces: `simdjson`, `simdjson::ondemand`, `simdjson::builder`.

**Latest release at time of writing: v4.6.5 (2026-07-29).** Always pin a release tag, never `master`.

> This repo's existing code uses nlohmann/json. Reach for simdjson on new, performance-sensitive JSON paths — do not rewrite working nlohmann code.

## Reading JSON — do this

1. Make simdjson available to the build (see **CMake Setup**).
2. Create **one** `ondemand::parser` and reuse it across documents; it owns growable buffers.
3. Get the input as **padded** data — `padded_string`, `padded_string::load(file)`, a `"..."_padded` literal, or `simdjson::padded_input` (C++17). simdjson reads up to `SIMDJSON_PADDING` bytes past the logical end.
4. `parser.iterate(json)` → `ondemand::document`.
5. Access fields **in the order they appear in the JSON**; On-Demand is a forward-only cursor.
6. Handle errors — exceptions (`simdjson_error`) or error codes (`.get(out)`). Pick one style per file.
7. Copy any `std::string_view` you need past the buffer's lifetime.

```cpp
#include <iostream>
#include <string_view>
#include "simdjson.h"
using namespace simdjson;

int main() {
  ondemand::parser parser;
  padded_string json = R"({"name":"ada","age":36,"langs":["c++","ada"]})"_padded;
  ondemand::document doc = parser.iterate(json);

  std::string_view name = doc["name"].get_string();  // view into the buffer
  int64_t age           = doc["age"].get_int64();
  std::cout << name << " is " << age << "\n";

  for (auto lang : doc["langs"].get_array()) {
    std::cout << "  " << std::string_view(lang.get_string()) << "\n";
  }
}
```

Read from a file: `padded_string json = padded_string::load("data.json");`

## Writing JSON — do this

simdjson **can** serialize. Use `simdjson::builder::string_builder` (low level) or `simdjson::to_json(value)` (one-shot).

1. `simdjson::to_json(x)` for anything the library already knows: scalars, `std::string`, `std::vector<T>`, `std::map<std::string,T>`, nested combinations, and any type with a `tag_invoke` serializer.
2. For a custom struct, define a `tag_invoke(serialize_tag, builder, const T&)` overload **in namespace `simdjson`** (C++20). With C++26 + `SIMDJSON_STATIC_REFLECTION`, no boilerplate is needed at all.
3. Reuse a `string_builder` across many objects (`sb.clear()` between them) — it keeps its capacity.
4. Retrieve with `sb.view()` (returns `simdjson_result<std::string_view>`, error-code friendly) rather than the throwing conversion operators.
5. Call `sb.validate_unicode()` if any input string may not be valid UTF-8.

```cpp
#include "simdjson.h"

struct Car {
  std::string make, model;
  int64_t year;
  std::vector<double> tire_pressure;
};

namespace simdjson {
template <typename builder_type>
void tag_invoke(serialize_tag, builder_type &b, const Car &car) {
  b.start_object();
  b.append_key_value("make", car.make);           b.append_comma();
  b.append_key_value("model", car.model);         b.append_comma();
  b.append_key_value("year", car.year);           b.append_comma();
  b.append_key_value("tire_pressure", car.tire_pressure);
  b.end_object();
}
} // namespace simdjson

int main() {
  Car c{"Toyota", "Corolla", 2017, {30.0, 30.2, 30.5}};
  std::string json = simdjson::to_json(c);   // {"make":"Toyota",...}
  std::cout << json << "\n";
}
```

Pretty output: `simdjson::fractured_json_string(simdjson::to_json(x).value(), opts)` with `simdjson::fractured_json_options` (for a DOM element: `simdjson::fractured_json(element, opts)`).
Re-emitting *parsed* JSON is different — see **Round-tripping**.

## CMake Setup

**FetchContent** (matches this repo's dependency pattern in the top-level `CMakeLists.txt`):

```cmake
include(FetchContent)
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson
  GIT_TAG v4.6.5          # pin a release tag
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(simdjson)
```

Then in the target's `CMakeLists.txt`:

```cmake
target_link_libraries(my_app PRIVATE simdjson::simdjson)
```

simdjson is **not header-only** — the `simdjson::simdjson` target (or a compiled `simdjson.cpp`) must be linked in.

Alternatives and the full option table (`SIMDJSON_BUILD_STATIC_LIB`, `SIMDJSON_DEVELOPMENT_CHECKS`, `SIMDJSON_STATIC_REFLECTION`, AVX-512 control, vcpkg/Conan, amalgamation) are in [references/cmake.md](references/cmake.md).

## Error Handling — two styles, pick one

**Exceptions** (concise; any failed access throws `simdjson::simdjson_error`):

```cpp
try {
  ondemand::document doc = parser.iterate(json);
  int64_t age = doc["age"].get_int64();
} catch (const simdjson_error &e) {
  std::cerr << "parse error: " << e.what() << "\n";  // e.error() is the error_code
}
```

**Error codes** (no exceptions). `simdjson_result<T>` supports `.get(out)` returning `error_code`, or structured bindings:

```cpp
ondemand::document doc;
if (auto err = parser.iterate(json).get(doc)) {
  std::cerr << error_message(err) << "\n"; return 1;
}
int64_t age;
if (auto e = doc["age"].get(age)) {
  if (e == NO_SUCH_FIELD) { /* optional key absent */ }
  else                    { /* wrong type or malformed */ }
}
```

Key codes: `SUCCESS`, `NO_SUCH_FIELD`, `INCORRECT_TYPE`, `NUMBER_ERROR`, `CAPACITY`, `MEMALLOC`, `EMPTY`, `TAPE_ERROR`, `UTF8_ERROR`, `NUMBER_OUT_OF_RANGE`. Build with `-DSIMDJSON_DISABLE_EXCEPTIONS=ON` to force error-code mode.

## Reading Values

| JSON | Call |
|------|------|
| string | `value.get_string()` → `std::string_view` (unescaped) |
| integer | `value.get_int64()` / `get_uint64()` |
| float | `value.get_double()` |
| bool | `value.get_bool()` |
| null | `value.is_null()` → `bool` |
| array | `value.get_array()` → `ondemand::array` |
| object | `value.get_object()` → `ondemand::object` |
| unknown | `value.type()` → `json_type` (switch); `value.get_number()` → `ondemand::number` |

```cpp
for (auto v : doc["items"].get_array()) { double x = v.get_double(); }

for (auto field : doc["config"].get_object()) {
  std::string_view key = field.unescaped_key();
  ondemand::value  val = field.value();
}
```

Deep access: `doc.at_pointer("/users/0/name")` (JSON Pointer, RFC 6901) or `doc.at_path("$.users[0].name")` (JSONPath subset). Note `at_pointer` rewinds the document, invalidating earlier values.

Custom struct **deserialization** mirrors serialization: define `tag_invoke(deserialize_tag, val, T&)` in namespace `simdjson`, then `T t; doc.get(t);`. See [references/reading.md](references/reading.md).

## Critical Rules (lifetime & ordering — most bugs live here)

- **Forward-only, lazy.** Access fields in document order. Out-of-order access forces re-scans and can invalidate earlier iterators. Fully consume a nested array/object before advancing the parent.
- **One live document per parser.** Documents are non-copyable. Reuse the parser *sequentially*, never concurrently.
- **Keep the source buffer alive.** `document`, `array`, `object`, `value` are cursors into the padded input. Never return one that outlives its buffer or parser.
- **`string_view` results are views** into the input or the parser's scratch buffer, invalidated by the next parse. Copy to `std::string` to retain.
- **Padding is mandatory.** Use `padded_string` / `padded_string_view` / `padded_input`, or a buffer with `SIMDJSON_PADDING` spare bytes. A bare unpadded `const char*` is undefined behavior.
- **One parser per thread.** Parsers are not thread-safe.
- **Values are consumed once.** Re-reading requires `doc.rewind()` or a fresh `iterate`.

## Round-tripping (read then write)

- Extract a sub-document's raw text while iterating: `simdjson::to_json_string(value)` → `simdjson_result<std::string_view>` over the original bytes. Bind it to a `std::string_view` first, then copy if it must outlive the buffer.
- DOM elements stream directly: `std::cout << element;`, `simdjson::minify(element)`, `simdjson::prettify(element)`.
- Minify a raw buffer: `simdjson::minify(input, len, out_buf, out_len)`.
- To *modify* JSON, parse into your own structs, mutate, then re-emit with `to_json`. simdjson has no mutable DOM.

## Streaming many documents (NDJSON)

```cpp
padded_string ndjson = padded_string::load("lines.ndjson");
for (auto doc : parser.iterate_many(ndjson)) {
  std::string_view id = doc["id"].get_string();
}
```

Batch size defaults to ~1 MB and **must exceed the largest single document**. Check `stream.truncated_bytes()` after the loop. Comma-separated input: pass `simdjson::stream_format::comma_delimited`.

## Testing (always do this — do not stop at compilation)

Build the target, then exercise it:

1. **Valid input** — parse a representative document and assert every extracted field.
2. **Round-trip** — serialize a struct with `to_json`, parse it back, assert equality.
3. **Malformed JSON** — feed truncated/garbage input; confirm the error path runs (exception caught or non-zero `error_code`), not a crash.
4. **Missing key** — omit a field; confirm the `NO_SUCH_FIELD` / default branch is taken.
5. **Wrong type** — put a string where a number is expected; confirm `INCORRECT_TYPE` is handled.
6. **Lifetime** — anything escaping the parse scope must be a copied `std::string`, not a dangling `string_view`.
7. **Non-ASCII** — include escapes and multi-byte UTF-8; on the write side call `validate_unicode()`.

In this repo, build via `make build` (or `cmake --build build -j`) and run the binary from `build/`.

If the build fails with undefined references to `simdjson::*`, the target was not linked — add `simdjson::simdjson` to `target_link_libraries`. If runtime output is garbage or you get `UTF8_ERROR` on valid JSON, the input was not padded.

## Additional Resources

- Reading API in depth — On-Demand, DOM, streams, JSON Pointer/Path, `tag_invoke` deserialization, reflection, implementation selection: [references/reading.md](references/reading.md)
- Writing API in depth — full `string_builder` method table, `to_json`, `extract_from`, fractured/pretty JSON, custom serializers: [references/writing.md](references/writing.md)
- Build integration — FetchContent, find_package, vcpkg/Conan, amalgamation, every `SIMDJSON_*` option, performance flags: [references/cmake.md](references/cmake.md)
- Complete compilable example programs: [references/examples.md](references/examples.md)

### Final Step — Record Usage

After helping write or modify simdjson code, run:

```bash
python3 ${PWD}/.claude/skills/skill-stat/scripts/record-stat.py "simdjson-guide-cpp"
```
