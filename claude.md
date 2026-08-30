## General Info
This is an agentic cli in c++ using cmake.

For C++ test work, read [docs/testing-with-googletest.md](docs/testing-with-googletest.md)
for the repository's GoogleTest patterns, test commands, and boundary-safety
guidance.

## Development Rules
Before each commit run `make test`, or the equivalent
`cmake --build build --parallel` and
`ctest --test-dir build --output-on-failure`.

For the inner edit-compile loop, prefer a narrow target:
`make dev` or `cmake --build build --target pi-cli --parallel` for CLI changes,
or the specific `test-*` target for focused test work.  Use
`-DPI_CPP_BUILD_TESTS=OFF` in a separate `build-dev` directory when you only
need the CLI binary.

Run `make format` to keep formatting consistent.  Run `make lint` to inspect
clang-tidy warnings.  The tidy
target currently prints advisory warnings from the broader codebase and exits
successfully; treat new warnings in touched code as actionable.  Use
`cmake --build build --target tidy-fix` only when you have reviewed the intended
scope, because it can make broad mechanical edits.

## Code Style

- Do not use decorative section divider comments (`// ─── Foo ────`). They are literal dogshit. They add no information and create noise. Use a blank line to separate sections.

## Notes Learned From Recent Work

- The viewport renderer owns assistant output and status painting.  Readline owns
  the user input row.  Do not draw a fake cursor in assistant output to represent
  user input; readline draws a visible reverse-video insertion cursor at the
  prompt.
- Viewport scrolling is routed through `Renderer::on_scroll(...)`.  It is a
  no-op for non-viewport renderers, and the CLI maps navigation keys to that
  hook while in readline.
- Keep terminal state RAII-only.  Types such as raw-mode guards and viewport
  renderers should explicitly delete copy/move operations when copying would
  duplicate ownership of terminal state.
- For libcurl, include `<curl/curl.h>` only.  Directly including internal curl
  headers such as `curl/easy.h` can fail because required macros are defined by
  the public umbrella header.
- For toml++, define `TOML_HEADER_ONLY` and `TOML_EXCEPTIONS` before including
  toml++ headers.
- OpenTelemetry is optional and disabled by default for faster local builds.
  Keep OTel includes and code behind `PI_CPP_OTEL_ENABLED`, and configure with
  `-DPI_CPP_OTEL_API=ON` only when working on tracing.
- Vendored dependencies that provide helper binaries should use
  `EXCLUDE_FROM_ALL` unless those helper targets are intentionally part of the
  default build.
- CMake automatically uses `ccache` when it is installed.  Prefer Ninja for a
  fresh local build directory when incremental scheduling matters:
  `cmake -S . -B build-ninja -G Ninja -DPI_CPP_OTEL_API=OFF`.
- Common mechanical C++23 pitfalls in this repo:
  - `std::ranges::sort(items, pred)`, not `std::ranges::sort(items, , pred)`.
  - `std::ranges::reverse(items)`, not `std::ranges::reverse(items, )`.
  - `std::ranges::remove_if` returns a subrange; erase with
    `removed.begin(), removed.end()`.
  - With nlohmann/json iterators, use `it.value()` or `(*it)[0]`; avoid `*it[0]`.

## Current Local Artifacts

`pici_architecture_presentation.html` is a generated architecture deck.  It is
not part of the build, but it documents the current architecture and patterns.
