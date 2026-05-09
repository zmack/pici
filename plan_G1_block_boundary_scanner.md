# Plan G1 — BlockBoundaryScanner: incremental block-boundary detection

## Goal

Provide a stateful, incremental scanner that processes raw markdown `content` byte-by-byte to find "stable block boundaries" — positions in the content string where it is safe to split the document for caching purposes. Called from `ViewportRenderer::repaint()` with the full growing `content` string; only processes newly-appended bytes on each call.

## Reference

- `src/core/stream_renderer.cpp:390-427` — `ViewportRenderer::repaint()`, where the scanner will be called.
- `src/core/stream_renderer.cpp:262` — `ViewportRenderer` class definition; scanner lives as a private member.
- `src/core/stream_renderer.cpp:230-238` — `DiffMarkdownRenderer::find_commit_boundary`, an analogue in rendered-output space; scanner operates in raw-input space instead.

## Definition of a stable boundary

A byte offset `p` is a **stable block boundary** if and only if:
1. `content[p-2] == '\n' && content[p-1] == '\n'` — position is directly after `\n\n`.
2. `!inside_fence` — not inside an open code fence.
3. `last_nonblank_col0` — the most recently completed non-blank line had `line_indent == 0` (its first non-space character was at column zero).

Note: the previous plan drafts included a guard `i+1 < s.size()` to avoid committing a boundary when no content follows. **This guard is dropped.** The boundary is always stored; the cache layer (G2) renders `content[boundary..]` which may be empty — `render_visible_markdown("")` returns `""` harmlessly. Keeping the guard caused boundary loss across incremental `advance()` calls (the boundary at index `N-1` was never re-evaluated when bytes `N+1..M` arrived later).

## Struct

Defined as a private inner struct of `ViewportRenderer` in `stream_renderer.cpp`. All fields reset to zero/false by `= {}`.

```cpp
struct BlockBoundaryScanner {
    size_t scan_pos{0};            // bytes of `s` consumed in prior advance() calls
    size_t last_stable{0};         // byte offset of the last confirmed stable boundary

    // ── Fence tracking (persists across advance() calls) ───────────────────
    bool inside_fence{false};
    char fence_char{0};            // '`' or '~' (valid when inside_fence == true)
    int  fence_len{0};             // opening run length (≥3, valid when inside_fence)

    // ── Current-line state (reset at each '\n') ────────────────────────────
    int  line_indent{0};           // leading spaces/tabs before first non-space
    bool saw_nonspace{false};      // have we seen non-space on this line
    int  fence_run{0};             // run of fence_run_char starting at line start
    char fence_run_char{0};        // the char being counted in the run
    bool line_only_fence{true};    // true iff no non-fence, non-space chars after the run

    // ── Cross-line state ───────────────────────────────────────────────────
    bool last_nonblank_col0{true}; // last completed non-blank line had line_indent == 0
    bool last_char_nl{false};      // previous byte was '\n'

    void advance(std::string_view s);
};
```

## `advance()` algorithm

Process `s[scan_pos..s.size()-1]` byte by byte, then set `scan_pos = s.size()`.

For each byte `c = s[i]`:

### Case `c == '\n'`

```
blank = !saw_nonspace

// Boundary check: second consecutive newline ending a blank line
if blank && last_char_nl && !inside_fence && last_nonblank_col0:
    last_stable = i + 1        // boundary is after the second '\n'
    // Note: no i+1 < s.size() guard — see Definition section above

// Non-blank line epilogue
if !blank:
    last_nonblank_col0 = (line_indent == 0)
    // Fence detection: line is a fence line if it had a 3+ run at column 0
    // (preceded by at most 3 spaces, followed by anything for opener;
    // only pure fence chars + trailing spaces for closer)
    if line_indent <= 3 && fence_run >= 3:
        if !inside_fence:
            inside_fence  = true
            fence_char     = fence_run_char
            fence_len      = fence_run
        elif fence_run_char == fence_char && fence_run >= fence_len && line_only_fence:
            inside_fence  = false
            fence_char     = 0
            fence_len      = 0

last_char_nl = true

// Reset line state for next line
line_indent = 0; saw_nonspace = false
fence_run = 0; fence_run_char = 0; line_only_fence = true
```

### Case `c != '\n'`

```
last_char_nl = false

if !saw_nonspace:
    if c == ' ' or c == '\t':
        line_indent++        // Note: tab counts as 1 indent unit (not 4); conservative
    else:
        saw_nonspace = true
        if line_indent <= 3 && (c == '`' || c == '~'):
            fence_run      = 1
            fence_run_char = c
            line_only_fence = true
        else:
            fence_run      = 0
            line_only_fence = false
elif line_only_fence:
    if c == fence_run_char:
        fence_run++
    elif c == ' ':
        // Trailing spaces after fence run are legal on closing lines;
        // do NOT flip line_only_fence. Openers may also have trailing
        // content (info string) so the non-space branch handles that.
        // (intentional no-op here)
    else:
        line_only_fence = false
```

### After the loop

```
scan_pos = s.size()
```

## Fence detection notes

- **Opening fence**: `line_indent <= 3 && fence_run >= 3` at line end. Info string (e.g., `python` in ` ```python`) causes `line_only_fence=false` — but openers don't require `line_only_fence`. The check `!inside_fence` fires for openers regardless of `line_only_fence`.
- **Closing fence**: Same character as opener, run length ≥ opener's length, and `line_only_fence` must be true (only fence chars and trailing spaces — no info string allowed).
- **Tab indentation**: Counted as 1 unit, not 4. Per CommonMark spec a tab is 4 columns. This means a fence indented with a single tab would be rejected (tab gives `line_indent=1`, which is ≤ 3 and passes). This is conservative — a fence that cmark treats as indented code (tab indent) would also pass, slightly over-committing. In practice LLMs don't use tab-indented fences.

## Edge cases from LLM output

| Scenario | Behavior | Correct? |
|---|---|---|
| Multi-paragraph list item: `"  - item\n\n  cont"` | `last_nonblank_col0=false` for `"  - item"` → no boundary. ✓ | Safe |
| Setext heading: `"Title\n===\n\npara"` | `===` is at indent 0 but `line_only_fence=false` (chars aren't `` ` `` or `~`). Both lines are col0. Boundary fires after `\n\n`. cmark renders them as heading + paragraph regardless. ✓ | Safe |
| Link reference def: `"[ref]: url\n\npara"` | Commits. `render(prefix)+render(suffix)` may differ if `para` uses `[ref]`. LLM-generated reference links are rare; document this limitation. | Accepted |
| HTML blocks spanning blank lines | Not detected; scanner commits inside them if `\n\n` appears. Type-1 HTML blocks (`<pre>`, `<script>`) are vanishingly rare in LLM output. | Accepted |
| Empty input | No newlines processed; `last_stable=0`. ✓ | Safe |
| Input ends with `\n\n` | Boundary committed (e.g., `last_stable=len`). G2 renders an empty suffix, which is `""`. ✓ | Safe |
| Consecutive blank lines `"para\n\n\nmore"` | Scanner commits at offset 6 (after `\n\n`), then at offset 7 (after `\n\n\n` as a new pair). `last_stable=7`. Combined render uses `content[7..] = "more"`. The boundary at `7` is past an extra blank line. Rendered prefix has 2 trailing `\n` (from `\n\n` at 4-5) but cached prefix from G2's full render handles this correctly. ✓ | Safe (G2 uses full render) |

## Unit tests

Add to `test/test_terminal.cpp` (or a new `test_block_scanner.cpp` file if preferred):

| Test | Input | Expected `last_stable` |
|---|---|---|
| Basic paragraph | `"para\n\nmore"` | `6` |
| Empty-tail boundary | `"para\n\n"` | `6` (no guard) |
| Indented line, no commit | `"  item\n\nmore"` | `0` |
| Two boundaries | `"p1\n\np2\n\nmore"` | `9` (last one) |
| Fence open+close | `` "```py\ncode\n```\n\nmore" `` | after final `\n\n` |
| No commit inside fence | `` "```py\n\nstill\n```\n\nmore" `` | after final `\n\n` only |
| Tilde fence | `"~~~\ncode\n~~~\n\nmore"` | after final `\n\n` |
| Incrementality | scan `"para\n"` then `"\nmore"` | same as scanning `"para\n\nmore"` in one call |
| Empty | `""` | `0` |
| Initial blank lines | `"\n\npara"` | `2` (initial `last_nonblank_col0=true`) |
| Trailing-space closer | `` "```\ncode\n```   \n\nmore" `` | after final `\n\n` (closer has trailing spaces) |

## Constraints

- Do **not** export `BlockBoundaryScanner` in any header. It is private to `ViewportRenderer`.
- Do **not** change the `Renderer` interface, `dispatch_event`, or any other renderer.
- Scanner must be reset on `on_turn_start()` — add `scanner_ = {};`.
- Scanner must be reset on `on_thinking_end()` before `repaint()` — the thinking prefix length has just become fixed; any prior `scan_pos` that included a partial thinking buffer is stale.
- C++23, no warnings.

## Estimated size

~80 LOC: struct definition (~25 lines) + `advance()` implementation (~45 lines) + resets in two places (~5 lines). Unit tests ~80 LOC additional.
