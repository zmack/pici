# Plan G4 — Byte-offset window cache in FinCache

## Goal

Eliminate the O(fin_rendered.size()) `split_lines` walk in G3's mixed path — the case where `tail_rows < content_rows` and some finalized rows are needed to fill the viewport. Cache a byte offset into `fin_cache_.rendered` pointing to where the last `content_rows` rows of finalized content begin. On each hot-path delta in the mixed case, slice from that offset and concatenate with `tail_rendered` instead of splitting `combined = fin + tail`.

## Reference

- Plan G3 — prerequisite; this plan extends G3's `FinCache` and `repaint()`.
- `src/core/stream_renderer.cpp:463-485` — `split_lines()`; the walk this plan avoids.
- `src/core/terminal.h` — `skip_ansi_sequence`, `advance_utf8`, `codepoint_width`; needed for the byte-offset helper.

## When G4 helps

G3 already handles `tail_rows >= content_rows` with O(tail) cost. G4 targets the remaining mixed path. In a typical LLM session:
- The finalized prefix grows over time (e.g., 200 rows).
- The tail at any moment is a short block (e.g., 5-15 rows of the current paragraph).
- `content_rows` is typically 30-60 rows.

Without G4: mixed path does `split_lines(fin + tail, w)` = O(fin + tail) per delta.
With G4: mixed path slices `fin_suffix` from a cached offset (O(1)) + `split_lines(fin_suffix + tail, w)` = O(content_rows × avg_line_bytes) per delta (bounded by viewport size, not document size).

## ANSI safety constraint

`fin_cache_.rendered` contains ANSI escape sequences (bold, italic, color). If we slice at a wrap boundary (mid-line), the slice may start mid-ANSI-sequence, or the terminal may have lost SGR state (bold on) that was set before the slice point.

**Fix**: restrict `window_offset` to always be at a `\n` boundary in `fin_cache_.rendered`. The markdown renderer always closes all SGR codes before emitting a block-ending `\n` (e.g., `\033[0m` precedes paragraph-ending `\n\n`). Cutting at `\n` ensures clean ANSI state.

Because we cut at `\n` (source line boundaries) rather than wrap boundaries, the "fin_suffix" may span slightly more visual rows than requested (a long source line that wraps into multiple rows). This is handled by the existing `first = max(0, total - content_rows)` tail-taking in the split step.

## New fields in `FinCache`

```cpp
struct FinCache {
    size_t      raw_end{0};
    std::string rendered;
    int         width{0};
    int         row_count{0};

    // Window: byte offset of the last block of rows in `rendered`.
    // Always points to the byte immediately after a '\n' (or 0 if at start).
    // Valid when window_height == content_rows; otherwise use full combined split.
    size_t window_offset{0};      // byte offset into rendered
    int    window_row_start{0};   // visual row index (0-based) at window_offset
    int    window_rows{0};        // visual rows from window_offset to end of rendered
    int    window_height{0};      // content_rows value when window was computed (0 = invalid)
};
```

## New helper: `find_window_offset`

Private static on `ViewportRenderer`. Walks `rendered` forward from the start, counting visual rows (using the same wrapping rules as `split_lines`), until it has advanced past `(row_count - content_rows)` rows. Returns the byte position immediately after the `\n` that ends that row.

```cpp
// Returns the byte offset in `rendered` immediately after the '\n' that ends
// physical row `target_row` (0-based). If `target_row == 0`, returns 0.
// Uses the same column-counting logic as split_lines: ANSI escapes are zero-width,
// wide chars are 2 columns, wrapping at `width`. Always lands on a '\n' boundary.
//
// Because we land on '\n' boundaries (not wrap boundaries), the result is
// ANSI-safe: all SGR codes opened before this position are closed by the
// paragraph/block endings that emit '\n'.
static size_t find_window_offset(std::string_view rendered, int width,
                                 int target_row) {
    if (target_row <= 0) return 0;
    int rows = 0;
    int col  = 0;
    for (size_t i = 0; i < rendered.size();) {
        if (rendered[i] == '\n') {
            ++rows;
            col = 0;
            ++i;
            if (rows == target_row) return i;
            continue;
        }
        if (rendered[i] == '\033') {
            const auto nxt = skip_ansi_sequence(rendered, i);
            if (nxt > i) { i = nxt; continue; }
        }
        const int cw  = codepoint_width(rendered, i);
        const auto nxt = advance_utf8(rendered, i);
        col += cw;
        if (col >= width) {
            // wrap — do NOT advance rows here; wraps don't create '\n' positions
            // we need. Keep scanning until the next actual '\n'.
            col = 0;
        }
        i = nxt;
    }
    return rendered.size(); // target_row beyond end; return end
}
```

**Note**: This helper does NOT count wraps as row increments for the purpose of finding the offset. It only stops at `\n` boundaries. This means the slice starting at `window_offset` may contain a few more visual rows than `window_rows` if the last kept source line wraps. The downstream `split_lines` + `first = max(0, total - content_rows)` handles this correctly.

## Compute window in cache-miss branch

After populating `fin_cache_.row_count`, compute the window:

```cpp
fin_cache_.window_height = content_rows;
if (fin_cache_.row_count > content_rows) {
    const int target = fin_cache_.row_count - content_rows;
    fin_cache_.window_offset    = find_window_offset(fin_cache_.rendered, w, target);
    fin_cache_.window_row_start = target;
    fin_cache_.window_rows      = fin_cache_.row_count - target;
} else {
    // Entire finalized content fits in the viewport; window starts at beginning.
    fin_cache_.window_offset    = 0;
    fin_cache_.window_row_start = 0;
    fin_cache_.window_rows      = fin_cache_.row_count;
}
```

`find_window_offset` runs once per cache advance. It is O(fin_rendered.size()) but that's already paid by the full render + `cursor_rows_for_rendered` calls in the same branch.

## Window invalidation

The window is invalidated (falls back to full combined split) when:

1. **Width changes**: `fin_cache_ = {}` clears everything including `window_height = 0`. ✓
2. **Boundary advances**: window is recomputed in cache-miss branch. ✓
3. **`content_rows` changes** (resize changes height): `fin_cache_.window_height != content_rows` → fallback fires. The window is NOT automatically rebuilt; it is rebuilt on the next cache miss (next boundary advance). If no new boundary is found after the resize, the window stays invalid and the fallback runs every repaint.

   Mitigation: rebuild the window proactively when `content_rows` changes. Add a check in `repaint()`: if `fin_cache_.window_height != content_rows && !fin_cache_.rendered.empty()`, recompute the window fields from `fin_cache_.rendered` and `fin_cache_.row_count` (no re-render needed, just rerun `find_window_offset` + update fields). This is O(fin_rendered.size()) but only fires on resize, which is rare.

## Updated mixed-path in `repaint()`

Replace the G3 mixed path with:

```cpp
} else {
    // Mixed path: tail_rows < content_rows — need some finalized rows.
    const int need_fin_rows = content_rows - tail_rows;

    // Proactively rebuild window if content_rows changed since last cache build.
    if (!fin_cache_.rendered.empty() && fin_cache_.window_height != content_rows) {
        fin_cache_.window_height = content_rows;
        if (fin_cache_.row_count > content_rows) {
            const int target           = fin_cache_.row_count - content_rows;
            fin_cache_.window_offset    = find_window_offset(fin_cache_.rendered, w, target);
            fin_cache_.window_row_start = target;
            fin_cache_.window_rows      = fin_cache_.row_count - target;
        } else {
            fin_cache_.window_offset    = 0;
            fin_cache_.window_row_start = 0;
            fin_cache_.window_rows      = fin_cache_.row_count;
        }
    }

    if (fin_cache_.window_height == content_rows
        && fin_cache_.window_rows >= need_fin_rows) {
        // ── Window hit: slice fin suffix + append tail ─────────────────────
        // If window has more rows than needed, advance within the window to
        // the right starting '\n' boundary.
        size_t slice_offset = fin_cache_.window_offset;
        if (fin_cache_.window_rows > need_fin_rows) {
            const int extra = fin_cache_.window_rows - need_fin_rows;
            slice_offset = find_window_offset(
                fin_cache_.rendered, w,
                fin_cache_.window_row_start + extra);
        }

        const std::string_view fin_suffix(
            fin_cache_.rendered.data() + slice_offset,
            fin_cache_.rendered.size() - slice_offset);

        std::string combined;
        combined.reserve(fin_suffix.size() + tail_rendered.size());
        combined.append(fin_suffix);
        combined += tail_rendered;

        // split_lines on a bounded string: O(fin_suffix + tail) = O(viewport)
        auto lines = split_lines(combined, w);
        const int total = static_cast<int>(lines.size());
        const int first = std::max(0, total - content_rows);
        frame.reserve(combined.size() + static_cast<std::size_t>(content_rows) * 8);
        for (int i = first; i < std::min(total, first + content_rows); ++i) {
            frame += lines[static_cast<std::size_t>(i)];
            if (i + 1 < std::min(total, first + content_rows)) frame += "\r\n";
        }

    } else {
        // ── Window miss: fall back to full combined split (same as G3) ──────
        std::string combined;
        combined.reserve(fin_cache_.rendered.size() + tail_rendered.size());
        combined += fin_cache_.rendered;
        combined += tail_rendered;

        auto lines = split_lines(combined, w);
        const int total = static_cast<int>(lines.size());
        const int first = std::max(0, total - content_rows);
        frame.reserve(combined.size() + static_cast<std::size_t>(content_rows) * 8);
        for (int i = first; i < std::min(total, first + content_rows); ++i) {
            frame += lines[static_cast<std::size_t>(i)];
            if (i + 1 < std::min(total, first + content_rows)) frame += "\r\n";
        }
    }
}
```

## Correctness

**ANSI state**: `slice_offset` is always at a byte immediately after `\n` (by construction of `find_window_offset`). Markdown blocks end their ANSI styling before `\n` (`\033[0m` is emitted before `\n\n` at paragraph ends). So `fin_suffix` always starts at a clean ANSI state. ✓

**Row count guarantee**: `fin_suffix` starts at the `\n`-boundary closest to `need_fin_rows` rows before the end of finalized content. `split_lines(combined)` may produce slightly more than `content_rows` rows if a source line wraps, but `first = max(0, total - content_rows)` handles this correctly. ✓

**`window_rows < need_fin_rows` fallback**: Can occur when `fin_cache_.row_count < need_fin_rows` (session not long enough to fill viewport). The fallback produces the same output as G3's mixed path. ✓

## Performance summary (post G1-G4)

| Scenario | Per-delta cost (rendering) | Per-delta cost (line-splitting) |
|---|---|---|
| Boundary advances | O(content.size()) cmark — one-time | O(fin.size()) — one-time window compute |
| Fast path: tail fills viewport | O(tail.size()) cmark | O(tail.size()) split_lines |
| Mixed path: window hit | O(tail.size()) cmark | O(fin_suffix + tail) split_lines ≈ O(viewport) |
| Mixed path: window miss / resize | O(tail.size()) cmark | O(fin + tail) split_lines — fallback |

The dominant per-delta cost (cmark parse) is O(tail.size()) on all hot paths. The document-length walk is fully amortized to boundary-advance events.

## Constraints

- `find_window_offset` must use identical ANSI/UTF-8 handling as `split_lines` — uses the same `skip_ansi_sequence`, `advance_utf8`, `codepoint_width` primitives.
- `window_offset` must always point to either `0` or a byte immediately following a `\n` in `rendered`. Verify by assertion in debug builds: `window_offset == 0 || rendered[window_offset - 1] == '\n'`.
- No behavior change for G3's fast path or for G2 when `fin_cache_.rendered.empty()`.
- C++23, no warnings.

## Estimated size

~60 LOC: `find_window_offset` helper (~25 lines), window compute in cache-miss branch (~12 lines), proactive rebuild check (~10 lines), window-hit branch (~25 lines). Fallback is G3 code reused.

## Deferral note

G4 is the most complex phase. If profiling after G2+G3 shows the mixed-path `split_lines` is not a bottleneck (because `fin_rendered` stays small in typical sessions, or because the fast path fires most of the time), G4 may not be worth the complexity. Ship G2+G3 first and profile before committing to G4.
