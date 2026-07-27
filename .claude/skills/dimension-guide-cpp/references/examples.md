# simdjson Complete Examples

Self-contained programs compiled against simdjson v3.x. Link `simdjson::simdjson`
(or compile `simdjson.cpp` with the amalgamation). Build in this repo with
`make build` and run the binary from `build/`.

## 1. Parse a JSON string with exception-based error handling

```cpp
#include <iostream>
#include <string>
#include <string_view>
#include "simdjson.h"
using namespace simdjson;

int main() {
  ondemand::parser parser;
  padded_string json = R"({
    "model": "ornith:35b",
    "temperature": 0.7,
    "stream": true,
    "stop": ["\n\n", "END"]
  })"_padded;

  try {
    ondemand::document doc = parser.iterate(json);

    std::string model    = std::string(doc["model"].get_string().value()); // keep a copy
    double temperature   = doc["temperature"].get_double();
    bool stream          = doc["stream"].get_bool();

    std::cout << "model=" << model << " temp=" << temperature
              << " stream=" << stream << "\n";

    std::cout << "stop tokens:";
    for (auto tok : doc["stop"].get_array())
      std::cout << " [" << std::string_view(tok.get_string()) << "]";
    std::cout << "\n";
  } catch (const simdjson_error& e) {
    std::cerr << "parse error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
```

## 2. Error-code style with optional/missing fields

```cpp
#include <iostream>
#include <string>
#include "simdjson.h"
using namespace simdjson;

int main() {
  ondemand::parser parser;
  padded_string json = R"({"id": 42, "name": "widget"})"_padded;

  ondemand::document doc;
  if (auto e = parser.iterate(json).get(doc)) {
    std::cerr << "iterate failed: " << error_message(e) << "\n";
    return 1;
  }

  int64_t id;
  if (auto e = doc["id"].get(id)) {
    std::cerr << "id: " << error_message(e) << "\n";
    return 1;
  }

  // Optional field: absence is not fatal.
  std::string_view desc;
  switch (auto e = doc["description"].get(desc)) {
    case SUCCESS:       std::cout << "desc=" << desc << "\n"; break;
    case NO_SUCH_FIELD: std::cout << "desc: (none)\n";        break;
    default:            std::cerr << error_message(e) << "\n"; return 1;
  }

  std::cout << "id=" << id << "\n";
  return 0;
}
```

## 3. Type-agnostic walk of arbitrary JSON

```cpp
#include <iostream>
#include "simdjson.h"
using namespace simdjson;

void print(ondemand::value v, int depth = 0) {
  std::string pad(depth * 2, ' ');
  switch (v.type()) {
    case json_type::object:
      for (auto f : v.get_object()) {
        std::cout << pad << f.unescaped_key() << ":\n";
        print(f.value(), depth + 1);
      }
      break;
    case json_type::array:
      for (auto e : v.get_array()) print(e.value(), depth + 1);
      break;
    case json_type::string:  std::cout << pad << '"' << std::string_view(v.get_string()) << "\"\n"; break;
    case json_type::number:  std::cout << pad << v.get_double() << "\n"; break;
    case json_type::boolean: std::cout << pad << (v.get_bool() ? "true" : "false") << "\n"; break;
    case json_type::null:    std::cout << pad << "null\n"; break;
  }
}

int main() {
  ondemand::parser parser;
  padded_string json = R"({"a":1,"b":[true,null,"x"],"c":{"d":2.5}})"_padded;
  ondemand::document doc = parser.iterate(json);
  print(doc.get_value());   // get_value() to treat the document root as a value
  return 0;
}
```

## 4. Load a file and extract nested data in document order

```cpp
#include <iostream>
#include <string>
#include <vector>
#include "simdjson.h"
using namespace simdjson;

int main(int argc, char** argv) {
  if (argc < 2) { std::cerr << "usage: prog <file.json>\n"; return 1; }

  ondemand::parser parser;
  padded_string json;
  if (auto e = padded_string::load(argv[1]).get(json)) {
    std::cerr << "cannot load: " << error_message(e) << "\n"; return 1;
  }

  ondemand::document doc = parser.iterate(json);

  // Access "meta" before "records" — same order as in the JSON.
  std::string_view version = doc["meta"]["version"].get_string();
  std::cout << "version=" << version << "\n";

  std::vector<std::string> names;                 // copies survive the parse
  for (auto rec : doc["records"].get_array())
    names.emplace_back(rec["name"].get_string().value());

  std::cout << names.size() << " records\n";
  return 0;
}
```

## 5. Streaming NDJSON (one document per line)

```cpp
#include <iostream>
#include "simdjson.h"
using namespace simdjson;

int main() {
  ondemand::parser parser;
  padded_string ndjson = R"({"id":1,"ok":true}
{"id":2,"ok":false}
{"id":3,"ok":true})"_padded;

  ondemand::document_stream stream = parser.iterate_many(ndjson);
  for (auto doc : stream) {
    int64_t id = doc["id"].get_int64();
    bool ok    = doc["ok"].get_bool();
    std::cout << "id=" << id << " ok=" << ok << "\n";
  }
  return 0;
}
```

## CMake for a standalone example

```cmake
cmake_minimum_required(VERSION 3.14)
project(simdjson_example CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson
  GIT_TAG v3.13.0)
FetchContent_MakeAvailable(simdjson)

add_executable(example main.cpp)
target_link_libraries(example PRIVATE simdjson::simdjson)
```
