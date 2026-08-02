# pici Code Quality Fixes

A prioritized list of improvements based on static analysis, test coverage gaps,
and code hygiene issues identified in the codebase.

---

## Priority 0: Immediate ✅

### Remove all decorative section divider comments

✅ **Done.** All 12 files cleaned across 59 occurrences. `make format` run,
CLI builds and links, all 18 tests pass after the change.

---

## Priority 1: High

### Add tests for uncovered critical paths

**18/18 tests pass**, but **26 of 41 source files (63%) lack a corresponding test**.
The largest untested compilation units are the highest risk:

| File | LOC | Risk |
|---|---|---|
| `src/core/stream_renderer.cpp` | 932 | High — entire terminal output pipeline |
| `src/cli/readline.cpp` | 443 | High — user input handling |
| `src/core/syntax_highlight.cpp` | 439 | Medium — mostly data-driven |
| `src/core/message_types.cpp` | 426 | Medium — serialization round-trips |
| `src/acp/handlers.cpp` | 348 | Medium — ACP request/response |
| `src/http/http_client.cpp` | 266 | Medium — HTTP transport |
| `src/core/event_types.cpp` | 130 | Low |
| `src/core/agent_state.cpp` | 2 | Trivial |

**Action:** Focus on `stream_renderer`, `readline`, and `http_client` first.

### Fix `agent_task.cpp` (58 clang-tidy warnings)

This file produces 54% of all tidy warnings. Major categories:

- **`misc-include-cleaner`** (~30 warnings) — Missing direct includes for
  `std::string`, `std::shared_ptr`, `std::size_t`, `std::vector`, `std::set`,
  `std::string_view`, `std::optional`, `std::mutex`, `std::jthread`, etc.
- **`bugprone-implicit-widening-of-multiplication-result`** (3) — Lines 186–188
  in `agent_task.h`: `int` multiplication widened to `std::size_t`.
- **`bugprone-narrowing-conversions`** (2) — Lines 339, 346: `unsigned long` to
  `difference_type`.
- **`bugprone-empty-catch`** (1) — Line 281: empty catch block hides issues.
- **`bugprone-exception-escape`** (1) — Line 241: `~AgentTaskManager` could throw.
- **`readability-implicit-bool-conversion`** (1) — Line 81: pointer-to-bool.
- **`readability-convert-member-functions-to-static`** (2) — Lines 255, 299.

**Action:** Add missing includes, fix narrowing conversions, remove empty catch,
mark throwing destructor `noexcept`.

### Fix `rpc_mode.cpp` (20 clang-tidy warnings)

- **`readability-implicit-bool-conversion`** (6) — `AgentTaskManager*` checked as
  boolean (lines 175, 243, 276, 288, 301, 314, 424).
- **`misc-include-cleaner`** (6) — Missing direct includes for
  `AgentTaskSnapshot`, `agent_task_status_to_string`, `AgentInterruptReason`,
  `AgentTaskManager`, `AgentWaitRequest`, `std::stop_token`, `std::size_t`.
- **`bugprone-exception-escape`** (1) — Line 187: lambda in a signal handler
  context that should not throw.
- **`performance-unnecessary-value-param`** (1) — Line 189: `std::stop_token`
  passed by value.
- **`misc-include-cleaner`** — Line 18: unused include `type_traits`.

**Action:** Add missing includes, replace pointer-to-bool with explicit
`!= nullptr`, pass `stop_token` by `const &`.

---

## Priority 2: Medium

### Break up monolithic source files

Several files are doing too much and should be split by responsibility:

| Current File | LOC | Proposed Split |
|---|---|---|
| `src/core/builtin_tools.cpp` | 1,310 | One file per tool (9 files) |
| `src/core/lua_tool.cpp` | 2,043 | `lua_bindings.cpp` + `lua_subagent.cpp` + `lua_inline_tools.cpp` |
| `src/main.cpp` | 1,480 | `repl.cpp` + `signal_handlers.cpp` + `app_bootstrap.cpp` |
| `src/core/agent_loop.cpp` | 1,188 | Extract state machine transitions into `agent_loop_transitions.cpp` |
| `src/core/providers/openai_completions.cpp` | 818 | Split provider from SSE parser |

### Fix remaining `misc-include-cleaner` warnings (46 total)

After fixing the two hotspot files above, ~10 warnings remain spread across
`lua_tool.cpp`, `models.cpp`, `main.cpp`, and `agent_loop.cpp`. These are
low-risk but clutter the tidy output.

### Fix `performance-unnecessary-value-param` (8 remaining)

Primarily in `agent_task.h` (line 116), `rpc_mode.cpp`, and `lua_tool.cpp`.
Parameters like `std::string`, `AgentTaskEvent` variants, and `stop_token` are
copied on every invocation when a `const &` would suffice.

### Fix `modernize-use-designated-initializers` (9 instances)

C++20 designated initializers are available but not used. Primarily in
`models.cpp` and `stream_renderer.cpp`. Mechanical fix.

---

## Priority 3: Low

### Enable LTO in release build

The release build uses `-O3 -DNDEBUG` but does not enable link-time
optimization. Adding `-flto` (or `CMAKE_INTERPROCEDURAL_OPTIMIZATION=ON`) would
likely reduce the binary by 10–20% (currently 18 MB).

### Extract JSON schema strings to external files

`builtin_tools.cpp` contains several multi-line JSON schema raw string literals
(300–400 chars each) baked into the source. Moving these to `.json` files and
loading them at startup would separate data from code and make schema changes
reviewable without touching C++.

### Fix `agent_state.h` design

The 246-line header defines the entire agent state object inline with 7 section
dividers, while the implementation file (`agent_state.cpp`) is only 2 lines.
Either extract implementation to `.cpp` or accept it as a data-only header
(with the dividers removed per P0).

### Fix `bugprone-implicit-widening-of-multiplication-result` (9 instances)

All in `agent_task.h` (lines 186–188) and `models.cpp`. Multiply `int` values
and assign to `std::size_t` without explicit cast. Could overflow on values
above ~46,000 (unlikely in practice but a correctness concern).

---

## Appendix: Tidy Warning Breakdown

| Checker | Count | % of Total |
|---|---|---|
| misc-include-cleaner | 46 | 42.6% |
| readability-implicit-bool-conversion | 14 | 13.0% |
| performance-unnecessary-value-param | 10 | 9.3% |
| modernize-use-designated-initializers | 9 | 8.3% |
| bugprone-implicit-widening-of-multiplication-result | 9 | 8.3% |
| readability-qualified-auto | 3 | 2.8% |
| performance-move-const-arg | 3 | 2.8% |
| cppcoreguidelines-init-variables | 3 | 2.8% |
| readability-convert-member-functions-to-static | 2 | 1.9% |
| modernize-use-starts-ends-with | 2 | 1.9% |
| bugprone-exception-escape | 2 | 1.9% |
| readability-container-size-empty | 1 | 0.9% |
| bugprone-empty-catch | 1 | 0.9% |
| clang-analyzer-cplusplus.NewDeleteLeaks | 1 | 0.9% |
| bugprone-narrowing-conversions | 2 | 1.9% |
| **Total** | **108** | **100%** |

### By File

| File | Warnings |
|---|---|
| `src/core/agent_task.cpp` | 58 |
| `src/cli/rpc_mode.cpp` | 20 |
| `src/core/agent_task.h` | 15 |
| `src/core/lua_tool.cpp` | 7 |
| `src/core/models.cpp` | 5 |
| `src/main.cpp` | 2 |
| `src/core/agent_loop.cpp` | 1 |

Two files (agent_task + rpc_mode) account for **72% of all tidy warnings**.

---

## Test Coverage Summary

| Metric | Value |
|---|---|
| Source `.cpp` files | 41 |
| Have a matching test | 15 (37%) |
| No test coverage | 26 (63%) |
| Total test LOC | 8,559 |
| Total source LOC | 20,030 |
| Test pass rate | 18/18 (100%) |