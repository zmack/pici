# C++ safety tooling

Build-time tooling for static analysis and sanitizers. None of it changes
application code.

## clang-tidy (`make lint`, `make tidy-fix`)

`scripts/run_clang_tidy.py` is driven by the CMake `tidy` and `tidy-fix`
targets. It runs one `clang-tidy` process per translation unit and does not
need `run-clang-tidy`.

- **Tool discovery.** CMake looks for `clang-tidy-30` down to `clang-tidy-14`,
  then plain `clang-tidy`, honoring `-DPI_CPP_LLVM_BIN_DIR=<dir>`. `python3`
  is also required. If either is missing the targets are not created.
- **Sources.** `src/**/*.cpp` is enumerated when the target runs, not at
  configure time, so new files are never missed. Each file must appear in
  `compile_commands.json`; files that do not (for example, behind a disabled
  option) are reported as skipped. An empty set, an unreadable compile
  database, or a missing tool fails the run. Headers are covered through
  `HeaderFilterRegex` in `.clang-tidy`.
- **Parallelism.** `PI_CPP_TIDY_JOBS=0` (default) means
  `min(usable CPUs, PI_CPP_TIDY_MAX_AUTO_JOBS=4, available RAM / 1.5 GB)`, at
  least 1. An explicit positive value is used as-is. Examples:
  `make lint TIDY_JOBS=2`, or
  `cmake -B build -DPI_CPP_TIDY_JOBS=8 && cmake --build build --target tidy`.
  Do not run `make lint` at the same time as a full build; they share CPU and
  memory.
- **Failure behavior.** Warnings are advisory (exit 0). A clang-tidy exit
  other than 0 (compile error, crash), a timeout (900 s per file), or a
  tool that cannot start fails the run with the files named. Set
  `-DPI_CPP_TIDY_STRICT=ON` (`make lint TIDY_STRICT=ON`) to also fail on any
  warning. The summary counts diagnostic lines, so a header warning seen from
  several translation units is counted more than once.
- **`tidy-fix`.** Runs serially by default because concurrent `-fix` runs race
  on edits to shared headers. Set `PI_CPP_TIDY_JOBS` explicitly only if you
  accept that. Review scope first; it can make broad mechanical edits.
- **`src/acp/handlers.cpp`.** Analyzed with `clang-analyzer-*` disabled (all
  other checks still run on it). This exclusion predates the script; its
  original rationale is not recorded in the repository. Re-evaluate before
  removing it (`--no-analyzer` in `CMakeLists.txt`).

## ASan + UBSan (`make asan`)

Opt-in: `-DPI_CPP_ASAN_UBSAN=ON`, built in `build-asan` by `make asan`.
Flags are applied globally so fetched dependencies are instrumented too. The
Make target runs ctest with `halt_on_error=1` for UBSan and leak detection on.

Configure fails if combined with `PI_CPP_MEMSTATS` (jemalloc) or with
ThreadSanitizer flags; use separate build directories (`make tsan` and
`make asan`). Expect a slower build and test run than the dev tree.

## Runner validation

Run `python3 -m unittest discover -s test -p test_run_clang_tidy.py -v`.
The fixture executable verifies overlapping processes, serial fixes, failure
propagation, configuration rejection, strict warnings in paths with spaces,
invalid arguments and resource bounds. It does not replace a real lint run.
