# Plan G3 — Skip-finalized fast path + row count tracking

## Goal

When the tail block's rendered content spans ≥ `content_rows` visual rows, the entire visible viewport is within the tail. In that case, skip `fin_cache_.rendered` entirely — no concatenation, no `split_lines` walk over the (potentially large) finalized prefix. Also add `row_count` to `FinCache` to track how many rows finalized content occupies (needed for the mixed-case path and as foundation for G4).

## Reference

- Plan G2 — prerequisite; this plan extends G2's `repaint()` and `FinCache`.
- `src/core/stream_renderer.cpp:463-485` — `split_lines()`; called for both tail and combined.
- `src/core/terminal.h:43` — `cursor_rows_for_rendered()`; alternative to `split_lines` for row counting only (no allocation). Can be used in place of `split_lines(...).size()` for the row-count computation. Prefer it here since we only need the count, not the lines.

## Changes to `FinCache`

Extend the G2 struct with one new field:

```cpp
struct FinCache {
    size_t      raw_end{0};
    std::string rendered;
    int         width{0};
    int         row_count{0};   // NEW: total visual rows of `rendered` at `width`
};
```

`row_count` is set in the cache-miss branch of `repaint()` at the same time `rendered` is populated.

## Updated cache-miss branch in `repaint()`

Replace the cache-miss block in G2 with:

```cpp
if (boundary > fin_cache_.raw_end) {
    const std::string full_rendered = render_visible_markdown(content);

    size_t rendered_boundary = 0;
    for (size_t i = 0; i + 1 < full_rendered.size(); ++i) {
        if (full_rendered[i] == '\n' && full_rendered[i + 1] == '\n') {
            rendered_boundary = i + 2;
        }
    }

    fin_cache_.rendered   = full_rendered.substr(0, rendered_boundary);
    fin_cache_.raw_end    = boundary;
    fin_cache_.width      = w;
    // Count rows of finalized content: O(fin_rendered.size()), happens only here.
    fin_cache_.row_count  = cursor_rows_for_rendered(fin_cache_.rendered, w);

    tail_rendered = full_rendered.substr(rendered_boundary);
}
```

`cursor_rows_for_rendered` is O(fin_rendered.size()) but runs only when the boundary advances — amortized rare.

## New branching logic in `repaint()`

Replace the "Build combined" section (the `split_lines` + frame-build block) with:

```cpp
// Count tail rows: O(tail_rendered.size()), always needed.
const int tail_rows = cursor_rows_for_rendered(tail_rendered, w);

std::string frame;
frame += "\033[H\033[J";

if (tail_rows >= content_rows) {
    // ── Fast path: entire visible viewport is in the tail ──────────────────
    // fin_cache_.rendered is completely off-screen; skip its split entirely.
    auto tail_lines_vec = split_lines(tail_rendered, w);
    const int total = static_cast<int>(tail_lines_vec.size());
    const int first = total - content_rows;   // tail_rows >= content_rows so first >= 0
    frame.reserve(tail_rendered.size() + static_cast<std::size_t>(content_rows) * 8);
    for (int i = first; i < total; ++i) {
        frame += tail_lines_vec[static_cast<std::size_t>(i)];
        if (i + 1 < total) frame += "\r\n";
    }

} else {
    // ── Mixed / short path: need some finalized rows + all tail rows ────────
    // G4 will optimize this case. For now: simple concatenation + split.
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

::write(fd_, frame.data(), frame.size());
paint_status();
```

## Correctness analysis

**Fast path** (`tail_rows >= content_rows`):
- `tail_lines_vec` is produced by `split_lines(tail_rendered, w)`, the same function used in the original code.
- `first = total - content_rows` is non-negative because `tail_rows >= content_rows`.
- `i < total` (not `i < first + content_rows`) because `total - first = content_rows` exactly.
- Output is identical to what the old code would have shown (the last `content_rows` rows of the full combined render, which in this case are all within `tail_rendered`). ✓

**Mixed / short path** (unchanged from G2 Option A):
- Full concatenation + split, same correctness argument as G2.

**Off-by-one check** — `tail_rows >= content_rows` vs `> content_rows`:
- If `tail_rows == content_rows`: visible viewport is exactly the tail. `first = 0`, show all `content_rows` lines. Skipping finalized is correct. ✓
- If `tail_rows > content_rows`: viewport shows last `content_rows` of tail. Skipping finalized is correct. ✓
- `>=` is right.

## Performance impact

| Scenario | G2 cost | G3 cost |
|---|---|---|
| Long session, short tail (paragraph streaming) | O(fin) split_lines walk | O(tail) only if tail fills viewport; O(fin+tail) otherwise |
| Long code block streaming (tail fills viewport) | O(fin+tail) split_lines | O(tail) — fast path fires ✓ |
| Short session (no finalized content) | O(tail) | O(tail) — same |

The fast path benefits most for large code blocks or when viewing a long-running session. For typical paragraph streaming (5-10 visible lines of tail), the mixed path is taken and cost is O(fin+tail) — same as G2. G4 will optimize the mixed path.

## `cursor_rows_for_rendered` vs `split_lines(...).size()`

`cursor_rows_for_rendered(s, w)` walks the string and counts rows without allocating any `std::string` objects. `split_lines(s, w).size()` does the same work but allocates one `std::string` per row. Using `cursor_rows_for_rendered` for `tail_rows` avoids these allocations in the fast path (where we only need the count to decide which branch to take, before calling `split_lines` separately).

If Plan F (`cursor_rows_for_suffix`) has been implemented by the time G3 ships, it can replace `cursor_rows_for_rendered(tail_rendered, w)` with an incremental computation — but G3 does not require Plan F. Implement G3 with `cursor_rows_for_rendered` first.

## Constraints

- Do **not** change `split_lines` signature or behavior.
- Do **not** change `cursor_rows_for_rendered` signature.
- `fin_cache_.row_count` need not be used in G3's mixed path — it's only stored here for G4 to consume.
- No behavior change for `RawStreamRenderer` or `DiffMarkdownRenderer`.
- C++23, no warnings.

## Done criteria

1. `FinCache` has `row_count` field, populated in the cache-miss branch.
2. `repaint()` branches on `tail_rows >= content_rows`.
3. Fast path does not call `split_lines(combined, w)` — only `split_lines(tail_rendered, w)`.
4. Mixed path behavior is identical to G2.
5. For a session where the tail grows to ≥ `content_rows` lines (e.g., large code block), verify with manual smoke test that the viewport shows the correct tail with no visual regression.

## Estimated size

~30 LOC net change: add `row_count` field (~1 line), add one `cursor_rows_for_rendered` call in cache-miss branch (~2 lines), replace the `split_lines` block with the branching logic (~25 lines).
