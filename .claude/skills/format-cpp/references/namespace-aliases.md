# Namespace Alias Mapping

Use these predefined aliases when introducing namespace aliases (rule D2). Place alias declarations at the nearest enclosing scope where they're used.

## Standard Library

| Namespace | Alias | Example |
|-----------|-------|---------|
| `std` | `s` | `namespace s = std;` |
| `std::chrono` | `sc` | `namespace sc = std::chrono;` |
| `std::chrono_literals` | `sl` | `using namespace sl = std::chrono_literals;` |
| `std::filesystem` | `sf` | `namespace sf = std::filesystem;` |
| `std::ranges` | `sr` | `namespace sr = std::ranges;` |
| `std::views` | `sv` | `namespace sv = std::views;` |
| `std::placeholders` | `sp` | `namespace sp = std::placeholders;` |
| `std::literals` | `slt` | `namespace slt = std::literals;` |
| `std::regex_constants` | `src` | `namespace src = std::regex_constants;` |
| `std::this_thread` | `st` | `namespace st = std::this_thread;` |
| `std::execution` | `se` | `namespace se = std::execution;` |
| `std::pmr` | `spm` | `namespace spm = std::pmr;` |
| `std::numbers` | `sn` | `namespace sn = std::numbers;` |

## Boost

| Namespace | Alias | Example |
|-----------|-------|---------|
| `boost` | `b` | `namespace b = boost;` |
| `boost::asio` | `ba` | `namespace ba = boost::asio;` |
| `boost::asio::ip` | `bai` | `namespace bai = boost::asio::ip;` |
| `boost::beast` | `bb` | `namespace bb = boost::beast;` |
| `boost::beast::http` | `bbh` | `namespace bbh = boost::beast::http;` |
| `boost::beast::websocket` | `bbw` | `namespace bbw = boost::beast::websocket;` |
| `boost::filesystem` | `bf` | `namespace bf = boost::filesystem;` |
| `boost::program_options` | `bpo` | `namespace bpo = boost::program_options;` |
| `boost::spirit` | `bs` | `namespace bs = boost::spirit;` |
| `boost::spirit::qi` | `bsq` | `namespace bsq = boost::spirit::qi;` |
| `boost::multiprecision` | `bm` | `namespace bm = boost::multiprecision;` |
| `boost::hana` | `bh` | `namespace bh = boost::hana;` |
| `boost::json` | `bj` | `namespace bj = boost::json;` |
| `boost::log` | `bl` | `namespace bl = boost::log;` |

## Testing

| Namespace | Alias | Example |
|-----------|-------|---------|
| `testing` | `t` | `namespace t = testing;` |
| `Catch` | `c` | `namespace c = Catch;` |
| `doctest` | `dt` | `namespace dt = doctest;` |

## Third-Party

| Namespace | Alias | Example |
|-----------|-------|---------|
| `fmt` | `f` | `namespace f = fmt;` |
| `spdlog` | `lg` | `namespace lg = spdlog;` |
| `nlohmann` | `nl` | `namespace nl = nlohmann;` |
| `absl` | `a` | `namespace a = absl;` |
| `grpc` | `g` | `namespace g = grpc;` |
| `Eigen` | `e` | `namespace e = Eigen;` |
| `torch` | `tc` | `namespace tc = torch;` |
| `tensorflow` | `tf` | `namespace tf = tensorflow;` |
| `folly` | `fl` | `namespace fl = folly;` |
| `seastar` | `ss` | `namespace ss = seastar;` |
| `ranges` (range-v3) | `rv` | `namespace rv = ranges;` |
| `ranges::views` | `rvv` | `namespace rvv = ranges::views;` |
| `entt` | `en` | `namespace en = entt;` |
| `imgui` | `ig` | `namespace ig = imgui;` |
| `pybind11` | `py` | `namespace py = pybind11;` |
| `cxxopts` | `cx` | `namespace cx = cxxopts;` |

## Derivation Rules for Unlisted Namespaces

When a namespace is not in the table above, derive an alias using these rules in order:

1. **First letter**: Use the first letter of the namespace. `mylib` -> `m`
2. **First two letters**: If the single letter collides with an existing alias in the same file, use the first two letters. `mylib` -> `ml`
3. **Initials of compound names**: For `snake_case` or `camelCase` names, use initials. `my_library` -> `ml`, `gameEngine` -> `ge`
4. **Add trailing letter**: If initials still collide, append the next distinguishing consonant. `ml` -> `mlb`

**Collision resolution**: An alias collides if it matches any other namespace alias in the same file. Always check before introducing a new alias. Aliases must be 1-3 characters long.

**Scope**: Declare aliases at the narrowest scope where they're used. If only used in one function, declare inside that function. If used across multiple functions in a file, declare at the top of the file after includes.
