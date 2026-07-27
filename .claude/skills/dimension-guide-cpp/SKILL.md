---
name: dimension-guide-cpp
description: Expert reference guide for parsing JSON in C++ with the simdjson library (repo https://github.com/simdjson/simdjson). Auto-activates whenever simdjson is used to parse or read a JSON string, file, or buffer in C++ — building an ondemand::parser, iterating a document, extracting fields/arrays/objects, handling parse errors, or wiring simdjson into CMake. Use when reading JSON with simdjson, choosing between the On-Demand and DOM APIs, fixing padding or lifetime/dangling-value issues, or optimizing a hot JSON path. Do NOT use for other C++ JSON libraries (this repo's default nlohmann/json, RapidJSON, Boost.JSON, jsoncpp), for serializing/writing JSON, or for JSON handling in non-C++ languages.
---

# simdjson C++ JSON Parsing Guide

simdjson is a high-performance C++ library that parses JSON using SIMD instructions. Its **On-Demand API** is the recommended default: lazy, forward-only, and the fastest way to read values you actually need.

Upstream: https://github.com/simdjson/simdjson · Include: `#include "simdjson.h"` · Namespace: `simdjson`, on-demand types in `simdjson::ondemand`.

> Note: this repo's existing code uses nlohmann/json. Only reach for simdjson on new, performance-sensitive read paths — do not rewrite working nlohmann code.

## When parsing JSON with simdjson, do this

1. Ensure simdjson is available to the build (see **CMake Setup**).
2. Create **one** `ondemand::parser` and reuse it across documents (it owns reusable buffers).
3. Get the input as **padded** data — `padded_string` (from a literal, string, or file). simdjson requires `SIMDJSON_PADDING` trailing bytes.
4. `parser.iterate(json)` to get an `ondemand::document`.
5. Access fields **in the order they appear in the JSON** for best performance.
6. Handle errors — either exceptions (`simdjson_error`) or error codes (see **Error Handling**).
7. Copy any `string_view` you need to keep past the buffer's lifetime.
8. Build and run against real and malformed JSON to verify (see **Testing**).

## CMake Setup

**FetchContent** (matches this repo's dependency pattern in the top-level `CMakeLists.txt`):

```cmake
include(FetchContent)
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson
  GIT_TAG v3.13.0          # pin a release tag
)
FetchContent_MakeAvailable(simdjson)
```

Then in the target's `CMakeLists.txt`:

```cmake
target_link_libraries(my_app PRIVATE simdjson::simdjson)
```

Alternatives:
- **find_package**: `find_package(simdjson CONFIG REQUIRED)` then link `simdjson::simdjson` (needs it installed).
- **Amalgamation**: download `simdjson.h` + `simdjson.cpp` from the release, add `simdjson.cpp` to your sources, `#include "simdjson.h"`. simdjson is *not* header-only — `simdjson.cpp` (or the CMake target) must be compiled in.

## Minimal On-Demand Example

```cpp
#include <iostream>
#include <string_view>
#include "simdjson.h"
using namespace simdjson;

int main() {
  ondemand::parser parser;                       // reuse across documents
  padded_string json = R"({"name":"ada","age":36,"langs":["c++","ada"]})"_padded;

  ondemand::document doc = parser.iterate(json); // throws simdjson_error on failure

  std::string_view name = doc["name"].get_string();   // view into the buffer
  int64_t age           = doc["age"].get_int64();

  std::cout << name << " is " << age << "\n";
  for (auto lang : doc["langs"].get_array()) {
    std::cout << "  " << std::string_view(lang.get_string()) << "\n";
  }
  return 0;
}
```

Load from a file instead of a literal with `padded_string json = padded_string::load("data.json");`.

## Error Handling — two styles, pick one

**Exceptions** (concise; any failed access throws `simdjson::simdjson_error`):

```cpp
try {
  ondemand::document doc = parser.iterate(json);
  int64_t age = doc["age"].get_int64();
} catch (const simdjson_error& e) {
  std::cerr << "parse error: " << e.what() << "\n";   // e.error() is the error_code
}
```

**Error codes** (no exceptions; check every step). `simdjson_result<T>` supports `.get(out)` returning an `error_code`, or structured bindings:

```cpp
ondemand::document doc;
auto err = parser.iterate(json).get(doc);
if (err) { std::cerr << error_message(err) << "\n"; return 1; }

int64_t age;
if (auto e = doc["age"].get(age)) {
  if (e == NO_SUCH_FIELD) { /* field absent */ }
  else { /* wrong type or malformed */ }
}
```

Use `NO_SUCH_FIELD` to detect optional/missing keys, and `INCORRECT_TYPE` for type mismatches.

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
| unknown type | `value.type()` → `json_type` (switch on it) |

Iterate an **array**:
```cpp
for (auto v : doc["items"].get_array()) {
  double x = v.get_double();
}
```

Iterate an **object** (keys as you go):
```cpp
for (auto field : doc["config"].get_object()) {
  std::string_view key = field.unescaped_key();
  ondemand::value val  = field.value();
}
```

JSON Pointer for deep access: `doc.at_pointer("/users/0/name").get_string();`

## Critical Rules (lifetime & ordering — most bugs live here)

- **Forward-only, lazy.** Access fields in document order. Reading out of order forces re-scans and can invalidate earlier iterators. Nested values must be consumed before advancing the parent.
- **One live document per parser.** Don't hold two documents from the same parser at once. Reuse the parser *sequentially*, not concurrently.
- **Keep the source buffer alive.** `document`, `array`, `object`, and `value` are cursors into the `padded_string`. If the buffer is destroyed, they dangle. Never return an `ondemand::value` that outlives its buffer/parser.
- **`string_view` results are views**, backed by the parser/buffer; they can be invalidated by further parsing. Copy to `std::string` if you need to retain them.
- **Padding is mandatory.** Use `padded_string` / `padded_string_view`, or a buffer with `SIMDJSON_PADDING` spare bytes. A raw `std::string` without padding is undefined behavior.
- **One parser per thread.** A parser is not thread-safe; give each thread its own.

## Testing (always do this after writing a parser)

Build the target, then exercise it — do not stop at compilation:

1. **Valid input**: parse a representative document and confirm every extracted field holds the expected value (print or assert).
2. **Malformed JSON**: feed truncated/garbage input and confirm your error path runs (exception caught, or non-zero `error_code`), not a crash.
3. **Missing/optional field**: omit a key and confirm the `NO_SUCH_FIELD` (or default) branch is taken.
4. **Wrong type**: put a string where a number is expected and confirm `INCORRECT_TYPE` is handled.
5. **Lifetime**: if any value escapes the parse scope, confirm it's a copied `std::string`, not a dangling `string_view`.

In this repo, build via `make build` (or `cmake --build build -j`) and run the produced binary from `build/`.

## Additional Resources

- Full API — On-Demand vs DOM, streams (NDJSON), types, capacity, implementations: [references/api-reference.md](references/api-reference.md)
- Complete compilable example programs: [references/examples.md](references/examples.md)

### Final Step — Record Usage

After helping write or modify simdjson parsing code, run:

```bash
python3 ${PWD}/.claude/skills/skill-stat/scripts/record-stat.py "dimension-guide-cpp"
```
