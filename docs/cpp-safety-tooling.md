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
  other checks still run on it) via `--no-analyzer` in `CMakeLists.txt`. The
  exclusion came in with the "Fix lints" commit (e960a30) with no recorded
  rationale. Measured 2026-10-13 (clang-tidy 18, Debian, runner flags): the
  analyzer alone on this file takes about 94 s and 0.7 GB RSS and reports
  nothing, so it is not needed to avoid a crash or a timeout (900 s limit)
  today. It is kept, unchanged, pending a decision; removing it is a one-line
  change plus one more ~95 s of lint time.
- **Header filter.** `HeaderFilterRegex` is `^.*/src/(core|http)/.*\.(h|hpp)$`.
  It used to require a `/pici/src/` path component, which silently matched
  nothing in a checkout or worktree not named `pici`. Widening it beyond
  `core` and `http` is a follow-up (needs its own baseline).

### Baseline (2026-10-13, clang-tidy 18)

Full `make lint` (`PI_CPP_TIDY_JOBS=2`): 72/72 files ok, 0 failures, 305
warnings, 31 minutes wall. Counts are diagnostic lines (header warnings may be
repeated per translation unit), taken before the fixes below.

| Check | Count | Disposition |
| --- | --- | --- |
| `misc-include-cleaner` | 177 | Disabled in `.clang-tidy`: it wants direct includes for every transitive std/third-party symbol, is not a safety signal, and was 58% of all output. |
| `readability-function-cognitive-complexity` | 86 | Baselined. Refactoring candidates, not defects; tackle per file with the owning change. |
| `bugprone-implicit-widening-of-multiplication-result` | 7 | Fixed (`UL` literals in `lua_tool.cpp`, `mcp_tool.cpp`). |
| `bugprone-unchecked-optional-access` | 3 | Baselined as false positives: `acp/main.cpp` dereferences `process` after a `catch` that returns; `session_runtime.cpp` dereferences `action` after a guarded `std::exchange` under the lock. |
| `performance-unnecessary-value-param` | 3 | Baselined (`session_runtime.cpp`, `builtin_tools.cpp`, `mcp_transport.cpp`); cheap handles, low value. |
| `cppcoreguidelines-pro-type-const-cast` | 3 | `NOLINT` with reason: Lua light userdata is an untyped `void*` and is only read back as const. |
| `cppcoreguidelines-owning-memory`, `-no-malloc` (4), `bugprone-multi-level-implicit-pointer-conversion`, `bugprone-sizeof-expression` (4) | 8 | `NOLINT` with reason in `lua_tool.cpp`: `lua_Alloc` is a C allocator ABI; the extra-space slot stores a pointer value. |
| `cppcoreguidelines-pro-bounds-pointer-arithmetic` | 3 | Baselined (`apply_patch.cpp`, `builtin_tools.cpp`, `mcp_transport.cpp`); bounded buffer walks. Worth a later review. |
| `misc-no-recursion` | 2 | Baselined: intentional tree/JSON recursion (`session_store.cpp`, `openai_codex_responses.cpp`). |
| `concurrency-mt-unsafe` | 1 | Baselined (`builtin_tools.cpp:1804`); re-check if that code runs off the main thread. |
| other single-hit style checks | ~10 | Baselined. |

After the fixes and the `misc-include-cleaner` removal, the same run reports
115 warnings (72/72 files ok, 0 failures); the remaining ones are the baselined
rows above.

There is no machine-readable baseline file yet: warnings are advisory, so
`make lint` exits 0 with these present. Making `TIDY_STRICT=ON` usable in CI
requires a baseline/suppression mechanism (follow-up).

## Safety gate (`make safety`)

`make safety` = `make test` + `make lint` + `make asan`: functional tests,
clang-tidy (fails on tool/compile errors, not warnings), then ASan+UBSan. It
is the single command CI should call. It is slow (one Debug build, one lint
pass, one sanitizer build) and must not share the machine with other heavy
builds. `make check` is the older test + lint + tsan gate.

## ASan + UBSan (`make asan`)

Opt-in: `-DPI_CPP_ASAN_UBSAN=ON`, built in `build-asan` by `make asan`.
Flags are applied globally so fetched dependencies are instrumented too. The
Make target runs ctest with `halt_on_error=1` for UBSan and leak detection on.

Configure fails if combined with `PI_CPP_MEMSTATS` (jemalloc) or with
ThreadSanitizer flags; use separate build directories (`make tsan` and
`make asan`). Expect a slower build and test run than the dev tree.

### First full run (2026-10-13)

Debug ASan+UBSan build, ctest with `detect_leaks=1`, `halt_on_error=1`: the
first run passed 45/51 (6 failing test binaries). Every failure was LeakSanitizer; there were no
AddressSanitizer memory errors and no UBSan reports. Root causes, all fixed:

- `MailboxStore` constructor leaked the SQLite handle when a later step
  (permissions, configure, migrate) threw, because a throwing constructor
  skips the destructor.
- `ModelCatalog` <-> `Authentication` shared_ptr cycle: the catalog's
  request-auth resolver captured `shared_ptr<Authentication>`, which owns the
  catalog. The resolver now captures a `weak_ptr`.
- `LuaHooksImpl` <-> `InlineLuaTool` cycle: the impl stored the tools that
  each hold a strong reference to it. The impl now returns the tools to
  `LuaHooks::registered_tools` instead of storing them.
- `CmdRunSession` Lua agent bindings captured `shared_ptr<TaskTree>`, while
  the TaskTree's agent owns the hooks holding those bindings. They now capture
  a `weak_ptr` (`live_tree()`).

After these fixes ASan+UBSan passed 51/51.

## Runner validation

Run `python3 -m unittest discover -s test -p test_run_clang_tidy.py -v`.
The fixture executable verifies overlapping processes, serial fixes, failure
propagation, configuration rejection, strict warnings in paths with spaces,
invalid arguments and resource bounds. It does not replace a real lint run.
