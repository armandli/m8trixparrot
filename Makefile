BUILD_DIR   ?= build
BUILD_TYPE  ?= Release
JOBS        ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
GENERATOR   ?=

CMAKE_FLAGS := -DCMAKE_BUILD_TYPE=$(BUILD_TYPE)
ifneq ($(GENERATOR),)
CMAKE_FLAGS += -G "$(GENERATOR)"
endif

.PHONY: all configure build clean rebuild run

all: build

configure: $(BUILD_DIR)/CMakeCache.txt

$(BUILD_DIR)/CMakeCache.txt: CMakeLists.txt src/CMakeLists.txt
	cmake -S . -B $(BUILD_DIR) $(CMAKE_FLAGS)

build: configure
	cmake --build $(BUILD_DIR) -j$(JOBS)

clean:
	rm -rf $(BUILD_DIR)

rebuild: clean build

# Run a built app directly, e.g. `make run APP=chat_tui`
APP ?= chat_tui
run: build
	$(BUILD_DIR)/$(APP)
