SHELL := /usr/bin/env bash

CMAKE ?= cmake
CTEST ?= ctest
STRIP ?= strip

BUILD_DIR ?= build
RELEASE_BUILD_DIR ?= build-release
RELEASE_JEMALLOC_BUILD_DIR ?= build-release-jemalloc
TSAN_BUILD_DIR ?= build-tsan
ASAN_BUILD_DIR ?= build-asan
BUILD_PARALLEL ?= --parallel
# clang-tidy: TIDY_JOBS=0 picks a bounded automatic count (see
# docs/cpp-safety-tooling.md); TIDY_STRICT=ON fails on any warning.
TIDY_JOBS ?= 0
TIDY_STRICT ?= OFF
TIDY_CONFIGURE_FLAGS ?= -DPI_CPP_TIDY_JOBS=$(TIDY_JOBS) -DPI_CPP_TIDY_STRICT=$(TIDY_STRICT)

COMMON_CONFIGURE_FLAGS ?= -DPI_CPP_OTEL_API=OFF
DEV_CONFIGURE_FLAGS ?= -DCMAKE_BUILD_TYPE=Debug
RELEASE_CONFIGURE_FLAGS ?= -DCMAKE_BUILD_TYPE=Release
RELEASE_JEMALLOC_CONFIGURE_FLAGS ?= -DCMAKE_BUILD_TYPE=Release \
	-DPI_CPP_MEMSTATS=ON \
	-DPI_CPP_MEMSTATS_LINK_JEMALLOC=ON
TSAN_CONFIGURE_FLAGS ?= -DCMAKE_BUILD_TYPE=Debug \
	-DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer" \
	-DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread"
ASAN_CONFIGURE_FLAGS ?= -DCMAKE_BUILD_TYPE=Debug -DPI_CPP_ASAN_UBSAN=ON

.PHONY: help safety configure dev release release-jemalloc strip-release lint format format-check test tsan asan check tidy-fix clean clean-release clean-release-jemalloc clean-tsan clean-asan

help:
	@printf '%s\n' \
		'Targets:' \
		'  make dev          Configure and build pi-cli + pi-acp in $(BUILD_DIR)' \
		'  make release      Configure and build pi-cli + pi-acp in $(RELEASE_BUILD_DIR)' \
		'  make release-jemalloc Configure and build a Release tree with jemalloc memory stats in $(RELEASE_JEMALLOC_BUILD_DIR)' \
		'  make strip-release Strip symbols from $(RELEASE_BUILD_DIR)/{pi-cli,pi-acp}' \
		'  make lint         Run clang-tidy (TIDY_JOBS=N, TIDY_STRICT=ON)' \
		'  make tidy-fix     Apply clang-tidy fixes (serial by default; review first)' \
		'  make format       Run the CMake clang-format target' \
		'  make format-check Check formatting without editing files' \
		'  make test         Build the dev tree and run ctest' \
		'  make tsan         Build with ThreadSanitizer in $(TSAN_BUILD_DIR) and run ctest' \
		'  make asan         Build with ASan+UBSan in $(ASAN_BUILD_DIR) and run ctest' \
		'  make check        Run test + lint + tsan (CI / pre-push gate)' \
		'  make safety       Run test + lint + asan (C++ safety gate; single CI command)' \
		'  make clean        Remove $(BUILD_DIR)' \
		'  make clean-release Remove $(RELEASE_BUILD_DIR)' \
		'  make clean-tsan   Remove $(TSAN_BUILD_DIR)' \
		'  make clean-asan   Remove $(ASAN_BUILD_DIR)' \
		'' \
		'Variables:' \
		'  BUILD_DIR=build-dev RELEASE_BUILD_DIR=build-release RELEASE_JEMALLOC_BUILD_DIR=build-release-jemalloc TSAN_BUILD_DIR=build-tsan BUILD_PARALLEL=--parallel'

configure:
	$(CMAKE) -B $(BUILD_DIR) $(COMMON_CONFIGURE_FLAGS) $(DEV_CONFIGURE_FLAGS)

dev: configure
	$(CMAKE) --build $(BUILD_DIR) --target pi-cli pi-acp $(BUILD_PARALLEL)

release:
	$(CMAKE) -B $(RELEASE_BUILD_DIR) $(COMMON_CONFIGURE_FLAGS) $(RELEASE_CONFIGURE_FLAGS)
	$(CMAKE) --build $(RELEASE_BUILD_DIR) --target pi-cli pi-acp $(BUILD_PARALLEL)

release-jemalloc:
	$(CMAKE) -B $(RELEASE_JEMALLOC_BUILD_DIR) $(COMMON_CONFIGURE_FLAGS) $(RELEASE_JEMALLOC_CONFIGURE_FLAGS)
	$(CMAKE) --build $(RELEASE_JEMALLOC_BUILD_DIR) --target pi-cli pi-acp $(BUILD_PARALLEL)

strip-release: release
	$(STRIP) --strip-all $(RELEASE_BUILD_DIR)/pi-cli
	$(STRIP) --strip-all $(RELEASE_BUILD_DIR)/pi-acp

# The tidy target manages its own process pool; do not pass BUILD_PARALLEL.
lint: configure
	$(CMAKE) -B $(BUILD_DIR) $(TIDY_CONFIGURE_FLAGS)
	$(CMAKE) --build $(BUILD_DIR) --target tidy

tidy-fix: configure
	$(CMAKE) -B $(BUILD_DIR) $(TIDY_CONFIGURE_FLAGS)
	$(CMAKE) --build $(BUILD_DIR) --target tidy-fix

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

# AddressSanitizer + UndefinedBehaviorSanitizer build.  Mutually exclusive with
# TSan (separate build dir) and with the jemalloc memstats build.
asan:
	$(CMAKE) -B $(ASAN_BUILD_DIR) $(COMMON_CONFIGURE_FLAGS) $(ASAN_CONFIGURE_FLAGS)
	$(CMAKE) --build $(ASAN_BUILD_DIR) $(BUILD_PARALLEL)
	env ASAN_OPTIONS="detect_leaks=1:strict_string_checks=1" \
		UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1" \
		$(CTEST) --test-dir $(ASAN_BUILD_DIR) --output-on-failure

# Full gate: functional tests, static analysis, and the race detector.
# Slower than `make test` alone (tsan needs its own build), so this is meant
# for CI / pre-push, not the inner dev loop.
check: test lint tsan

# C++ safety gate for CI: functional tests, clang-tidy (fails on tool/compile
# errors; warnings are baselined in docs/cpp-safety-tooling.md, so not strict),
# then ASan+UBSan.  Needs clang-tidy; do not run with other heavy builds.
safety: test lint asan

clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR)

clean-release:
	$(CMAKE) -E rm -rf $(RELEASE_BUILD_DIR)

clean-release-jemalloc:
	$(CMAKE) -E rm -rf $(RELEASE_JEMALLOC_BUILD_DIR)

clean-tsan:
	$(CMAKE) -E rm -rf $(TSAN_BUILD_DIR)

clean-asan:
	$(CMAKE) -E rm -rf $(ASAN_BUILD_DIR)
