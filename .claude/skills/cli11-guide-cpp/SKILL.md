---
name: cli11-guide-cpp
description: Expert reference guide for the CLI11 C++ command-line argument parsing library (repo https://github.com/CLIUtils/CLI11). Auto-activates whenever writing or modifying a command-line argument parser in C++ — parsing argc/argv, adding options, flags, positional arguments, subcommands, validators, or config-file support. Use when creating a new C++ executable's main() that takes arguments, adding a CLI to an existing C++ tool, wiring CLI11 into CMake (FetchContent / find_package / single-header), or debugging CLI11 parse errors, required/default/validator behavior, or subcommand dispatch. Do NOT use for Python argparse/click, other C++ arg libraries (getopt, Boost.Program_options, gflags, p-ranav/argparse, Lyra), or general config-file parsing unrelated to CLI11.
---

# CLI11 C++ Command-Line Parsing Guide

CLI11 is a header-only C++11 library for parsing command-line arguments. It supports options, flags, positional arguments, subcommands, validators, config files (INI/TOML), and environment variables, with clean error messages and auto-generated `--help`.

Upstream: https://github.com/CLIUtils/CLI11 · Single include: `<CLI/CLI.hpp>`

## When writing a new parser, do this

1. Ensure CLI11 is available to the build (see **CMake Setup**). In this repo it is already fetched — link `CLI11::CLI11` and `#include <CLI/CLI.hpp>`.
2. Create one `CLI::App` with a description string.
3. Declare a plain C++ variable for each argument, then bind it with `add_option` / `add_flag`.
4. Apply modifiers (`->required()`, `->capture_default_str()`, `->check(...)`) by chaining.
5. Parse with the `CLI11_PARSE(app, argc, argv)` macro.
6. Build and run with `--help` and a real invocation to verify (see **Testing**).

## CMake Setup

**Preferred in this repo: FetchContent** (top-level `CMakeLists.txt` already contains this — reuse it, do not duplicate):

```cmake
include(FetchContent)
FetchContent_Declare(
  cli11
  GIT_REPOSITORY https://github.com/CLIUtils/CLI11
  GIT_TAG v2.6.2          # pin a release tag
)
FetchContent_MakeAvailable(cli11)
```

Then in the executable's `CMakeLists.txt`:

```cmake
target_link_libraries(my_app PRIVATE CLI11::CLI11)
```

Alternatives (only if not using FetchContent):
- **find_package**: `find_package(CLI11 CONFIG REQUIRED)` then link `CLI11::CLI11` (needs CLI11 installed system-wide).
- **Single header**: download `CLI11.hpp` from the release page into the project and `#include` it directly — no CMake target needed.

## Minimal Skeleton (matches this repo's conventions)

```cpp
#include <string>
#include <CLI/CLI.hpp>

int main(int argc, char** argv) {
  CLI::App app{"myapp - one-line description of the tool"};

  std::string model = "ornith:35b";   // default lives in the variable
  std::string out_file;

  // "name,-m,--model" => positional name PLUS short/long flags, all one option.
  app.add_option("model,-m,--model", model, "Model to use")
      ->capture_default_str();         // shows the default in --help
  app.add_option("out_file,-o,--out", out_file, "Where to write output");

  CLI11_PARSE(app, argc, argv);        // handles --help and errors, exits on failure

  // ... use `model` and `out_file` ...
  return 0;
}
```

Key idea: **CLI11 binds directly to your variables.** After `CLI11_PARSE` returns, the variables hold the parsed values (or their defaults). No map lookups.

## Core API Cheat Sheet

| Goal | Call |
|------|------|
| Option with value | `app.add_option("-f,--file", var, "help")` |
| Positional argument | `app.add_option("filename", var, "help")` (no leading dash) |
| Positional + flags in one | `app.add_option("name,-n,--name", var, "help")` |
| Boolean flag | `app.add_flag("-v,--verbose", bool_var, "help")` |
| Counting flag (`-vvv`) | `app.add_flag("-v,--verbose", int_var, "help")` |
| Flag with negation | `app.add_flag("--color,!--no-color", bool_var, "help")` |
| Collect multiple values | `add_option` bound to a `std::vector<T>` |
| Make required | `->required()` |
| Show default in help | `->capture_default_str()` |
| Restrict to a set | `->check(CLI::IsMember({"a","b","c"}))` |
| File must exist | `->check(CLI::ExistingFile)` |
| Numeric range | `->check(CLI::Range(0, 100))` |
| Read from env var | `->envname("MYAPP_TOKEN")` |
| Add a version flag | `app.set_version_flag("--version", "1.2.3")` |
| Subcommand | `auto* sub = app.add_subcommand("run", "desc")` |
| Require one subcommand | `app.require_subcommand(1)` |

Modifiers chain and return `CLI::Option*`, so `->required()->check(...)->envname(...)` all compose.

## Subcommands (git-style CLIs)

```cpp
CLI::App app{"tool"};
app.require_subcommand(1);            // exactly one subcommand must be given

auto* add = app.add_subcommand("add", "Add an item");
std::string name;
add->add_option("name", name, "Item name")->required();

auto* list = app.add_subcommand("list", "List items");
bool verbose = false;
list->add_flag("-v,--verbose", verbose);

CLI11_PARSE(app, argc, argv);

if (*add)  { /* add->parsed() is true; use `name` */ }
if (*list) { /* use `verbose` */ }
```

Prefer subcommand **callbacks** for larger tools: `add->callback([&]{ do_add(name); });` — CLI11 invokes it after a successful parse.

## Parse: macro vs. manual

`CLI11_PARSE(app, argc, argv)` is the easy path: it prints help/errors and calls `return app.exit(e)` on failure. Use manual parsing when you need to run cleanup or keep control of the exit code:

```cpp
try {
  app.parse(argc, argv);
} catch (const CLI::ParseError& e) {
  return app.exit(e);   // prints message + returns proper exit code
}
```

## Common Gotchas

- **`--help` and version exit via an exception.** With manual `parse`, `CLI::CallForHelp` / `CLI::CallForVersion` are thrown and handled by `app.exit()`. Don't treat them as errors — `app.exit()` returns 0 for them.
- **Defaults belong in the variable**, not a separate call. Add `->capture_default_str()` so the default appears in `--help`; otherwise CLI11 doesn't know to print it.
- **Unrecognized args are an error** by default. Call `app.allow_extras()` to collect leftovers instead of failing.
- **Options are single-value by default.** To accept multiple, bind a `std::vector<T>`, or set `->expected(min, max)`.
- **Positional-name-plus-flags** (`"model,-m,--model"`) makes the value settable positionally *or* by flag — that is the pattern used in this repo's `chat_tui`.
- **`->required()` on a positional** makes omitting it an error with a clear message; don't hand-roll the check.

## Testing (always do this after adding a parser)

Build the target, then exercise the parser end-to-end — do not stop at compilation:

1. `--help`: run `./my_app --help` and confirm every option/positional/subcommand and its default is listed.
2. **Happy path**: run with a realistic full invocation and confirm the variables took the expected values (add a temporary print or a debugger check if needed).
3. **Error path**: pass a bad value (missing required arg, out-of-range number, unknown flag) and confirm CLI11 prints a clear message and the process exits non-zero (`echo $?`).
4. If subcommands exist, run at least two different subcommands and the no-subcommand case.

In this repo, build via `make build` (or `cmake --build build -j`) and run the produced binary from `build/`.

## Additional Resources

- Full option/validator/config/formatter API: [references/api-reference.md](references/api-reference.md)
- Complete compilable example programs: [references/examples.md](references/examples.md)

### Final Step — Record Usage

After helping create or modify a CLI11 parser, run:

```bash
python3 ${PWD}/.claude/skills/skill-stat/scripts/record-stat.py "cli11-guide-cpp"
```
