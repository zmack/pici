# Plan G2 — FinCache: cached finalized-block rendering

## Goal

Eliminate the O(N) `render_markdown_ansi` (cmark) call on every `repaint()` delta. Cache the rendered ANSI output for the finalized prefix of `content` (everything up to the last stable block boundary found by G1's `BlockBoundaryScanner`). On each delta, render only the mutable tail block; concatenate with the cached prefix for viewport building.

This reduces per-delta rendering cost from O(content.size()) to O(tail.size()), where tail ≈ the current open paragraph or code fence being streamed.

## Reference

- `src/core/stream_renderer.cpp:390-427` — `ViewportRenderer::repaint()`; the function being replaced.
- `src/core/stream_renderer.cpp:51-56` — `render_visible_markdown()`; the wrapper used for all renders.
- `src/core/stream_renderer.cpp:271-281` — `on_turn_start()`; must reset new state.
- `src/core/stream_renderer.cpp:299-304` — `on_thinking_end()`; must reset new state before repaint.
- Plan G1 — `BlockBoundaryScanner`; prerequisite.

## Correctness invariant

The key challenge: `render_visible_markdown(prefix) + render_visible_markdown(suffix)` is **NOT** in general equal to `render_visible_markdown(prefix + suffix)` because `render_visible_markdown` normalizes trailing newlines based on the *input's* trailing newline count. A prefix ending with `\n\n` gets 2 trailing newlines appended; the joined render produces only the cmark block-level newlines at that position, which may differ.

**Fix**: never render the prefix in isolation for caching. Instead, when the boundary advances, compute the *full* render of `content` and store a prefix of *that*. The hot path then renders only the suffix raw content. Correctness follows because:
1. `fin_cache_.rendered` is a byte-exact prefix of an actual `render_visible_markdown(content)` call — not an approximation.
2. `render_visible_markdown(suffix)` correctly renders the suffix in isolation because the suffix starts at a top-level block boundary (by the scanner's construction), so cmark produces the same output regardless of what precedes it.
3. The trailing-newline accounting is correct: both `fin_cache_.rendered` and `render_visible_markdown(suffix)` source their trailing-newline counts from the content they were given, and the suffix's trailing newlines equal the full content's trailing newlines. The cached prefix ends at `\n\n` in the rendered output (chosen as the last `\n\n` in the full render), so no extra blank line is injected at the join.

**Known limitation**: link reference definitions (`[ref]: url`) followed by uses (`[ref]`) may render incorrectly if the definition is in the cached prefix and the use is in the tail rendered independently, because cmark resolves references across the whole document. This pattern is rare in LLM output; it is accepted and should be noted in a code comment.

## New types and members

Add as private members of `ViewportRenderer` in `stream_renderer.cpp`:

```cpp
struct FinCache {
    size_t      raw_end{0};    // scanner.last_stable when this cache was built
    std::string rendered;      // full_rendered[0..rendered_boundary] from that render
    int         width{0};      // terminal width when rendered was computed
};

BlockBoundaryScanner scanner_;
FinCache             fin_cache_;
```

## Resets

**`on_turn_start()`** — add after existing clears:
```cpp
scanner_   = {};
fin_cache_ = {};
```

**`on_thinking_end()`** — add before the existing `repaint()` call:
```cpp
void on_thinking_end() override {
    in_thinking_ = false;
    status_text_.clear();
    scanner_   = {};    // thinking prefix length just became fixed; prior scan_pos is stale
    fin_cache_ = {};
    repaint();
}
```

## New `repaint()`

Complete replacement of `ViewportRenderer::repaint()` (lines 390-427):

```cpp
void repaint() {
    const int w            = term_width(fd_);
    const int h            = term_height(fd_);
    const int content_rows = h - 2;
    if (content_rows <= 0) return;

    // Build raw content (same composition as before)
    std::string content;
    if (!thinking_buffer_.empty()) {
        content += "[thinking]\n";
        content += thinking_buffer_;
        content += "\n\n";
    }
    content += raw_buffer_;

    // ── Advance boundary scanner (O(new bytes) only) ──────────────────────
    scanner_.advance(content);
    const size_t boundary = scanner_.last_stable;

    // ── Invalidate cache on terminal width change ─────────────────────────
    if (fin_cache_.width != w) {
        fin_cache_ = {};
    }

    std::string tail_rendered;

    if (boundary > fin_cache_.raw_end) {
        // ── Cache miss: boundary advanced (or cache was empty).
        // Render the full content — O(content.size()), but only on boundary advance.
        // Extract the finalized prefix from the actual full render so the cached
        // bytes are byte-identical to what the full render would produce.
        const std::string full_rendered = render_visible_markdown(content);

        // Find the last \n\n in the full rendered output — the rendered boundary.
        // We store everything up to that point as the finalized cache.
        size_t rendered_boundary = 0;
        for (size_t i = 0; i + 1 < full_rendered.size(); ++i) {
            if (full_rendered[i] == '\n' && full_rendered[i + 1] == '\n') {
                rendered_boundary = i + 2;
            }
        }

        fin_cache_.rendered = full_rendered.substr(0, rendered_boundary);
        fin_cache_.raw_end  = boundary;
        fin_cache_.width    = w;

        // The tail is the remainder of the full render — no second parse needed.
        tail_rendered = full_rendered.substr(rendered_boundary);

    } else {
        // ── Cache hit: hot path — render only the suffix (O(tail.size())) ──
        const std::string_view tail_raw(content.data() + fin_cache_.raw_end,
                                        content.size()  - fin_cache_.raw_end);
        tail_rendered = render_visible_markdown(tail_raw);
    }

    // ── Build combined rendered string and split into viewport lines ───────
    // fin_cache_.rendered ends at a \n\n in the rendered domain.
    // tail_rendered starts with the first block after that boundary.
    // Concatenation is correct: no extra blank line at the join.
    std::string combined;
    combined.reserve(fin_cache_.rendered.size() + tail_rendered.size());
    combined += fin_cache_.rendered;
    combined += tail_rendered;

    auto lines = split_lines(combined, w);
    const int total = static_cast<int>(lines.size());
    const int first = std::max(0, total - content_rows);

    std::string frame;
    frame.reserve(combined.size() + static_cast<std::size_t>(content_rows) * 8);
    frame += "\033[H\033[J";

    for (int i = first; i < std::min(total, first + content_rows); ++i) {
        frame += lines[static_cast<std::size_t>(i)];
        if (i + 1 < std::min(total, first + content_rows))
            frame += "\r\n";
    }

    ::write(fd_, frame.data(), frame.size());
    paint_status();
}
```

## Behavioral invariants

| Scenario | Behavior |
|---|---|
| No stable boundary found (`boundary == 0`) | `fin_cache_.rendered = ""`, `combined = tail_rendered = render_visible_markdown(content)`. Identical to old behavior. |
| Boundary at `content.size()` (entire content finalized) | `tail_rendered = full_rendered.substr(len) = ""`. `combined = fin_cache_.rendered`. Correct. |
| Terminal resize | `fin_cache_.width != w` → `fin_cache_ = {}` → next repaint triggers cache miss path → full re-render. Same as before. |
| Turn start | `on_turn_start()` resets scanner and cache. Next repaint starts fresh. |
| Thinking block present | Thinking prefix is frozen after `on_thinking_end()`. Scanner is reset at that point. Subsequent text deltas only extend `content` via `raw_buffer_`, so `scan_pos` tracking remains valid. |

## What stays the same

- `split_lines()` function — unchanged and still called.
- `paint_status()` — unchanged.
- `leave()` — unchanged; uses `render_visible_markdown(raw_buffer_)` directly.
- `RawStreamRenderer` and `DiffMarkdownRenderer` — untouched.
- `Renderer` interface — untouched.

## Property test

Add to `test/` (or `test/test_markdown.cpp`): for a corpus of markdown samples with known stable boundaries (detected by `BlockBoundaryScanner`), assert:

```cpp
// For any content and stable boundary produced by the scanner:
auto scanner = BlockBoundaryScanner{};
scanner.advance(content);
size_t b = scanner.last_stable;
if (b > 0 && b < content.size()) {
    auto full   = render_visible_markdown(content);
    auto prefix = render_visible_markdown(content.substr(0, b));
    auto suffix = render_visible_markdown(content.substr(b));
    // The prefix of the full render (up to last \n\n) should match `prefix`
    // only for the body; the trailing-newline count may differ for edge cases.
    // Assert the BODY matches (ignoring trailing newlines) — this is the
    // important correctness property.
}
```

A simpler smoke-test approach: run a streaming session with the G2 renderer active and compare frame-by-frame output against the reference renderer (same content, no cache). Any visual difference indicates a boundary rule error.

## Estimated size

~40 LOC net change to `repaint()` (replace ~37 lines with ~50 lines). ~10 LOC for struct definition and resets. No new files. Total: ~60 LOC.
