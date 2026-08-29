# Remove standalone Markdown and viewport renderers

## Goal

Retain `region` as the interactive renderer and `raw` as the non-TTY/plain-output renderer. Remove the standalone `markdown` and `viewport` renderer modes, and make the default `auto` selection choose `region` on a TTY.

The Markdown functionality itself must remain: `region` uses the shared Markdown-to-ANSI helpers.

## Current inventory

There are four concrete implementations and five registry names:

- `raw` -> `RawStreamRenderer`
- `markdown` -> `DiffMarkdownRenderer`
- `viewport` -> `ViewportRenderer`
- `region` -> `RegionRenderer`
- `auto` -> selector (`markdown` on TTY, `raw` otherwise)

Relevant locations:

- `src/core/stream_renderer.h/.cpp`: common interface, raw/markdown/viewport implementations, auto selection, registry, dispatcher, shared rendering helpers
- `src/core/region_renderer.h/.cpp`: region implementation and public frame-builder/state types
- `src/main.cpp`: renderer selection and `VerboseRenderer` adapter
- `src/cli/args.h/.cpp`: `--render` option and help text
- `src/cli/config.cpp`: `[display].render` loading/merging
- `CMakeLists.txt`: renderer source and tests
- `test/test_markdown.cpp`, `test/test_region_renderer.cpp`, `test/test_readline.cpp`, `test/test_config.cpp`

## Intended behavior after removal

| Selection | Result |
|---|---|
| no `--render`, TTY | `region` |
| no `--render`, non-TTY | `raw` |
| `--render auto`, TTY | `region` |
| `--render auto`, non-TTY | `raw` |
| `--render region` | `region` |
| `--render raw` | `raw` |
| removed/unknown name | existing warning and fallback to `auto` |

Document the supported built-in names as `auto`, `raw`, and `region`.

## Implementation steps

### 1. Protect shared Markdown functionality

Before deleting renderer classes, inventory the helpers in `stream_renderer.cpp` and confirm which are used by `region_renderer.cpp`. Preserve or relocate anything needed by region, including:

- `render_visible_markdown()` and its trailing-newline behavior
- Markdown/ANSI formatting helpers
- terminal width, wrapping, and visible-row helpers
- command-output escaping/fencing behavior

Pure Markdown rendering remains part of the core functionality; only the incremental `DiffMarkdownRenderer` wrapper is removed.

### 2. Remove the two renderer implementations

In `src/core/stream_renderer.cpp`:

- Delete `DiffMarkdownRenderer`.
- Delete `ViewportRenderer`.
- Remove `make_diff_renderer()` and `make_viewport_renderer()` definitions.
- Change `make_auto_renderer()` to call `make_region_renderer()` for TTYs and `make_raw_renderer()` otherwise.
- Keep the common `Renderer` interface, dispatcher, raw renderer, shared helpers, registry, and auto factory.

In `src/core/stream_renderer.h`:

- Remove the declarations for `make_diff_renderer()` and `make_viewport_renderer()`.
- Keep `render_visible_markdown()`, `make_raw_renderer()`, `make_region_renderer()`, `make_auto_renderer()`, and the registry API.

### 3. Update registry and selection surfaces

In the registry constructor, retain only the built-in registrations for:

- `raw`
- `region`
- `auto`

Keep custom `register_renderer()` support unchanged.

Review `main.cpp::make_renderer()` and preserve the current unknown-name warning/fallback behavior. Do not remove the generic renderer capability forwarding in `VerboseRenderer`; region depends on status, tool-output, scrolling, resize, repaint, and prompt-preparation hooks.

### 4. Update CLI, config, and documentation

Update the supported mode comments/help and examples in:

- `src/cli/args.h`
- `src/cli/args.cpp`
- `config.toml.example`
- `README.md`
- renderer-related docs/plans/examples that present `markdown` or `viewport` as supported modes

Make the default change explicit: interactive `auto` now uses region. Explain that raw remains the pipe/non-TTY path.

Do not remove `[display].render` or `Args::render`; they remain needed for `auto`, `raw`, `region`, and custom registry renderers.

### 5. Update tests

Preserve pure Markdown tests, but remove tests that instantiate the deleted renderer:

- In `test/test_markdown.cpp`, retain tests for Markdown formatting/shared helpers and remove or rewrite tests calling `make_diff_renderer()`.
- In `test/test_region_renderer.cpp`, remove viewport-specific construction/behavior tests while retaining all region state, frame, tool, registry, and lifecycle coverage.
- Update registry tests to assert that `raw`, `region`, and `auto` exist and `markdown`/`viewport` do not.
- Update config/CLI tests for the reduced mode set and default behavior.
- Review readline PTY tests for assumptions about the previous default TTY renderer.

Add coverage for `auto` selection where feasible:

- TTY -> region
- non-TTY -> raw

### 6. Review alternate-screen integrations

Because region becomes the default TTY renderer, explicitly audit and test:

- alternate-screen enter/exit on normal completion
- Ctrl-C, abort, transport errors, and shutdown cleanup
- readline prompt handoff and cursor anchoring
- terminal resize during and between turns
- `/tree` and `/model` selector interaction
- status line and subagent pane updates
- concurrent tool output and scrolling
- print mode and piped/non-TTY output

Capability-based code should remain. Remove only comments or branches that are genuinely viewport-specific.

### 7. Build and source-compatibility review

Keep both renderer source files in `CMakeLists.txt`; `stream_renderer.cpp` still contains the dispatcher, raw renderer, registry, auto factory, and shared rendering utilities.

Removing the two factory declarations is a source-breaking change for external C++ callers. This is acceptable for the current cleanup. If compatibility is later required, add deprecated compatibility factories rather than restoring the removed implementations.

## Validation

Run the normal build and tests:

```bash
make test
```

Also manually exercise:

```bash
pici --render region "..."
pici --render raw "..."
pici "..."
pici -p "..."
pici "..." | cat
```

Verify that default TTY sessions use region, pipes remain plain output, and region's alternate-screen/readline lifecycle remains clean.

## Main risks

1. Accidentally deleting Markdown helpers required by region.
2. Changing the default TTY path and exposing region's stricter alternate-screen lifecycle to all interactive users.
3. Leaving stale registry names, docs, config examples, or tests behind.
4. Breaking external users of the removed factory functions.
5. Removing viewport-specific code that is actually generic capability handling needed by region.

## Completion criteria

- Only `raw`, `region`, and `auto` are built-in registry modes.
- `auto` selects region on TTY and raw otherwise.
- Shared Markdown formatting still works inside region.
- No tests, help text, README sections, or config examples claim that standalone `markdown` or `viewport` are available.
- Region, readline, selector, resize, abort, tool, and non-TTY behavior pass regression testing.
