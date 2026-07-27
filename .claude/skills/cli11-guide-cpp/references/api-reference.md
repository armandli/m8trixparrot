# CLI11 API Reference

Deep reference for CLI11 (v2.x, tested against v2.6.2). Load this when the task
goes beyond the basic add_option/add_flag/parse flow in SKILL.md. All snippets
assume `#include <CLI/CLI.hpp>` and a `CLI::App app{...}`.

## The App object

```cpp
CLI::App app{"description string", "app_name"};   // both args optional
```

Useful `App` methods:

| Method | Effect |
|--------|--------|
| `app.require_subcommand(min, max)` | Require between min and max subcommands. `require_subcommand(1)` = exactly one. |
| `app.require_option(min, max)` | Require a number of options be present. |
| `app.allow_extras(true)` | Don't error on unrecognized tokens; collect them via `app.remaining()`. |
| `app.positionals_at_end(true)` | Positionals must come after all options. |
| `app.prefix_command()` | Stop parsing at the first unrecognized token (pass-through to a wrapped command). |
| `app.set_help_flag("--help", "help text")` | Rename/replace the help flag. |
| `app.set_help_all_flag("--help-all")` | Add a flag that expands help for all subcommands. |
| `app.set_version_flag("--version", "1.2.3")` | Add a version flag; a string or a callback returning a string. |
| `app.set_config("--config", "default.ini", "help", required)` | Enable config-file loading (see below). |
| `app.failure_message(CLI::FailureMessage::help)` | Print full help on any parse failure. |
| `app.get_formatter()` | Access/replace the help formatter. |
| `app.name("newname")` | Set the (sub)command name programmatically. |

## Options

`add_option` returns `CLI::Option*`. The name string may combine a positional
name and any number of short/long flags, comma-separated:

```cpp
app.add_option("-f,--file", path, "Input file");     // flag only
app.add_option("count", n, "How many");              // positional only
app.add_option("file,-f,--file", path, "Input");     // positional OR flag
```

Bind to a `std::vector<T>` to accept multiple values:

```cpp
std::vector<std::string> includes;
app.add_option("-I,--include", includes, "Include dirs (repeatable)");
```

### Option modifiers (chainable)

| Modifier | Meaning |
|----------|---------|
| `->required()` | Option must be provided. |
| `->capture_default_str()` | Record the variable's current value as the printed default. |
| `->default_val(v)` | Set a default value (and capture its string form). |
| `->expected(n)` / `->expected(min, max)` | Exact / ranged number of values consumed. |
| `->take_first()` / `->take_last()` / `->take_all()` | Which occurrence wins when repeated. |
| `->allow_extra_args()` | A vector option may consume multiple tokens greedily. |
| `->delimiter(',')` | Split a single value on a delimiter (e.g. `-x 1,2,3`). |
| `->each([](std::string s){...})` | Callback per value as parsed. |
| `->envname("VAR")` | Fall back to an environment variable. |
| `->group("Group Name")` | Group in help output; `->group("")` hides it. |
| `->needs(other_opt)` | Requires another option also be set. |
| `->excludes(other_opt)` | Mutually exclusive with another option. |
| `->check(Validator)` | Validate the value (see Validators). |
| `->transform(Validator)` | Validate AND rewrite the value. |
| `->ignore_case()` | Case-insensitive matching (esp. with `IsMember`). |
| `->ignore_underscore()` | Treat `_` and `-` as equivalent in values. |

### Flags

```cpp
bool verbose = false;
app.add_flag("-v,--verbose", verbose, "Enable verbose output");

int level = 0;
app.add_flag("-v,--verbose", level, "Repeat for more (-vvv)");   // counts

bool color = true;
app.add_flag("--color,!--no-color", color, "Toggle color");       // ! negates

// Flag with an explicit value form: --flag=3
int val = 1;
app.add_flag("--set{3}", val, "Sets val to 3 when present");
```

Callback flavors when you need logic instead of a bound variable:

```cpp
app.add_flag_callback("--stop", []{ /* ... */ }, "Run on flag");
app.add_option_function<int>("--num", [](const int& v){ /* ... */ }, "help");
```

## Validators

Pass to `->check(...)` (validate only) or `->transform(...)` (validate + modify):

| Validator | Checks |
|-----------|--------|
| `CLI::ExistingFile` | Path exists and is a file. |
| `CLI::ExistingDirectory` | Path exists and is a directory. |
| `CLI::ExistingPath` | Path exists (file or dir). |
| `CLI::NonexistentPath` | Path does not exist. |
| `CLI::Range(min, max)` | Number within inclusive range. |
| `CLI::Bound(min, max)` | Clamp value into range (use with `transform`). |
| `CLI::PositiveNumber` / `CLI::NonNegativeNumber` | Sign checks. |
| `CLI::Number` | Parses as a number. |
| `CLI::IsMember({"a","b"})` | Value is one of a set (works with maps to transform to enum). |
| `CLI::ValidIPV4` | Valid IPv4 address. |

Map an input string to an enum value:

```cpp
enum class Level { Low, Mid, High };
Level lvl{Level::Mid};
std::map<std::string, Level> m{{"low",Level::Low},{"mid",Level::Mid},{"high",Level::High}};
app.add_option("--level", lvl, "Level")
   ->transform(CLI::CheckedTransformer(m, CLI::ignore_case));
```

Custom validator:

```cpp
CLI::Validator even{[](std::string& s){
  return (std::stoi(s) % 2 == 0) ? std::string{} : std::string{"must be even"};
}, "EVEN"};
app.add_option("-n", n)->check(even);
```

## Subcommands

```cpp
auto* sub = app.add_subcommand("name", "description");
sub->alias("nm");                     // alternate name
sub->require_option();                // subcommand must get at least one option
sub->fallthrough();                   // options after the subcommand may bind to parent
sub->callback([&]{ /* run after successful parse of this subcommand */ });
```

Detect what was chosen after parsing:

```cpp
if (*sub) { ... }                     // operator bool == was this subcommand parsed
if (app.got_subcommand("name")) { ... }
for (auto* s : app.get_subcommands()) { ... }
```

Option groups (non-command grouping / mutual-exclusion sets):

```cpp
auto* grp = app.add_option_group("Mode");
grp->require_option(1);               // exactly one of the following:
grp->add_flag("--fast", fast);
grp->add_flag("--slow", slow);
```

## Config files

```cpp
app.set_config("--config", "app.ini", "Read a config file", /*required=*/false);
```

- Long-option names become keys; subcommands become `[section]` headers.
- CLI11 supports INI and TOML syntax. Command-line values override config values.
- Dump the effective config with `app.config_to_str(default_also=true)`.

## Manual parsing and exit codes

```cpp
try {
  app.parse(argc, argv);
} catch (const CLI::ParseError& e) {
  return app.exit(e);   // prints help for --help/--version (exit 0) or error (non-zero)
}
```

Exception hierarchy of note: `CLI::ParseError` (base), `CLI::CallForHelp`,
`CLI::CallForAllHelp`, `CLI::CallForVersion`, `CLI::RequiredError`,
`CLI::ValidationError`, `CLI::ConversionError`, `CLI::ExtrasError`. `app.exit(e)`
maps each to the right stream and code, so prefer it over bespoke handling.

## Formatting help

```cpp
app.get_formatter()->column_width(40);
app.get_formatter()->label("REQUIRED", "(required)");
// Or subclass CLI::Formatter and app.formatter(std::make_shared<MyFormatter>());
```
