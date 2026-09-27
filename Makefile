BUILD_DIR   ?= build
BUILD_TYPE  ?= Release
# Where `make install` puts m8 and the tool_* commands. /usr/local needs sudo;
# `make install PREFIX=~/.local` does not.
PREFIX      ?= /usr/local
JOBS        ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
GENERATOR   ?=

# The ThreadSanitizer build lives in its own tree: TSan is a whole-program
# instrumentation, so sharing build/ would mean rebuilding everything each time
# you switch.
TSAN_DIR    ?= build-tsan

CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DCMAKE_INSTALL_PREFIX=$(PREFIX)
ifneq ($(GENERATOR),)
CMAKE_FLAGS += -G "$(GENERATOR)"
endif

.PHONY: all configure build clean rebuild run test integration-test install tsan tsan-test

all: build

configure: $(BUILD_DIR)/CMakeCache.txt

CMAKE_FILES := $(shell find CMakeLists.txt src test -name CMakeLists.txt 2>/dev/null)

$(BUILD_DIR)/CMakeCache.txt: $(CMAKE_FILES)
	cmake -S . -B $(BUILD_DIR) $(CMAKE_FLAGS)

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

# The default suite is hermetic. The `integration` label covers the live tests
# that need a running Ollama (see test/integration/); run those with
# `make integration-test`.
test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -LE integration

integration-test: build
	ctest --test-dir $(BUILD_DIR) --output-on-failure -L integration

# vdb holds three locks per store (a structure shared_mutex, 256 striped per-key
# shared_mutexes, and a log mutex) with a required acquisition order. Nothing but
# ThreadSanitizer will catch an inversion, so the concurrency tests get a build
# that can see one. Not part of `make test`: a TSan run is several times slower.
tsan:
	cmake -S . -B $(TSAN_DIR) -DCMAKE_BUILD_TYPE=RelWithDebInfo \
	  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
	  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
	cmake --build $(TSAN_DIR) -j$(JOBS) --target core_tests

tsan-test: tsan
	ctest --test-dir $(TSAN_DIR) --output-on-failure \
	  -R "VectorStoreTest|MemoryStoreTest|AgentPoolTest"

install: build
	cmake --install $(BUILD_DIR)

clean:
	rm -rf $(BUILD_DIR) $(TSAN_DIR)

rebuild: clean build

# Run a built app directly, e.g. `make run APP=chat_tui`
APP ?= chat_tui
run: build
	$(BUILD_DIR)/$(APP)
