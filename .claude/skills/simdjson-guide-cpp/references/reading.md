# simdjson Reading / Parsing Reference

Deep reference for the read side of simdjson v4.x. Load when a task goes beyond the
basic `iterate` / `get` flow in SKILL.md. Assumes `#include "simdjson.h"` and
`using namespace simdjson;`.

## 1. Choosing an API

| API | Namespace | Model | Use when |
|-----|-----------|-------|----------|
| **On-Demand** | `simdjson::ondemand` | Lazy forward-only cursor over the text | Default. Fastest. You read each value roughly once, in document order. |
| **DOM** | `simdjson::dom` | Eager; whole document materialized on a tape | You need repeated random access to the same nodes, or the code is simpler with a tree. Slower, more memory. |
| **Compile-time** | `simdjson::compile_time` | `constexpr` parse at compile time | C++26 + reflection only. Embedded config. |

## 2. Input: getting padded data

simdjson may read up to `simdjson::SIMDJSON_PADDING` bytes past the logical end of
the buffer. Every input path below guarantees that.

| Method | Padding | Ownership | Notes |
|--------|---------|-----------|-------|
| `padded_string::load("f.json")` | auto | owned | Recommended for files. |
| `"[1,2,3]"_padded` | auto | owned | UDL for literals (`using namespace simdjson`). |
| `simdjson::padded_input(sv)` | auto/conditional | usually non-owning | C++17+. Adds padding only if needed. May trip sanitizers. |
| `std::string` (non-const lvalue) | checked | may copy | simdjson can grow the string in place. |
| `padded_string_view(ptr, len, capacity)` | manual | user | Zero-copy when you already reserved `capacity >= len + SIMDJSON_PADDING`. |
| `padded_memory_map("f.json")` | auto | non-owning | mmap; POSIX always, Windows needs `-DSIMDJSON_ENABLE_MEMORY_FILE_MAPPING_ON_WINDOWS=ON`. |

```cpp
ondemand::parser parser;
auto json = padded_string::load("cars.json");
ondemand::document doc = parser.iterate(json);
```

A thread-local parser is available via `ondemand::parser::get_parser()`.

## 3. Parser and document

```cpp
ondemand::parser parser;               // reuse across documents
ondemand::parser bounded(1000*1000);   // cap document size at 1 MB
bounded.allocate(1000*1000);           // pre-allocate, avoid growth
```

- `parser.iterate(...)` → `simdjson_result<ondemand::document>`; `CAPACITY` error if the
  document exceeds the cap (useful for rejecting oversized HTTP bodies with 413).
- A `document` is single-pass and forward-only, and is **non-copyable**.
- `doc.rewind()` resets the cursor to the start so values can be read again.
- `doc.type()` reports the top-level `json_type`.
- Trailing-garbage detection: after consuming the document, `doc.at_end()` /
  re-checking the iterate result surfaces `TRAILING_CONTENT`.

## 4. Extracting values

Every accessor returns `simdjson_result<T>`, consumable three ways:

```cpp
int64_t a = doc["a"].get_int64();          // 1. implicit/throwing (exception mode)
int64_t b; auto e = doc["b"].get(b);       // 2. .get(out) -> error_code
auto [c, err] = doc["c"].get_int64();      // 3. structured binding
```

Accessors: `get_int64()`, `get_uint64()`, `get_double()`, `get_bool()`, `get_string()`,
`get_array()`, `get_object()`, `get_number()`, `is_null()`, `type()`,
`raw_json()` / `raw_json_token()` (underlying text), `get_raw_json_string()`.

Explicit casts also work: `double(v)`, `uint64_t(v)`, `bool(v)`.

### Polymorphic values

```cpp
switch (value.type()) {
  case json_type::array:   /* get_array()  */ break;
  case json_type::object:  /* get_object() */ break;
  case json_type::number:  /* get_number() */ break;
  case json_type::string:  /* get_string() */ break;
  case json_type::boolean: /* get_bool()   */ break;
  case json_type::null:    /* is_null()    */ break;
}
```

`ondemand::number` (`value.get_number()`) reports `get_number_type()` →
`signed_integer` / `unsigned_integer` / `floating_point_number`, plus
`is_int64()`, `get_int64()`, `get_double()`, etc. Use it when the numeric type is
not known statically.

Non-standard numbers: `SIMDJSON_ALLOW_INFINITY` / `SIMDJSON_ALLOW_NAN` macros, or
the CMake option `SIMDJSON_ENABLE_NAN_INF`.

## 5. Arrays and objects

```cpp
ondemand::array arr = doc["items"].get_array();
size_t n = arr.count_elements();   // scans the whole array — call before iterating, then rewind
for (auto v : arr) { /* v is simdjson_result<ondemand::value> */ }
for (double x : arr) { /* cast during iteration */ }

ondemand::object obj = doc["cfg"].get_object();
size_t k = obj.count_fields();     // also a full scan
for (auto field : obj) {
  std::string_view key = field.unescaped_key();  // escapes resolved
  std::string_view raw = field.escaped_key();    // raw token
  ondemand::value  v   = field.value();
}
```

Lookup:
- `obj["host"]` — order-insensitive: searches forward, then wraps once. Missing key → `NO_SUCH_FIELD`.
- `obj.find_field("host")` — order-**sensitive**, forward only, fastest. Use when you know the key order.
- Repeated `obj["k"]` lookups on the same object are O(n) each — extract once into a local.

C++20 ranges work on arrays/objects: `for (auto v : array | std::views::take(10))`.

## 6. Strings

- `get_string()` → `std::string_view` with escapes resolved, backed by the parser's
  string buffer; valid only until the next parse.
- `get_string(true)` — `allow_replacement`: substitutes U+FFFD for invalid UTF-8 instead of erroring.
- `get_raw_json_string()` — the raw token including quotes and escapes.
- To retain: `std::string kept(value.get_string().value());`

## 7. JSON Pointer and JSONPath

```cpp
double p = doc.at_pointer("/0/tire_pressure/1");       // RFC 6901; "~1" = "/", "~0" = "~"
double q = doc.at_path("[0].tire_pressure[1]");        // JSONPath subset (RFC 9535)
int    x = obj.at_path("$.c.foo.a[1]");

doc.for_each_at_path_with_wildcard("$.address.*", [](auto v) { /* ... */ });
```

`at_pointer()` calls `rewind()` internally, invalidating previously obtained values.
Build dynamic pointers with string concatenation:
`doc.at_pointer("/" + std::to_string(i) + "/name")`.

## 8. Custom type deserialization

### tag_invoke (C++20, recommended)

```cpp
namespace simdjson {
template <typename simdjson_value>
auto tag_invoke(deserialize_tag, simdjson_value &val, Car &car) {
  ondemand::object obj;
  auto error = val.get_object().get(obj);
  if (error) return error;
  if ((error = obj["make"].get_string(car.make)))  return error;
  if ((error = obj["year"].get(car.year)))         return error;
  return simdjson::SUCCESS;
}
} // namespace simdjson

Car c(doc);            // exception style
Car c2; doc.get(c2);   // error-code style
```

Container support is automatic once the element type has a deserializer:

```cpp
std::vector<Car>            cars(doc);
std::optional<Car>          maybe = doc.get<std::optional<Car>>();
std::map<std::string, Car>  m     = doc.get<std::map<std::string, Car>>();
std::unique_ptr<Car>        p(doc);
```

### Template specialization (pre-C++20)

```cpp
template <>
simdjson_inline simdjson_result<Car>
simdjson::ondemand::value::get() noexcept {
  ondemand::object obj;
  auto error = get_object().get(obj);
  if (error) return error;
  Car car;
  if ((error = obj["make"].get_string(car.make)))     return error;
  if ((error = obj["year"].get_int64().get(car.year))) return error;
  return car;
}
```

### Static reflection (C++26)

```cpp
#define SIMDJSON_STATIC_REFLECTION 1
#include "simdjson.h"

struct Car { std::string make, model; int year; };
Car c = doc.get<Car>();                       // no boilerplate

struct Renamed {
  [[= simdjson::rename<"first_name">]] std::string firstName;
  [[= simdjson::skip]]                 int internalState;
};
```

**Key selectors / partial deserialization** — compile-time perfect hashing extracts a
subset in one pass:

```cpp
Car car{};
doc.extract_into<"make", "model">(car);
```

Limits: field names ≤ 63 chars, ≤ 255 fields, no special characters; exceeding them
silently falls back to per-field lookup.

## 9. Streaming: iterate_many (NDJSON / concatenated JSON)

```cpp
ondemand::document_stream stream = parser.iterate_many(ndjson);
for (auto doc : stream) { std::string_view id = doc["id"].get_string(); }
size_t lost = stream.truncated_bytes();   // call only after full iteration
```

With options:

```cpp
parser.iterate_many(json, /*batch_size*/ 1000000,
                    simdjson::stream_format::comma_delimited).get(stream);
```

- Batch size defaults to `ondemand::DEFAULT_BATCH_SIZE` (~1 MB) and must exceed the
  largest single document.
- Iterator introspection: `i.current_index()` (byte offset), `i.source()`
  (`string_view` of that document), `i.error()`.

DOM equivalent is `parser.parse_many(...)` → `dom::document_stream`.

## 10. DOM API

```cpp
dom::parser parser;
dom::element doc = parser.load("file.json");        // or parser.parse(padded_string)
int64_t age = doc["age"];
for (dom::element child : doc["items"]) { /* ... */ }
for (auto [key, val] : doc.get_object()) { /* C++17 */ }
double v = doc.at_pointer("/0/tire_pressure/1");
```

- `dom::element` is a 16-byte borrowed reference — valid only while the `dom::parser` lives.
- `element.type()` → `dom::element_type`.
- Serialization back to text: `std::cout << element`, `simdjson::minify(element)`,
  `simdjson::prettify(element)`. DOM numbers round-trip as doubles.
- `parser.parse_unpadded(ptr, len)` avoids a temporary copy for unpadded buffers.

## 11. Extracting raw JSON sub-documents (On-Demand)

```cpp
std::vector<std::string> tires;                  // copies; survives the parse
for (ondemand::object car : parser.iterate(json)) {
  if (uint64_t(car["year"]) > 2000) {
    std::string_view raw = simdjson::to_json_string(car["tire_pressure"]);
    tires.emplace_back(raw);
  }
}
```

`simdjson::to_json_string(value)` returns `simdjson_result<std::string_view>` over the
**original input bytes** — no re-serialization, same lifetime rules as any other view.
Bind it to a `std::string_view` (or call `.value()`) before constructing a
`std::string`; `emplace_back` on the raw result will not compile.

## 12. Utilities

```cpp
bool ok = simdjson::validate_utf8(ptr, len);

const char *in = "[ 1, 2, 3 ]";
std::unique_ptr<char[]> buf{new char[std::strlen(in)]};
size_t out_len;
auto error = simdjson::minify(in, std::strlen(in), buf.get(), out_len); // -> "[1,2,3]"
```

## 13. Runtime implementation selection

simdjson dispatches at runtime to the best SIMD kernel (icelake / haswell / westmere /
arm64 / ppc64 / lsx / fallback).

```cpp
std::cout << get_active_implementation()->name() << "\n";
get_active_implementation() = get_available_implementations()["fallback"];  // force
```

Do **not** compile with `-march=native` — it defeats runtime dispatch and portability.
Disable AVX-512 (e.g. downclocking concerns) with `cmake -D SIMDJSON_AVX512_ALLOWED=OFF`.

## 14. Development checks

```cpp
#define SIMDJSON_DEVELOPMENT_CHECKS 1
#include "simdjson.h"
```

Adds expensive asserts that catch On-Demand misuse (out-of-order access, reusing a
consumed value, two live documents). Automatically on in debug builds. Remove for release.

## 15. Threading

Parsers (On-Demand and DOM) are **not** thread-safe and hold reusable state. One
parser per thread. Do not share documents/values across threads while reading, and do
not mutate the input buffer during parsing.
