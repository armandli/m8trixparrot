# simdjson API Reference

Deep reference for simdjson (v3.x). Load this when a task goes beyond the basic
iterate/get flow in SKILL.md. Assumes `#include "simdjson.h"` and
`using namespace simdjson;`.

## On-Demand API (recommended)

### Parser and document

```cpp
ondemand::parser parser;                       // reuse; owns growable buffers
padded_string json = padded_string::load("f.json");   // or ..._padded literal
ondemand::document doc = parser.iterate(json);
```

- `parser.iterate(...)` returns `simdjson_result<ondemand::document>`.
- A `document` is **single-pass and forward-only**. Re-iterate by calling `iterate` again on the source.
- The parser allocates on first use and grows as needed; set an upper bound with `parser.iterate(json, /*capacity*/)` overloads only if you need to cap memory.

### Getting typed values

`simdjson_result<T>` (returned by every accessor) offers three consumption styles:

```cpp
// 1. Implicit/throwing conversion (exception mode)
int64_t a = doc["a"].get_int64();

// 2. .get(out) -> error_code (no exceptions)
int64_t b; auto e = doc["b"].get(b);

// 3. structured binding
auto [c, err] = doc["c"].get_int64();
```

Accessors: `get_int64()`, `get_uint64()`, `get_double()`, `get_bool()`,
`get_string()`, `get_array()`, `get_object()`, `is_null()`, `type()`,
`raw_json()` / `raw_json_token()` (the underlying text).

### Types and polymorphic JSON

```cpp
switch (value.type()) {
  case json_type::array:   /* get_array()  */ break;
  case json_type::object:  /* get_object() */ break;
  case json_type::number:  /* get_double() or get_number() */ break;
  case json_type::string:  /* get_string() */ break;
  case json_type::boolean: /* get_bool()   */ break;
  case json_type::null:    /* is_null()    */ break;
}
```

`value.get_number()` yields an `ondemand::number` that can report whether it is a
signed int, unsigned int, or double (`number.get_number_type()`), useful when the
numeric type isn't known ahead of time.

### Arrays and objects

```cpp
ondemand::array arr = doc["items"].get_array();
size_t n = arr.count_elements();      // note: consumes; call before iterating
for (auto v : arr) { /* v is simdjson_result<ondemand::value> */ }

ondemand::object obj = doc["cfg"].get_object();
for (auto field : obj) {
  std::string_view k = field.unescaped_key();   // or field.key() for raw
  ondemand::value  v = field.value();
}
// direct lookup (searches from current position, wraps once):
auto host = obj["host"];
```

Field lookup within an object is **order-sensitive** for speed: looking up keys in
the JSON's own order avoids re-scans. Missing keys yield `NO_SUCH_FIELD`.

### JSON Pointer

```cpp
auto name = doc.at_pointer("/users/0/name").get_string();
```

## Strings and escaping

- `get_string()` returns a `std::string_view` with escapes resolved, backed by the
  parser's string buffer. It is valid only until the next parse or buffer change.
- To retain a value: `std::string kept(value.get_string().value());`.
- `get_string(true)` (allow_replacement) tolerates invalid unicode by replacing it.
- Keys: `field.unescaped_key()` (processed) vs `field.key()` (raw token including quotes).

## Error handling reference

- Exception type: `simdjson::simdjson_error` (`.what()`, `.error()` → `error_code`).
- `error_code` enum values of note: `SUCCESS`, `NO_SUCH_FIELD`, `INCORRECT_TYPE`,
  `NUMBER_ERROR`, `INCORRECT_TYPE`, `TAPE_ERROR`, `CAPACITY`, `MEMALLOC`,
  `EMPTY`, `UNCLOSED_STRING`, `UNESCAPED_CHARS`, `UTF8_ERROR`, `NUMBER_OUT_OF_RANGE`.
- `error_message(code)` → human-readable string.
- To force error-code mode, always call `.get(out)` or bind `auto [v, e]`; never
  implicitly convert (that path throws).

## Streaming many documents (NDJSON / concatenated JSON)

```cpp
padded_string ndjson = padded_string::load("lines.ndjson");
ondemand::document_stream stream = parser.iterate_many(ndjson);
for (auto doc : stream) {
  // each `doc` is one JSON document in the stream
  std::string_view id = doc["id"].get_string();
}
```

`iterate_many` is ideal for large files of one-JSON-per-line records; it parses in
batches and keeps memory bounded.

## DOM API (older; eager, random-access)

Use only when you need repeated random access to the same nodes or an
eager/materialized tree. It is simpler but slower and uses more memory than
On-Demand.

```cpp
dom::parser parser;
dom::element doc = parser.load("file.json");   // or parser.parse(padded_string)
int64_t age = doc["age"];                       // implicit conversions
for (dom::element child : doc["items"]) { /* ... */ }
for (auto [key, val] : doc.get_object()) { /* ... */ }
```

DOM elements remain valid as long as the `dom::parser` (which owns the parsed tape
and string buffer) is alive.

## Padding details

- Input buffers must have at least `simdjson::SIMDJSON_PADDING` readable bytes past
  the logical end. `padded_string` and `padded_string::load` guarantee this.
- To parse an existing buffer you own with sufficient trailing capacity, wrap it in
  `padded_string_view(ptr, length, capacity)` to avoid a copy.
- Parsing a plain `std::string`/`char*` without padding is undefined behavior.

## Runtime implementation selection

simdjson picks the best SIMD kernel (haswell/westmere/icelake/arm64/fallback) at
runtime. Inspect or override:

```cpp
std::cout << "active: " << get_active_implementation()->name() << "\n";
// force a specific one (e.g. for reproducibility):
get_active_implementation() = get_available_implementations()["fallback"];
```

## Threading

A `parser` (On-Demand or DOM) is **not** thread-safe and holds reusable state. Give
each thread its own parser. Documents/values must not be shared across threads while
being read.
