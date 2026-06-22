SHELL := /usr/bin/env bash

CMAKE ?= cmake
CTEST ?= ctest
STRIP ?= strip

BUILD_DIR ?= build
RELEASE_BUILD_DIR ?= build-release
TSAN_BUILD_DIR ?= build-tsan
BUILD_PARALLEL ?= --parallel

COMMON_CONFIGURE_FLAGS ?= -DPI_CPP_OTEL_API=OFF
DEV_CONFIGURE_FLAGS ?= -DCMAKE_BUILD_TYPE=Debug
RELEASE_CONFIGURE_FLAGS ?= -DCMAKE_BUILD_TYPE=Release
TSAN_CONFIGURE_FLAGS ?= -DCMAKE_BUILD_TYPE=Debug \
	-DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
	-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"

.PHONY: help configure dev release strip-release lint format format-check test tsan clean clean-release clean-tsan

help:
	@printf '%s\n' \
		'Targets:' \
		'  make dev          Configure and build the dev CLI in $(BUILD_DIR)' \
		'  make release      Configure and build the release CLI in $(RELEASE_BUILD_DIR)' \
		'  make strip-release Strip symbols from $(RELEASE_BUILD_DIR)/pi-cli' \
		'  make lint         Run the CMake clang-tidy target' \
		'  make format       Run the CMake clang-format target' \
		'  make format-check Check formatting without editing files' \
		'  make test         Build the dev tree and run ctest' \
		'  make tsan         Build with ThreadSanitizer in $(TSAN_BUILD_DIR) and run ctest' \
		'  make clean        Remove $(BUILD_DIR)' \
		'  make clean-release Remove $(RELEASE_BUILD_DIR)' \
		'  make clean-tsan   Remove $(TSAN_BUILD_DIR)' \
		'' \
		'Variables:' \
		'  BUILD_DIR=build-dev RELEASE_BUILD_DIR=build-release TSAN_BUILD_DIR=build-tsan BUILD_PARALLEL=--parallel'

configure:
	$(CMAKE) -B $(BUILD_DIR) $(COMMON_CONFIGURE_FLAGS) $(DEV_CONFIGURE_FLAGS)

dev: configure
	$(CMAKE) --build $(BUILD_DIR) --target pi-cli $(BUILD_PARALLEL)

release:
	$(CMAKE) -B $(RELEASE_BUILD_DIR) $(COMMON_CONFIGURE_FLAGS) $(RELEASE_CONFIGURE_FLAGS)
	$(CMAKE) --build $(RELEASE_BUILD_DIR) --target pi-cli $(BUILD_PARALLEL)

strip-release: release
	$(STRIP) --strip-all $(RELEASE_BUILD_DIR)/pi-cli

lint: configure
	$(CMAKE) --build $(BUILD_DIR) --target tidy $(BUILD_PARALLEL)

format: configure
	$(CMAKE) --build $(BUILD_DIR) --target format

format-check: configure
	$(CMAKE) --build $(BUILD_DIR) --target format-check

test: configure
	$(CMAKE) --build $(BUILD_DIR) $(BUILD_PARALLEL)
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure

# ThreadSanitizer build: catches data races (e.g. concurrent mutation of
# shared state across worker threads) that ordinary tests won't surface.
# Uses TSAN_OPTIONS=halt_on_error=1 so CI fails fast on the first race
# instead of drowning in duplicate reports from the same root cause.
# Run under `setarch -R` (disable ASLR): some kernels map memory in a way
# TSan's runtime doesn't expect, causing a spurious
# "FATAL: ThreadSanitizer: unexpected memory mapping" abort with ASLR on.
tsan:
	$(CMAKE) -B $(TSAN_BUILD_DIR) $(COMMON_CONFIGURE_FLAGS) $(TSAN_CONFIGURE_FLAGS)
	$(CMAKE) --build $(TSAN_BUILD_DIR) $(BUILD_PARALLEL)
	setarch $$(uname -m) -R env TSAN_OPTIONS="halt_on_error=1" \
		$(CTEST) --test-dir $(TSAN_BUILD_DIR) --output-on-failure

clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR)

clean-release:
	$(CMAKE) -E rm -rf $(RELEASE_BUILD_DIR)

clean-tsan:
	$(CMAKE) -E rm -rf $(TSAN_BUILD_DIR)
