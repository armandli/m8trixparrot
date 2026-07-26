# m8trixparrot

Experiments in building AI agents in C++ on top of local Ollama models,
with terminal UIs built on [FTXUI](https://github.com/ArthurSonzogni/FTXUI).

## Layout

```
.
├── CMakeLists.txt          top-level build: fetches deps, adds src/
├── Makefile                convenience wrapper around cmake/make
└── src/
    ├── core/                shared library (agentcore): Ollama HTTP client, etc.
    │   ├── ollama_client.hpp/.cpp
    │   └── CMakeLists.txt
    └── apps/                one subdirectory per agent experiment
        ├── CMakeLists.txt   registers each experiment
        └── chat_tui/        first experiment: minimal FTXUI chat against Ollama
            ├── main.cpp
            └── CMakeLists.txt
```

Every experiment is its own executable target under `src/apps/<name>/`,
linked against the shared `agentcore` library. All built executables are
placed directly in `build/` (e.g. `build/chat_tui`) regardless of how deep
their source lives, so they're easy to find and run.

Communication with Ollama happens over HTTP via libcurl
(`src/core/ollama_client.{hpp,cpp}`), talking to Ollama's REST API
(`/api/chat`) with JSON bodies parsed via
[nlohmann/json](https://github.com/nlohmann/json).

## Adding a new experiment

1. Create `src/apps/<name>/` with a `main.cpp` and a `CMakeLists.txt`
   (copy `src/apps/chat_tui/CMakeLists.txt` as a starting point).
2. Add `add_subdirectory(<name>)` to `src/apps/CMakeLists.txt`.
3. `make build` — the new binary appears at `build/<name>`.

## Build

Dependencies: a C++20 compiler, CMake, and `libcurl` (system-provided on
macOS/Linux). FTXUI, nlohmann/json, and CLI11 are fetched automatically by
CMake via `FetchContent`.

```sh
make build       # configure (if needed) + build everything into build/
make run APP=chat_tui   # build, then run a specific app
make clean        # remove the build directory
make rebuild       # clean + build
```

Or drive CMake directly:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Running chat_tui

Requires a running [Ollama](https://ollama.com) server with a pulled model:

```sh
ollama pull llama3.2
build/chat_tui llama3.2                       # positional: model name
build/chat_tui --model llama3.2 --history /tmp/chat.txt  # named flags
build/chat_tui --help                         # list all options
```

Type a message and press Enter to send it; type `/quit` to exit.
