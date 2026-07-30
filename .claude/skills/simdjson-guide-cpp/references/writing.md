# simdjson Writing / Serialization Reference

**Yes — simdjson writes JSON as well as reads it.** Serialization lives in
`simdjson::builder` and ships inside `simdjson.h` (which includes
`simdjson/builder.h`). It was added in the v4 line; if your pinned tag is v3.x or
older, upgrade before using anything on this page.

Assumes `#include "simdjson.h"`. C++20 unlocks the template-key and automatic
container overloads; C++26 + reflection removes the boilerplate entirely.

## 1. Three levels of API

| Level | Entry point | Use when |
|-------|-------------|----------|
| One-shot | `simdjson::to_json(value)` | You just want a `std::string` of JSON. |
| Declarative | `tag_invoke(serialize_tag, ...)` per type, then `to_json` | Custom structs, reusable across the codebase. |
| Manual | `simdjson::builder::string_builder` | Full control, streaming assembly, hot loops. |

## 2. `simdjson::to_json`

```cpp
std::vector<std::vector<double>> c = {{1.0, 2.0}, {3.0, 4.0}};

std::string json = simdjson::to_json(c);                 // throwing

std::string out;                                          // reuse the buffer
auto error = simdjson::to_json(c, out);
if (error) { /* handle */ }

std::string j2 = simdjson::to_json(c, 31123);             // size hint
auto err2      = simdjson::to_json(c, out, 31123);

std::string j3;                                           // no-exception style
if (simdjson::to_json(c).get(j3)) { /* error */ }
```

`to_json` returns `simdjson_result<std::string>`, so the one-argument form only
assigns straight to a `std::string` when exceptions are enabled. Use the `(value, out)`
overload or `.get(out)` in no-exception builds, and `.value()` when you need the string
inline.

For serializing only a subset of a struct's fields, see `extract_from` in §6 — it is
reflection-only (C++26).

## 3. `string_builder`

```cpp
simdjson::builder::string_builder sb;
simdjson::builder::string_builder sb2{1233213};   // initial capacity
```

The builder uses a growable buffer, so appending in a loop is linear, not quadratic.
**Reuse one instance** across many documents and call `clear()` between them —
capacity is retained.

### Appending content

| Method | Effect |
|--------|--------|
| `append(v)` | Numbers and booleans (`true`/`false`); floats use the shortest accurate representation |
| `append(char c)` | One raw character |
| `append_null()` | Literal `null` |
| `append(container)` | C++20: `std::vector<T>`, `std::map<std::string,T>`, nested — full JSON value |
| `escape_and_append(sv)` | String contents, JSON-escaped, **no** surrounding quotes |
| `escape_and_append_with_quotes(sv)` | Escaped **and** quoted — the normal way to write a string value |
| `escape_and_append_with_quotes(char)` | Single char, quoted |
| `escape_and_append_with_quotes<"literal">()` | C++20 compile-time constant, escaping done at compile time |
| `append_raw(const char*)` | Raw, unescaped, NUL-terminated — for pre-formatted JSON |
| `append_raw(std::string_view)` | Raw, unescaped |
| `append_raw(const char*, size_t)` | Raw, unescaped, explicit length |
| `append_key_value(key, value)` | Writes `"key":value` |
| `append_key_value<"mykey">(value)` | C++20 compile-time key |

`append_raw` bypasses escaping — only pass it text you know is already valid JSON
(e.g. a `to_json_string()` slice from a parsed document). Anything user-supplied must
go through `escape_and_append_with_quotes`.

### Structure control

`start_object()` `end_object()` `start_array()` `end_array()` `append_comma()` `append_colon()`

You are responsible for commas between elements — the builder does not track them.

### Validation and retrieval

| Method | Notes |
|--------|-------|
| `view()` | → `simdjson_result<std::string_view>`. **Preferred** (C++20); error-code friendly |
| `operator std::string_view()` | May throw |
| `operator std::string()` | May throw; copies |
| `validate_unicode()` | → `bool`; true if the accumulated buffer is valid UTF-8 |
| `clear()` | Resets write position to 0, keeps capacity |

```cpp
std::string_view p;
if (sb.view().get(p)) { return false; }   // error path, no exceptions
```

## 4. Manual example

```cpp
struct Car {
  std::string make, model;
  int64_t year;
  std::vector<double> tire_pressure;
};

void serialize_car(const Car &car, simdjson::builder::string_builder &builder) {
  builder.start_object();

  builder.append_key_value("make", car.make);
  builder.append_comma();
  builder.append_key_value("model", car.model);
  builder.append_comma();
  builder.append_key_value("year", car.year);
  builder.append_comma();

  builder.escape_and_append_with_quotes("tire_pressure");
  builder.append_colon();
  builder.start_array();
  for (size_t i = 0; i < car.tire_pressure.size(); ++i) {
    builder.append(car.tire_pressure[i]);
    if (i + 1 < car.tire_pressure.size()) { builder.append_comma(); }
  }
  builder.end_array();

  builder.end_object();
}
```

C++20 shorthand with compile-time keys and automatic container handling:

```cpp
Car c = {"Toyota", "Corolla", 2017, {30.0, 30.2, 30.513, 30.79}};
simdjson::builder::string_builder sb;
sb.start_object();
sb.append_key_value<"make">(c.make);                    sb.append_comma();
sb.append_key_value<"model">(c.model);                  sb.append_comma();
sb.append_key_value<"year">(c.year);                    sb.append_comma();
sb.append_key_value<"tire_pressure">(c.tire_pressure);  // vector<double> handled
sb.end_object();
std::string_view p = sb.view();
```

Containers serialize directly with no struct at all:

```cpp
std::map<std::string, double> m = {{"key1", 1}, {"key2", 1}};
simdjson::builder::string_builder sb;
sb.append(m);
std::string_view p = sb.view();
```

## 5. Custom serializers via `tag_invoke`

Define the overload **inside namespace `simdjson`** so ADL finds it. Once defined, the
type works with `to_json`, with `sb.append(...)`, and inside any container.

```cpp
namespace simdjson {
template <typename builder_type>
void tag_invoke(serialize_tag, builder_type &builder, const Car &car) {
  builder.start_object();
  builder.append_key_value("make", car.make);                   builder.append_comma();
  builder.append_key_value("model", car.model);                 builder.append_comma();
  builder.append_key_value("year", car.year);                   builder.append_comma();
  builder.append_key_value("tire_pressure", car.tire_pressure);
  builder.end_object();
}
} // namespace simdjson

std::string json = simdjson::to_json(std::vector<Car>{c1, c2});
```

Pair it with the matching `deserialize_tag` overload (see `reading.md` §8) to get a
full round trip for the type.

## 6. Static reflection (C++26)

```cpp
#define SIMDJSON_STATIC_REFLECTION 1
#include "simdjson.h"

struct Car { std::string make, model; int64_t year; std::vector<double> tire_pressure; };

simdjson::builder::string_builder sb;
Car c = {"Toyota", "Corolla", 2017, {30.0, 30.2}};
sb << c;                       // no tag_invoke needed
std::string_view p{sb};
```

Field annotations:

```cpp
struct Person {
  [[= simdjson::rename<"first_name">]] std::string firstName = "";
  [[= simdjson::rename<"last_name">]]  std::string lastName  = "";
  [[= simdjson::skip]]                 int internalCache     = 0;
  int age = 0;
};

Person p{"Alice", "Smith", 999, 30};
std::string json = simdjson::to_json(p);
// {"first_name":"Alice","last_name":"Smith","age":30}
```

Reflection also unlocks **`simdjson::extract_from<...>`** — serialize a chosen subset
of fields:

```cpp
struct Car { std::string make, model; int year; double price; bool electric; };

Car car{"Ford", "F-150", 2024, 55000.0, false};
std::string json = simdjson::extract_from<"year", "price">(car);   // {"year":2024,"price":55000.0}

std::string out;
auto error = simdjson::extract_from<"year", "price">(car).get(out);
```

All of §6 requires the `SIMDJSON_STATIC_REFLECTION` CMake option (or macro) and a
compiler with C++26 reflection. Without it, `extract_from` and
`to_fractured_json_string` do not exist. Everything else on this page works via
`tag_invoke`.

## 7. Pretty printing — fractured JSON

**Verified against v4.6.5.** The generally available entry points are:

```cpp
// (a) format any JSON text — pair with to_json for structs
simdjson::fractured_json_options opts;
opts.indent_spaces = 2;
std::string pretty = simdjson::fractured_json_string(simdjson::to_json(car).value(), opts);

// (b) format a parsed DOM element
dom::parser parser;
dom::element doc = parser.parse(json);
std::string pretty2 = simdjson::fractured_json(doc, opts);   // opts optional
```

`fractured_json_string(std::string_view [, opts])` and `fractured_json(T [, opts])`
both return a plain `std::string`.

> `to_fractured_json_string(obj, opts, capacity)` appears in the upstream docs but is
> compiled only under `SIMDJSON_STATIC_REFLECTION`, and lives in a per-architecture
> namespace (`simdjson::haswell::builder::…`) — it is **not** reachable as
> `simdjson::to_fractured_json_string` or `simdjson::builder::to_fractured_json_string`.
> Use form (a) above instead.

```json
{
    "records": [
        { "active": true , "id": 1, "name": "Alice" },
        { "active": false, "id": 2, "name": "Bob"   },
        { "active": true , "id": 3, "name": "Carol" }
    ]
}
```

### `fractured_json_options`

| Option | Default | Purpose |
|--------|---------|---------|
| `max_total_line_length` | 120 | Max characters per line |
| `max_inline_length` | 80 | Max length of an inlined element |
| `max_inline_complexity` | 2 | Max nesting depth rendered inline |
| `max_compact_array_complexity` | 1 | Max complexity for compact arrays |
| `indent_spaces` | 4 | Spaces per indent level |
| `enable_table_format` | true | Align similar objects into columns |
| `min_table_rows` | 3 | Rows required to trigger table mode |
| `table_similarity_threshold` | 0.8 | Fraction of shared keys for table detection |
| `enable_compact_multiline` | true | Several items per line in arrays |
| `max_items_per_line` | 10 | Array items per line cap |
| `simple_bracket_padding` | true | `{ }` vs `{}` |
| `colon_padding` | true | `": "` vs `":"` |
| `comma_padding` | true | `", "` vs `","` |

For re-emitting an already-parsed DOM element there is also `simdjson::prettify(element)`
and `simdjson::minify(element)`.

## 8. Supported types out of the box

Numbers (all integer/float widths), `bool`, `nullptr`/null, `std::string`,
`std::string_view`, `const char*`, `std::vector<T>`, `std::map<std::string, T>`,
`std::optional<T>`, `std::tuple<...>`, nested combinations, and any type with a
`tag_invoke` serializer or (C++26) reflectable members.

## 9. Rules and gotchas

- **UTF-8 in, UTF-8 out.** The builder escapes JSON metacharacters and control
  characters but does not transcode. Call `validate_unicode()` when strings come from
  an untrusted source.
- **The builder does not validate structure.** Mismatched `start_object`/`end_array`
  or missing commas produce invalid JSON silently. Round-trip in tests.
- **`view()` points into the builder's buffer.** It dangles after `clear()`, further
  appends that reallocate, or builder destruction. Copy to `std::string` to keep it.
- **Reuse the builder** in hot loops; construction plus growth is the main cost.
- **No mutable DOM.** simdjson cannot edit a parsed document in place. Parse into your
  own types, mutate those, re-serialize with `to_json`.
