# Terminal title activity during pici turns

## Status and intended reader

This is an implementation plan for Luna. It is intended to be executable
without rediscovering the feature boundaries or making product decisions during
implementation.

The feature adds a Codex-style activity indicator to the terminal tab/window
title while the primary `pi-cli` agent run is active. The minimum behavior is:

```text
idle:    pici
active:  ⠋ pici
         ⠙ pici
         …
```

When a Lua `tab_title(ctx)` hook supplies a custom title, that value replaces
the default project label:

```text
idle:    fix authentication
active:  ⠋ fix authentication
```

This plan copies the useful default behavior from the checked-out Codex TUI. It
does not copy Codex's full `/title` configuration picker or all of its optional
title segments.

## Repository state and constraints

At the time this plan was written, the working tree already contained
user-owned changes in:

- `src/cli/args.h`
- `src/cli/config.cpp`
- `src/cli/config.h`
- `test/test_config.cpp`
- `plans/server-side-compaction.md` (untracked)

Do not reset, discard, reformat, or otherwise absorb those changes. This
feature does not require edits to the dirty config files.

Follow `AGENTS.md` throughout:

- Keep terminal state RAII-only.
- Explicitly delete copy and move operations for types that own terminal state
  or an animation thread.
- Do not add decorative section-divider comments.
- Use a narrow CLI or test target during the edit/compile loop.
- Run `make format`, inspect `make lint`, and run `make test` before any commit.
- Do not commit unless explicitly requested.

The relevant Codex checkout was at `vendor/codex` commit `7a0e974e08` during
planning. The primary references are:

- `vendor/codex/codex-rs/tui/src/terminal_title.rs`
- `vendor/codex/codex-rs/tui/src/chatwidget/status_surfaces.rs`
- `vendor/codex/codex-rs/tui/src/bottom_pane/title_setup.rs`
- Codex commit `a2095de3f1` (`Add terminal title spinner while running`)
- Codex commit `7269a2ef40` (`Adjust terminal title spinner formatting and clearing`)

If the vendored checkout changes before implementation, preserve the behavior
defined in this plan rather than silently expanding scope to match newer Codex
features.

## Current pici behavior

Pici already contains most of the low-level and extension-facing pieces:

- `src/core/terminal.cpp::set_terminal_title` emits OSC 0, terminated by BEL,
  and does nothing when the destination file descriptor is not a TTY.
- `src/core/terminal.h` exposes that helper.
- `src/main.cpp::update_terminal_ui` evaluates `hooks->tab_title(context)`
  immediately before each readline prompt and writes a returned title.
- A Lua `nil` result intentionally leaves the existing terminal title
  unchanged.
- `src/main.cpp::run_turn` blocks around `AgentSession::run_prompt` while
  renderer events are dispatched.
- The renderer exposes `on_turn_start` and `on_turn_end`, but those events map
  to internal agent-loop iterations. Tool calls can produce several of these
  event pairs during one user submission.

The key gap is a lifecycle owner that can animate the title across the entire
primary CLI run and reliably restore the idle title afterward.

## Required behavior

### Title content

Use Codex's ten Braille spinner frames in this order:

```text
⠋ ⠙ ⠹ ⠸ ⠼ ⠴ ⠦ ⠧ ⠇ ⠏
```

Advance frames every 100 milliseconds. Emit the first frame immediately when
the run starts; do not wait 100 milliseconds before showing activity.

The base-title resolution order is:

1. The last non-`nil` string returned by Lua `tab_title(ctx)`.
2. The Git repository root basename for the CLI's startup working directory.
3. The startup working-directory basename when no Git root is found.
4. The literal `pici` if the path has no usable basename.

The base title is rendered exactly, after sanitization, while idle. During an
active run, prefix it with the current spinner frame and one ASCII space:

```text
<frame> <base-title>
```

This ordering matches the checked-out Codex default selection of `activity`
followed by `project-name`.

An empty string returned by `tab_title(ctx)` is different from `nil`:

- `nil` leaves the current base title unchanged.
- `""` sets an empty base title.
- An empty idle base writes an empty OSC title payload.
- While active with an empty base, show only the spinner frame, with no trailing
  space.

Do not automatically add the word `pici` to a non-empty Lua title. Existing
hooks may already include it, and their idle output must remain exact.

### Lifecycle boundary

For this feature, an active turn is one complete call to
`AgentSession::run_prompt`, not one `TurnStartEvent`/`TurnEndEvent` pair.

Start title activity immediately before entering `run_prompt`. Keep it active
through:

- all streamed model responses;
- tool execution;
- subsequent model requests caused by tool results;
- steering/follow-up work consumed by the same run;
- transport or model errors;
- Ctrl-C interruption and abort delivery.

Stop activity and restore the current idle base title after `run_prompt`
finishes. Restoration must also happen during stack unwinding if an exception
escapes.

All paths that use `run_and_persist` should receive this behavior, including:

- ordinary interactive user prompts;
- an initial `--message` prompt when stdout is a TTY;
- Lua slash commands that return `CommandResult.prompt`.

Do not connect animation to `Renderer::on_turn_start` or
`Renderer::on_turn_end`; doing so would briefly restore the idle title between
tool-call iterations.

### Process and surface scope

Apply title management only to the normal `pi-cli` path and only when stdout is
a TTY. The low-level TTY check remains authoritative.

Do not enable it for:

- `pi-cli --rpc`;
- `pi-acp`;
- piped or redirected stdout;
- background `AgentTaskManager` children independently of their owning primary
  CLI run;
- list/test/immediate-exit modes that never enter the normal CLI renderer path.

The primary title remains active while a foreground tool waits for child-agent
work, because the owning `run_prompt` is still active. A detached background
child must not keep the title active after the primary run finishes.

### Exit behavior

Track whether the controller received an `applied` result from at least one
title write. On normal object destruction:

1. Stop and join the animation thread.
2. Clear the managed terminal title with an empty OSC 0 payload.

Do not attempt to query or restore the terminal title that existed before pici
started. There is no portable title-query mechanism, and Codex follows the same
policy.

Signal handling must remain async-signal-safe. Do not write title sequences,
lock a mutex, or join a thread from a signal handler. Existing signal/abort
machinery should cause ordinary control flow or process cleanup to destroy the
controller.

## Design

### Low-level formatting and sanitization

Extend `src/core/terminal.h` and `src/core/terminal.cpp` with pure helpers that
can be tested without a live terminal. Exact names may follow local naming
conventions, but keep these responsibilities separate:

```cpp
std::string sanitize_terminal_title(std::string_view title);
std::string format_active_terminal_title(std::string_view base_title,
                                         std::string_view frame);
std::string terminal_title_sequence(std::string_view sanitized_title);
```

`set_terminal_title(fd, title)` should use the shared sanitization and sequence
builder rather than maintaining a separate byte-replacement loop. Change it to
return a small result enum or boolean that distinguishes at least `applied` from
`not-a-tty/write-failed`. The controller uses that result to decide whether it
owns a title that must later be cleared. Callers must not treat title-write
failure as a fatal CLI error.

Sanitization requirements, following Codex:

- Remove ASCII and Unicode control characters that can terminate or reshape an
  OSC sequence.
- Remove bidi controls and common invisible formatting codepoints, including
  the ranges covered by Codex's `is_disallowed_terminal_title_char`.
- Collapse each whitespace run to one ASCII space.
- Remove leading and trailing whitespace.
- Limit output to 240 Unicode scalar values without splitting a UTF-8 sequence.
- If the 240-character boundary falls between a pending space and a visible
  character, prefer the visible character, as Codex does.

The OSC sequence remains:

```text
ESC ] 0 ; <sanitized title> BEL
```

Build the complete sequence before calling `write` so every update uses one
system call. Do not print failures to the terminal; title output is best-effort
and must not corrupt normal CLI output.

### Project label resolution

Add a small filesystem helper, preferably near the controller rather than in
the Lua bridge:

```cpp
std::string terminal_project_label(const std::filesystem::path &cwd);
```

Walk from the startup working directory toward the filesystem root. The first
ancestor containing an existing `.git` file or directory is the project root.
Return that directory's filename. If no Git marker exists, return the startup
directory's filename, then fall back to `pici`.

Do not shell out to `git`, parse Git config, or repeatedly rescan the filesystem
on animation frames. Resolve the default once at controller construction.

### RAII title controller

Add a focused type in `core/terminal.{h,cpp}`. The suggested public shape is:

```cpp
class TerminalTitleController {
public:
  TerminalTitleController(int fd, std::string initial_base_title);
  ~TerminalTitleController() noexcept;

  TerminalTitleController(const TerminalTitleController &) = delete;
  TerminalTitleController &operator=(const TerminalTitleController &) = delete;
  TerminalTitleController(TerminalTitleController &&) = delete;
  TerminalTitleController &operator=(TerminalTitleController &&) = delete;

  void set_base_title(std::string title);
  void start_activity();
  void stop_activity();
};

class TerminalTitleActivityGuard {
public:
  explicit TerminalTitleActivityGuard(TerminalTitleController &controller);
  ~TerminalTitleActivityGuard() noexcept;

  TerminalTitleActivityGuard(const TerminalTitleActivityGuard &) = delete;
  TerminalTitleActivityGuard &operator=(const TerminalTitleActivityGuard &) = delete;
  TerminalTitleActivityGuard(TerminalTitleActivityGuard &&) = delete;
  TerminalTitleActivityGuard &operator=(TerminalTitleActivityGuard &&) = delete;
};
```

Equivalent names or a `controller.activity_guard()` factory are acceptable if
the ownership and lifecycle semantics stay the same.

Controller invariants:

- Construction writes the sanitized initial base title immediately when the
  destination is a TTY, so the project label is visible at the first readline
  prompt even when no Lua hook exists.
- There is at most one animation thread.
- `start_activity()` is idempotent or asserts a clearly documented inactive
  precondition; it must never create two threads.
- `stop_activity()` requests stop, wakes the wait immediately, joins, and only
  then restores the base title.
- The animation wait must be stop-token-aware. Do not use a plain 100 ms sleep
  that adds shutdown latency.
- The worker uses a snapshot or synchronized access to the base title; there
  must be no data race with `set_base_title`.
- A stopped worker cannot emit a frame after the idle title is restored.
- Cache the last successfully requested sanitized title and skip duplicate OSC
  writes.
- A non-TTY controller should remain inert and should not start a worker thread.
- Destruction is noexcept and best-effort.

The base title normally changes while readline is idle. Nevertheless, make the
controller internally safe if `set_base_title` and animation overlap; future UI
changes should not introduce a data race.

For deterministic unit testing, make the output sink and animation cadence
injectable behind a private/test constructor or small internal dependency. Do
not expose test-only behavior in the user-facing API. Avoid tests that depend on
the wall clock advancing at exactly 100 ms.

### CLI wiring

In `src/main.cpp`:

1. Construct the controller after RPC and immediate-exit paths have returned
   and near renderer construction, so its lifetime covers the normal CLI loop.
2. Resolve the startup cwd once and pass its project label as the initial base
   title.
3. Replace the direct `set_terminal_title` call in `update_terminal_ui` with
   `controller.set_base_title(*title)` when the Lua hook returns a value.
4. Preserve the `nil` path by making no controller call.
5. Create `TerminalTitleActivityGuard` inside `run_and_persist`, immediately
   around the existing `run_turn(...)` call.

The intended shape is approximately:

```cpp
auto run_and_persist = [&](const std::string &input) {
  core::TerminalTitleActivityGuard activity(title_controller);
  return run_turn(runtime, input, *renderer, args.verbose,
                  stream_diagnostics);
};
```

Do not put title behavior in `VerboseRenderer`. `VerboseRenderer` is also an
event adapter, so it has the same internal-turn boundary problem as the base
renderer callbacks.

## Tests

Add focused cases to `test/test_terminal.cpp` and reuse its existing test
registration style.

### Pure formatting tests

Cover at least:

- `format_active_terminal_title("pici", "⠋") == "⠋ pici"`.
- An empty base produces exactly `"⠋"`.
- No trailing or duplicate separator is introduced.
- Every spinner frame appears in the required order.
- OSC output uses `\x1b]0;`, the payload, and a final BEL.

### Sanitization tests

Cover at least:

- ESC, BEL, newline, carriage return, tab, and C1 controls.
- Leading, trailing, and repeated whitespace.
- Trojan-Source-style bidi controls and zero-width formatting characters.
- Ordinary multibyte UTF-8 and emoji remain intact.
- A value longer than 240 Unicode scalar values is truncated without splitting
  UTF-8.
- The pending-space boundary case prefers a visible character.
- Content sanitizing to empty produces a valid empty-title sequence rather than
  leaking the original bytes.

### Controller tests

Using an injected sink and controlled cadence, prove:

- Construction on a simulated non-TTY produces no writes or worker.
- Starting activity emits the first frame immediately.
- Subsequent ticks advance and wrap through the ten frames.
- Stopping joins before restoring the exact sanitized base title.
- No activity write occurs after restoration.
- Repeated start/stop calls cannot create concurrent workers.
- `set_base_title` during activity is race-free and the restored title uses the
  newest base value.
- Destruction while active stops the worker and clears the managed title.
- Duplicate title values do not produce duplicate writes.

### CLI-level smoke checks

After unit tests pass, manually run `pi-cli` in a terminal with a request long
enough to observe several frames. Check:

- The title is the project label at readline.
- The spinner begins immediately after submitting.
- It remains active through a tool call and subsequent model response.
- Completion restores the idle title.
- Ctrl-C restores the idle title.
- A Lua `tab_title` value becomes the base and is restored after activity.
- Redirected stdout contains no OSC title bytes.

Do not make the automated test suite require a particular terminal emulator or
window manager.

## Documentation

Update the terminal UI section in `README.md` and the hook documentation in
`addons/README.md`.

Document:

- The default project-label title.
- The animated prefix during a primary CLI run.
- The exact `nil` versus empty-string behavior of `tab_title`.
- That a custom hook value is used as the complete base title, without an
  automatic `pici` prefix.
- That title output is suppressed when stdout is not a TTY.
- That pici clears the title it manages on exit rather than attempting to
  restore an unknown previous title.

## Implementation order

1. Add and test pure sanitization, formatting, sequence, and project-label
   helpers.
2. Implement the controller with an injected test sink and deterministic
   lifecycle tests.
3. Wire the controller into `src/main.cpp` around `run_and_persist` and the
   existing `tab_title` hook.
4. Build `pi-cli` and perform manual terminal smoke checks.
5. Update documentation.
6. Run formatting, lint, and the full test suite.

## Verification commands

Use the narrow loop first:

```bash
cmake --build build --target test-terminal --parallel
./build/test-terminal
cmake --build build --target pi-cli --parallel
```

Then run repository-wide checks:

```bash
make format
make lint
make test
```

Treat new clang-tidy warnings in `terminal.{h,cpp}` or `main.cpp` as
actionable. Existing advisory warnings elsewhere are not part of this feature.

## Acceptance criteria

The implementation is complete when all of the following are true:

- A normal TTY `pi-cli` session shows the detected project label while idle.
- The title immediately changes to an animated Braille-frame prefix for the
  complete duration of each primary `run_prompt` call.
- The title never flickers idle between tool/model iterations within one run.
- Completion, error, Ctrl-C, and exception paths restore the current idle base
  title.
- Lua `tab_title` retains its documented `nil` semantics and its exact idle
  string becomes the animated base.
- Non-TTY, RPC, ACP, and background-only work emit no new title updates.
- Title payloads cannot inject OSC/control sequences, bidi trickery, or broken
  UTF-8 and are bounded to 240 Unicode scalar values.
- The controller has no data race, late post-restoration write, duplicate
  worker, or avoidable 100 ms shutdown delay.
- The managed title is cleared on controller destruction.
- Focused tests, formatting, lint inspection, and the full test suite pass.

## Non-goals and follow-up seams

Do not expand this change into:

- a `/title` command or interactive title-item picker;
- TOML configuration for title segments or animation;
- title status words such as `Ready`, `Thinking`, or `Waiting`;
- action-required blinking for approvals;
- task-progress, model, token, branch, or rate-limit title segments;
- independent aggregation of detached child-agent activity;
- terminal-specific title-query protocols.

The controller and pure formatting helpers should make those possible later,
but no speculative configuration schema or public API is needed now.
