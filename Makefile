SHELL := /usr/bin/env bash

CMAKE ?= cmake
CTEST ?= ctest
STRIP ?= strip

BUILD_DIR ?= build
RELEASE_BUILD_DIR ?= build-release
BUILD_PARALLEL ?= --parallel

COMMON_CONFIGURE_FLAGS ?= -DPI_CPP_OTEL_API=OFF
DEV_CONFIGURE_FLAGS ?= -DCMAKE_BUILD_TYPE=Debug
RELEASE_CONFIGURE_FLAGS ?= -DCMAKE_BUILD_TYPE=Release

.PHONY: help configure dev release strip-release lint format format-check test clean clean-release

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
		'  make clean        Remove $(BUILD_DIR)' \
		'  make clean-release Remove $(RELEASE_BUILD_DIR)' \
		'' \
		'Variables:' \
		'  BUILD_DIR=build-dev RELEASE_BUILD_DIR=build-release BUILD_PARALLEL=--parallel'

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

clean:
	$(CMAKE) -E rm -rf $(BUILD_DIR)

clean-release:
	$(CMAKE) -E rm -rf $(RELEASE_BUILD_DIR)
