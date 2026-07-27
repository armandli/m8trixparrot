# CLI11 Complete Examples

Each example is a self-contained `main.cpp` that compiles against CLI11 v2.x.
Link `CLI11::CLI11` (or use the single header). Build in this repo with
`make build` and run the binary from `build/`.

## 1. Simple tool: options, positional, flag

```cpp
#include <iostream>
#include <string>
#include <CLI/CLI.hpp>

int main(int argc, char** argv) {
  CLI::App app{"greet - print a greeting"};

  std::string name;                       // required positional
  std::string greeting = "Hello";         // optional, has default
  int times = 1;
  bool shout = false;

  app.add_option("name", name, "Who to greet")->required();
  app.add_option("-g,--greeting", greeting, "Greeting word")->capture_default_str();
  app.add_option("-n,--times", times, "Repeat count")
      ->capture_default_str()
      ->check(CLI::PositiveNumber);
  app.add_flag("-s,--shout", shout, "Uppercase the output");

  CLI11_PARSE(app, argc, argv);

  std::string line = greeting + ", " + name + "!";
  if (shout) for (char& c : line) c = std::toupper(static_cast<unsigned char>(c));
  for (int i = 0; i < times; ++i) std::cout << line << "\n";
  return 0;
}
```

Try: `greet World`, `greet World -n 3 -s`, `greet` (errors: required), `greet X -n -2` (errors: PositiveNumber).

## 2. Repeated values, sets, and env fallback

```cpp
#include <iostream>
#include <string>
#include <vector>
#include <CLI/CLI.hpp>

int main(int argc, char** argv) {
  CLI::App app{"build - a mock compiler frontend"};

  std::vector<std::string> sources;               // one or more positionals
  std::vector<std::string> includes;              // repeatable -I
  std::string opt = "O2";
  std::string token;

  app.add_option("sources", sources, "Source files")->required()
      ->check(CLI::ExistingFile);
  app.add_option("-I,--include", includes, "Include dir (repeatable)");
  app.add_option("--opt", opt, "Optimization level")
      ->check(CLI::IsMember({"O0", "O1", "O2", "O3"}))
      ->capture_default_str();
  app.add_option("--token", token, "Auth token")->envname("BUILD_TOKEN");

  CLI11_PARSE(app, argc, argv);

  std::cout << "opt=" << opt << " sources=" << sources.size()
            << " includes=" << includes.size()
            << " token_set=" << (!token.empty()) << "\n";
  return 0;
}
```

Try: `build a.cpp b.cpp -I inc1 -I inc2 --opt O3`, `BUILD_TOKEN=xyz build a.cpp`.

## 3. Git-style subcommands with callbacks

```cpp
#include <iostream>
#include <string>
#include <CLI/CLI.hpp>

int main(int argc, char** argv) {
  CLI::App app{"notes - a tiny note manager"};
  app.require_subcommand(1);

  // add
  std::string text;
  auto* add = app.add_subcommand("add", "Add a note");
  add->add_option("text", text, "Note text")->required();
  add->callback([&]{ std::cout << "added: " << text << "\n"; });

  // list
  bool verbose = false;
  auto* list = app.add_subcommand("list", "List notes");
  list->add_flag("-v,--verbose", verbose, "Show details");
  list->callback([&]{ std::cout << "listing (verbose=" << verbose << ")\n"; });

  // remove
  int id = 0;
  auto* rm = app.add_subcommand("remove", "Remove a note by id");
  rm->add_option("id", id, "Note id")->required()->check(CLI::NonNegativeNumber);
  rm->callback([&]{ std::cout << "removed: " << id << "\n"; });

  CLI11_PARSE(app, argc, argv);   // callbacks fire here, in order
  return 0;
}
```

Try: `notes add "buy milk"`, `notes list -v`, `notes remove 4`, `notes` (errors: needs a subcommand).

## 4. Manual parse with custom exit handling

```cpp
#include <iostream>
#include <CLI/CLI.hpp>

int main(int argc, char** argv) {
  CLI::App app{"job"};
  int workers = 4;
  app.add_option("-j,--jobs", workers, "Worker count")
      ->capture_default_str()->check(CLI::Range(1, 64));

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    // Do any cleanup here, then defer to CLI11 for message + exit code.
    return app.exit(e);           // --help/--version return 0; real errors non-zero
  }

  std::cout << "running with " << workers << " workers\n";
  return 0;
}
```

## 5. Mutually exclusive modes via an option group

```cpp
#include <iostream>
#include <CLI/CLI.hpp>

int main(int argc, char** argv) {
  CLI::App app{"mode-demo"};

  bool fast = false, safe = false;
  auto* mode = app.add_option_group("mode", "Pick exactly one mode");
  mode->add_flag("--fast", fast, "Prioritize speed");
  mode->add_flag("--safe", safe, "Prioritize correctness");
  mode->require_option(1);        // exactly one of --fast/--safe

  CLI11_PARSE(app, argc, argv);
  std::cout << (fast ? "fast\n" : "safe\n");
  return 0;
}
```

Try: `mode-demo --fast`, `mode-demo` (errors: requires one), `mode-demo --fast --safe` (errors: at most one).

## CMake for a standalone example

```cmake
cmake_minimum_required(VERSION 3.14)
project(cli11_example CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11
  GIT_TAG v2.6.2)
FetchContent_MakeAvailable(cli11)

add_executable(example main.cpp)
target_link_libraries(example PRIVATE CLI11::CLI11)
```
