# simdjson Complete Examples

Self-contained programs. Each compiles with C++20 and links `simdjson::simdjson`.
These were compiled and run against **simdjson v4.6.5** with GCC 15 / C++20.

## 0. Build harness used by every example

`CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.21)
project(simdjson_demo CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson
  GIT_TAG v4.6.5
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(simdjson)

add_executable(demo main.cpp)
target_link_libraries(demo PRIVATE simdjson::simdjson)
```

```bash
cmake -S . -B build && cmake --build build -j && ./build/demo
```

## 1. Read a config file, exception style

```cpp
#include <iostream>
#include <string>
#include <string_view>
#include "simdjson.h"

using namespace simdjson;

struct Config {
  std::string host;
  uint16_t    port;
  bool        verbose;
};

int main(int argc, char **argv) {
  if (argc < 2) { std::cerr << "usage: demo <config.json>\n"; return 1; }

  ondemand::parser parser;
  try {
    padded_string json = padded_string::load(argv[1]);
    ondemand::document doc = parser.iterate(json);

    Config cfg;
    cfg.host    = std::string(std::string_view(doc["host"].get_string()));  // copy!
    cfg.port    = static_cast<uint16_t>(doc["port"].get_uint64());
    cfg.verbose = doc["verbose"].get_bool();

    std::cout << cfg.host << ":" << cfg.port
              << (cfg.verbose ? " (verbose)" : "") << "\n";
  } catch (const simdjson_error &e) {
    std::cerr << "config error: " << e.what() << "\n";
    return 1;
  }
}
```

## 2. Read with error codes and optional fields (no exceptions)

```cpp
#include <iostream>
#include <optional>
#include <string>
#include "simdjson.h"

using namespace simdjson;

int main() {
  ondemand::parser parser;
  auto json = R"({"id":42,"name":"widget","tags":["a","b"]})"_padded;

  ondemand::document doc;
  if (auto e = parser.iterate(json).get(doc)) {
    std::cerr << error_message(e) << "\n"; return 1;
  }

  uint64_t id;
  if (auto e = doc["id"].get(id)) { std::cerr << error_message(e) << "\n"; return 1; }

  std::string_view name;
  if (auto e = doc["name"].get_string().get(name)) {
    std::cerr << error_message(e) << "\n"; return 1;
  }
  std::string name_copy(name);   // survives the parser

  // optional field: absence is not an error
  std::optional<double> weight;
  double w;
  auto we = doc["weight"].get(w);
  if (we == SUCCESS)             { weight = w; }
  else if (we != NO_SUCH_FIELD)  { std::cerr << error_message(we) << "\n"; return 1; }

  ondemand::array tags;
  if (auto e = doc["tags"].get_array().get(tags)) {
    std::cerr << error_message(e) << "\n"; return 1;
  }
  for (auto t : tags) {
    std::string_view s;
    if (t.get_string().get(s)) { continue; }
    std::cout << "tag: " << s << "\n";
  }

  std::cout << id << " " << name_copy
            << " weight=" << (weight ? std::to_string(*weight) : "none") << "\n";
}
```

## 3. Full round trip: struct → JSON → struct (C++20 `tag_invoke`)

```cpp
#include <cassert>
#include <iostream>
#include <string>
#include <vector>
#include "simdjson.h"

struct Car {
  std::string make;
  std::string model;
  int64_t     year{};
  std::vector<double> tire_pressure;
};

namespace simdjson {

// ---- write ----
template <typename builder_type>
void tag_invoke(serialize_tag, builder_type &b, const Car &car) {
  b.start_object();
  b.append_key_value("make", car.make);                   b.append_comma();
  b.append_key_value("model", car.model);                 b.append_comma();
  b.append_key_value("year", car.year);                   b.append_comma();
  b.append_key_value("tire_pressure", car.tire_pressure);
  b.end_object();
}

// ---- read ----
template <typename simdjson_value>
auto tag_invoke(deserialize_tag, simdjson_value &val, Car &car) {
  ondemand::object obj;
  auto error = val.get_object().get(obj);
  if (error) { return error; }
  if ((error = obj["make"].get_string(car.make)))   { return error; }
  if ((error = obj["model"].get_string(car.model))) { return error; }
  if ((error = obj["year"].get(car.year)))          { return error; }
  ondemand::array arr;
  if ((error = obj["tire_pressure"].get_array().get(arr))) { return error; }
  for (auto v : arr) {
    double d;
    if ((error = v.get(d))) { return error; }
    car.tire_pressure.push_back(d);
  }
  return simdjson::SUCCESS;
}

} // namespace simdjson

int main() {
  std::vector<Car> fleet = {
    {"Toyota", "Corolla", 2017, {30.0, 30.2, 30.5, 30.8}},
    {"Ford",   "F-150",   2024, {35.0, 35.0, 35.0, 35.0}},
  };

  // write
  std::string json = simdjson::to_json(fleet);
  std::cout << json << "\n";

  // read back
  simdjson::ondemand::parser parser;
  simdjson::padded_string padded(json);
  simdjson::ondemand::document doc = parser.iterate(padded);

  std::vector<Car> back;
  if (auto e = doc.get(back)) {
    std::cerr << simdjson::error_message(e) << "\n"; return 1;
  }

  assert(back.size() == fleet.size());
  assert(back[1].model == "F-150");
  assert(back[0].tire_pressure.size() == 4);
  std::cout << "round trip ok\n";
}
```

## 4. Manual `string_builder` — streaming assembly in a loop

```cpp
#include <iostream>
#include <string>
#include <vector>
#include "simdjson.h"

struct Event { std::string kind; int64_t ts; std::string payload; };

int main() {
  std::vector<Event> events = {
    {"click", 1700000000, R"(text with "quotes" and \backslash)"},
    {"view",  1700000042, "plain"},
  };

  simdjson::builder::string_builder sb;   // reused; grows once
  sb.start_array();
  for (size_t i = 0; i < events.size(); ++i) {
    const Event &e = events[i];
    sb.start_object();
    sb.append_key_value("kind", e.kind);       sb.append_comma();
    sb.append_key_value("ts", e.ts);           sb.append_comma();
    sb.append_key_value("payload", e.payload); // escaping is automatic
    sb.end_object();
    if (i + 1 < events.size()) { sb.append_comma(); }
  }
  sb.end_array();

  if (!sb.validate_unicode()) { std::cerr << "invalid UTF-8 in input\n"; return 1; }

  std::string_view out;
  if (sb.view().get(out)) { std::cerr << "builder error\n"; return 1; }
  std::cout << out << "\n";

  sb.clear();   // ready for the next batch, capacity retained
}
```

## 5. Filter a large document, keeping raw sub-documents

```cpp
#include <iostream>
#include <string>
#include <vector>
#include "simdjson.h"

using namespace simdjson;

int main() {
  auto cars_json = R"( [
    { "make":"Toyota","model":"Camry","year":2018,"tire_pressure":[40.1,39.9] },
    { "make":"Kia","model":"Soul","year":2012,"tire_pressure":[30.1,31.0] },
    { "make":"Toyota","model":"Tercel","year":1999,"tire_pressure":[29.8,30.0] }
  ] )"_padded;

  ondemand::parser parser;
  std::vector<std::string> recent;    // copy: to_json_string views the input buffer

  for (ondemand::object car : parser.iterate(cars_json)) {
    if (uint64_t(car["year"]) > 2000) {
      // to_json_string returns simdjson_result<string_view>: bind, then copy
      std::string_view raw = simdjson::to_json_string(car["tire_pressure"]);
      recent.emplace_back(raw);
    }
  }

  for (const auto &s : recent) { std::cout << s << "\n"; }
}
```

## 6. NDJSON stream with truncation check

```cpp
#include <iostream>
#include "simdjson.h"

using namespace simdjson;

int main(int argc, char **argv) {
  if (argc < 2) { std::cerr << "usage: demo <file.ndjson>\n"; return 1; }

  ondemand::parser parser;
  padded_string json = padded_string::load(argv[1]);

  ondemand::document_stream stream;
  if (auto e = parser.iterate_many(json).get(stream)) {
    std::cerr << error_message(e) << "\n"; return 1;
  }

  size_t n = 0;
  for (auto it = stream.begin(); it != stream.end(); ++it) {
    if (it.error()) {
      std::cerr << "bad record at byte " << it.current_index() << ": "
                << error_message(it.error()) << "\n";
      continue;
    }
    auto doc = *it;
    std::string_view id;
    if (!doc["id"].get_string().get(id)) { std::cout << id << "\n"; }
    ++n;
  }

  std::cout << n << " records, " << stream.truncated_bytes()
            << " trailing bytes discarded\n";
}
```

## 7. DOM API when random access is needed

```cpp
#include <iostream>
#include "simdjson.h"

using namespace simdjson;

int main() {
  dom::parser parser;
  auto json = R"({"a":{"b":[1,2,3]},"c":"hello"})"_padded;
  dom::element doc = parser.parse(json);

  // random access, any order, repeatedly
  std::cout << doc["c"] << "\n";
  std::cout << doc.at_pointer("/a/b/2") << "\n";
  std::cout << doc["a"] << "\n";

  // re-emit
  std::cout << simdjson::minify(doc)   << "\n";
  std::cout << simdjson::prettify(doc) << "\n";
  std::cout << simdjson::fractured_json(doc) << "\n";   // FracturedJson pretty format
}
```

## 8. Bounded parser for a server request loop

```cpp
#include <iostream>
#include "simdjson.h"

using namespace simdjson;

// Rejects oversized bodies instead of growing the parser without bound.
int handle_request(ondemand::parser &parser, std::string &body) {
  ondemand::document doc;
  auto error = parser.iterate(body).get(doc);
  if (error == CAPACITY) { return 413; }        // Payload Too Large
  if (error)             { return 400; }        // Bad Request

  std::string_view action;
  if (doc["action"].get_string().get(action)) { return 400; }
  std::cout << "action: " << action << "\n";
  return 200;
}

int main() {
  ondemand::parser parser(1000 * 1000);   // 1 MB cap
  parser.allocate(1000 * 1000);           // pre-allocate; no growth later

  std::string body = R"({"action":"ping"})";
  std::cout << handle_request(parser, body) << "\n";
}
```

## 9. Pretty-printing a struct (FracturedJson)

```cpp
#include <iostream>
#include <string>
#include <vector>
#include "simdjson.h"

struct Car { std::string make, model; int64_t year; std::vector<double> tire_pressure; };

namespace simdjson {
template <typename builder_type>
void tag_invoke(serialize_tag, builder_type &b, const Car &car) {
  b.start_object();
  b.append_key_value("make", car.make);                   b.append_comma();
  b.append_key_value("model", car.model);                 b.append_comma();
  b.append_key_value("year", car.year);                   b.append_comma();
  b.append_key_value("tire_pressure", car.tire_pressure);
  b.end_object();
}
} // namespace simdjson

int main() {
  Car c = {"Toyota", "Corolla", 2017, {30.0, 30.2}};

  simdjson::fractured_json_options opts;
  opts.indent_spaces = 2;

  std::cout << simdjson::fractured_json_string(simdjson::to_json(c).value(), opts) << "\n";
}
```

Output:

```json
{
  "make": "Toyota",
  "model": "Corolla",
  "year": 2017,
  "tire_pressure": [ 30.0, 30.2 ]
}
```

## 10. Diagnostics: which SIMD kernel is active

```cpp
#include <iostream>
#include "simdjson.h"

int main() {
  std::cout << "simdjson " << SIMDJSON_VERSION << "\n";
  std::cout << "active: " << simdjson::get_active_implementation()->name()
            << " (" << simdjson::get_active_implementation()->description() << ")\n";
  for (auto impl : simdjson::get_available_implementations()) {
    std::cout << "  available: " << impl->name() << "\n";
  }
}
```
