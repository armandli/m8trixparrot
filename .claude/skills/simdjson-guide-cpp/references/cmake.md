# simdjson Build Integration (CMake and alternatives)

simdjson is **not header-only**. Either link the CMake target or compile
`simdjson.cpp` from the amalgamation into your build.

Latest release at time of writing: **v4.6.5 (2026-07-29)**. Pin a release tag; `master`
is a development branch.

## 1. FetchContent (recommended, matches this repo)

Top-level `CMakeLists.txt`, alongside the existing `FetchContent_Declare` blocks:

```cmake
include(FetchContent)

FetchContent_Declare(
  simdjson
  GIT_REPOSITORY https://github.com/simdjson/simdjson
  GIT_TAG v4.6.5
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(simdjson)
```

Per-target `CMakeLists.txt`:

```cmake
add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE simdjson::simdjson)
```

Set options **before** `FetchContent_MakeAvailable`:

```cmake
set(SIMDJSON_DEVELOPMENT_CHECKS OFF CACHE INTERNAL "")
set(SIMDJSON_BUILD_STATIC_LIB   ON  CACHE INTERNAL "")
FetchContent_MakeAvailable(simdjson)
```

## 2. find_package (system / package-manager install)

```cmake
find_package(simdjson CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE simdjson::simdjson)
```

Install sources: vcpkg (`vcpkg install simdjson`), Conan (`simdjson/4.x`), Homebrew
(`brew install simdjson`), MSYS2, and distro packages (Debian, Ubuntu, Fedora,
RedHat, Rocky, Alpine).

## 3. Amalgamation (single header + single source)

Download `simdjson.h` and `simdjson.cpp` from the release assets, drop them in the
tree, and:

```cpp
#include "simdjson.h"
```

```bash
c++ -std=c++20 -O3 myproject.cpp simdjson.cpp -o myproject
```

```cmake
add_executable(my_app main.cpp third_party/simdjson/simdjson.cpp)
target_include_directories(my_app PRIVATE third_party/simdjson)
```

Do **not** compile `simdjson.cpp` with `-march=native` — see §6.

## 4. Exported targets

| Target | Meaning |
|--------|---------|
| `simdjson::simdjson` | Primary library (shared or static per `BUILD_SHARED_LIBS`) |
| `simdjson::simdjson_static` | Static library, present when `SIMDJSON_BUILD_STATIC_LIB=ON` |

## 5. User-facing CMake options

| Option | Default | Purpose |
|--------|---------|---------|
| `SIMDJSON_BUILD_STATIC_LIB` | OFF | Build a static lib alongside the shared one |
| `BUILD_SHARED_LIBS` | OFF | Build simdjson as a shared library |
| `SIMDJSON_ENABLE_THREADS` | ON | Link thread support (needed by `parse_many`/`iterate_many` threading) |
| `SIMDJSON_DEVELOPMENT_CHECKS` | OFF | Expensive asserts catching On-Demand misuse; use in debug only |
| `SIMDJSON_DEVELOPER_MODE` | OFF | Enable simdjson's own tests/benchmarks/tools |
| `SIMDJSON_STATIC_REFLECTION` | OFF | C++26 reflection support (experimental) |
| `SIMDJSON_DISABLE_DEPRECATED_API` | OFF | Compile-fail on deprecated APIs |
| `SIMDJSON_ENABLE_NAN_INF` | OFF | Accept `NaN` / `Infinity` in JSON (non-standard) |
| `SIMDJSON_MINUS_ZERO_AS_FLOAT` | OFF | Treat `-0` as a floating-point value |
| `SIMDJSON_ENABLE_MEMORY_FILE_MAPPING_ON_WINDOWS` | OFF | `padded_memory_map` on Windows 10.1803+ |
| `SIMDJSON_SINGLEHEADER` | ON | Generate the amalgamated header |
| `SIMDJSON_INSTALL` | ON at top level | Emit the install target |
| `SIMDJSON_AVX512_ALLOWED` | ON | Set OFF to avoid AVX-512 kernels (downclocking concerns) |

Exception control is a preprocessor switch, not just CMake:
`-DSIMDJSON_DISABLE_EXCEPTIONS=ON` (or define the macro before including the header)
makes every API error-code only.

## 6. Compiler and flags

- **Language standard**: C++11 minimum. C++17 for `padded_input` and structured
  bindings, **C++20 for the builder's template keys, automatic containers, and
  `tag_invoke`**, C++26 for reflection. This repo is C++20 — that is the right target.
- **Compilers**: LLVM Clang 6+, GCC 7.4+, Xcode 11+, Visual Studio 2017+. On Windows
  prefer clang-cl over MSVC for speed.
- **Release builds**: define `NDEBUG` and use `-O3` (`/O2 /Ob2` on MSVC). `NDEBUG` is
  separate from the optimizer flag and matters on its own.
- **Never `-march=native`.** simdjson dispatches to the best SIMD kernel at runtime;
  `-march=native` both defeats that and makes binaries non-portable.

## 7. Verifying the integration

```bash
cmake -S . -B build && cmake --build build -j
```

Then run a program that prints the active kernel — this proves both link and dispatch:

```cpp
#include <iostream>
#include "simdjson.h"
int main() {
  std::cout << "simdjson " << SIMDJSON_VERSION
            << " kernel: " << simdjson::get_active_implementation()->name() << "\n";
}
```

## 8. Troubleshooting

| Symptom | Cause and fix |
|---------|---------------|
| `undefined reference to simdjson::...` | Target not linked. Add `simdjson::simdjson` to `target_link_libraries`, or add `simdjson.cpp` to the sources. |
| `simdjson.h: No such file or directory` | `FetchContent_MakeAvailable` missing, or include dirs not propagated — link the target rather than adding paths by hand. |
| `simdjson::builder` / `to_json` not found | Pinned tag predates v4. Bump `GIT_TAG`. |
| `append_key_value<"k">` fails to compile | Needs C++20. Check `CMAKE_CXX_STANDARD`. |
| `SIMDJSON_STATIC_REFLECTION` errors | Compiler lacks C++26 reflection; use `tag_invoke` instead. |
| Illegal instruction at runtime | Built with `-march=native` on a different machine. Remove it. |
| Field lookups return defaults / wrong values, and the behaviour changes with link order | **Mixed compile flags across translation units.** `ondemand::object`/`value` are header-inlined into every TU that includes `simdjson.h`; TUs built with different flags (`-O2` vs `-O3`, `-std=c++20` vs `-std=gnu++20`, `NDEBUG` on/off) emit different COMDAT copies, and the linker picks one arbitrarily — an ODR violation that silently corrupts on-demand cursors passed across TU boundaries. Build every TU that touches simdjson with identical flags (i.e. let one CMake config own them all). This bites hardest in ad-hoc `g++` commands linking a CMake-built archive. |
| FetchContent re-clones every configure | Add `GIT_SHALLOW TRUE` and pin an immutable tag. |
| simdjson's own tests get built | Ensure `SIMDJSON_DEVELOPER_MODE` is OFF (the default for subprojects). |
