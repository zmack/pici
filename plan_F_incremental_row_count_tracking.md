# Plan F — DiffMarkdownRenderer: incremental row-count tracking

## Goal
Eliminate the O(N²) cost of `cursor_rows_for_rendered` calls in `DiffMarkdownRenderer::on_text_delta`. Today, on every delta we call `cursor_rows_for_rendered` on the *entire* rendered buffer up to three times (`prev_cursor_rows_`, `committed_rows_`, and the structural-fallback recompute), and each call is O(rendered.size()). Across an N-byte session that is O(N²) work just for row counting. Replace these full-buffer scans with incremental updates: track the cursor column at the end of the previous render, and on each delta scan only the *new* bytes (the appended suffix, or the newly-committed slice). Fall back to a bounded recompute on the structural-change path, never on the full buffer.

## Reference
- `src/core/stream_renderer.cpp:97-198` — `DiffMarkdownRenderer::on_text_delta`; the three full-buffer `cursor_rows_for_rendered` calls live at lines 113, 124, 168, 194-195, 197.
- `src/core/stream_renderer.cpp:208-221` — `clear_state()` and member declarations; the place to add `prev_col_` and to reset it.
- `src/core/stream_renderer.cpp:230-249` — `find_commit_boundary` and `advance_commit`; `advance_commit` is the second site that calls `cursor_rows_for_rendered` on the full prefix-up-to-commit and is replaced by an incremental update.
- `src/core/terminal.h:43` — `cursor_rows_for_rendered(std::string_view, int)`; signature unchanged. We add a sibling helper.
- `src/core/terminal.cpp:176-191` — body of `cursor_rows_for_rendered`. The new helper is a near-clone with two extra parameters: a starting column and an out-column.

## Current state assumptions
- `DiffMarkdownRenderer` has three integer state fields tracking on-screen geometry:
  - `committed_bytes_` — bytes of `prev_rendered_` that are committed (already in scrollback).
  - `committed_rows_`  — visual rows the committed prefix occupies.
  - `prev_cursor_rows_`— total visual rows of `prev_rendered_`.
- `prev_rendered_` only mutates between deltas and is replaced wholesale at the end of each `on_text_delta`.
- `committed_bytes_` is monotonically non-decreasing within a turn (`advance_commit` only grows it; `clear_state` resets to 0 between turns).
- The fast path (lines 104-115) only fires when the new render strictly extends `prev_rendered_` byte-for-byte, so the suffix `rendered.substr(prev_rendered_.size())` is exactly the new content appended after the previous final cursor position.
- The structural path (lines 140-198) can shorten or rewrite the live tail; in the worst case it falls back to `live_start = 0`. Plan C (incremental render, out of scope here) bounds the live region to a single open block, so the live portion is small in steady state.
- `cursor_rows_for_rendered` returns `1` for the empty input (counts the row the cursor is sitting on). Any incremental delta must preserve that invariant: rows should equal `cursor_rows_for_rendered(full, w)` exactly.
- ANSI escape sequences are zero-width and do not change column. Newlines reset column to 0 and increment rows. Wide chars contribute 2 columns. These are the same rules `cursor_rows_for_rendered` already encodes; the incremental helper must match them byte-for-byte.

## Deliverables

### 1. New helper in `terminal.h` / `terminal.cpp`
Add an incremental row-counting helper that takes a starting column and returns both the number of additional rows produced and the column at the end. Signature:

```cpp
// Incremental cursor-row accounting for a slice rendered to a terminal that
// is already at column `start_col` of its current row.
//
// Inputs:
//   slice      — bytes about to be written (may include ANSI escapes, '\n',
//                wide chars).
//   width      — terminal width in columns. Width <= 0 → returns {0, 0}.
//   start_col  — current cursor column (0 ≤ start_col; if start_col >= width
//                the caller should normalize first, but the helper tolerates
//                start_col == 0..width-1 as the well-defined input range).
//
// Outputs:
//   added_rows — number of *additional* rows the cursor advances by while
//                consuming `slice`. Equivalently, the row delta. A slice that
//                stays on the current row contributes 0.
//   end_col    — cursor column after the slice (0 ≤ end_col < width).
//
// Semantics match cursor_rows_for_rendered: ANSI escapes are zero-width,
// '\n' resets col to 0 and adds a row, codepoints contribute codepoint_width,
// and reaching `width` wraps (col := 0, ++rows).
//
// Invariant we rely on:
//   cursor_rows_for_rendered(a + b, w)
//     == cursor_rows_for_rendered(a, w)
//        + cursor_rows_for_suffix(b, w, col_after(a, w)).added_rows
// where col_after(a, w) is end_col of cursor_rows_for_suffix(a, w, 0).
struct SuffixRows { int added_rows; int end_col; };
SuffixRows cursor_rows_for_suffix(std::string_view slice, int width,
                                  int start_col);
```

Implementation (parallel to `cursor_rows_for_rendered` but starts from `start_col` and counts deltas, not absolute rows):

```cpp
SuffixRows cursor_rows_for_suffix(std::string_view slice, int width,
                                  int start_col) {
  if (width <= 0) return {0, 0};
  int col   = std::clamp(start_col, 0, width - 1);
  int added = 0;
  for (std::size_t i = 0; i < slice.size();) {
    if (slice[i] == '\033') {
      const auto next = skip_ansi_sequence(slice, i);
      if (next > i) { i = next; continue; }
    }
    if (slice[i] == '\n') { ++added; col = 0; ++i; continue; }
    const int cw = codepoint_width(slice, i);
    i = advance_utf8(slice, i);
    col += cw;
    if (col >= width) { ++added; col = 0; }
  }
  return {added, col};
}
```

Notes:
- This counts **deltas**, not absolute rows. `cursor_rows_for_rendered(s, w)` is `1 + cursor_rows_for_suffix(s, w, 0).added_rows`.
- The `>= width` wrap rule matches `cursor_rows_for_rendered` exactly (line 188 in `terminal.cpp`).
- `start_col` is clamped defensively; in practice we only ever pass a value the helper itself produced, so it will be in `[0, width-1]`.

### 2. New `prev_col_` field on `DiffMarkdownRenderer`
Add the column at the end of `prev_rendered_` to the renderer's state:

```cpp
int prev_col_{0}; // cursor column at the END of prev_rendered_ (0..w-1)
```

Reset in `clear_state()`:

```cpp
void clear_state() {
  text_buffer_.clear();
  prev_rendered_.clear();
  committed_bytes_  = 0;
  committed_rows_   = 0;
  prev_cursor_rows_ = 0;
  prev_col_         = 0;   // ← new
  committed_col_    = 0;   // ← new (see deliverable 4)
}
```

### 3. Fast path: incremental update of `prev_cursor_rows_` and `prev_col_`
Replace lines 104-115:

```cpp
if (!prev_rendered_.empty() &&
    rendered.size() >= prev_rendered_.size() &&
    rendered.compare(0, prev_rendered_.size(), prev_rendered_) == 0) {
  const auto suffix = rendered.substr(prev_rendered_.size());
  if (!suffix.empty()) {
    ::write(fd_, suffix.data(), suffix.size());
    advance_commit(rendered, w);                     // see deliverable 4
  }
  // Incremental row/col update — O(suffix.size()), not O(rendered.size()).
  const auto sr = cursor_rows_for_suffix(suffix, w, prev_col_);
  prev_cursor_rows_ += sr.added_rows;
  prev_col_          = sr.end_col;
  prev_rendered_     = std::move(rendered);
  return;
}
```

Removed: `prev_cursor_rows_ = cursor_rows_for_rendered(prev_rendered_, w);`.

### 4. First-write path: seed `prev_col_` from the initial render
At lines 117-126:

```cpp
if (prev_rendered_.empty()) {
  if (!rendered.empty()) {
    ::write(fd_, rendered.data(), rendered.size());
    advance_commit(rendered, w);
  }
  // First render — seed from start_col=0.
  const auto sr = cursor_rows_for_suffix(rendered, w, 0);
  prev_cursor_rows_ = 1 + sr.added_rows;
  prev_col_         = sr.end_col;
  prev_rendered_    = std::move(rendered);
  return;
}
```

This replaces the full `cursor_rows_for_rendered(prev_rendered_, w)` call. Result is identical by the additive invariant.

### 5. Incremental `committed_rows_` via a `committed_col_` field
`committed_bytes_` is monotonically non-decreasing within a turn, so `committed_rows_` can be tracked incrementally just like `prev_cursor_rows_`. Add a sibling field:

```cpp
int committed_col_{0}; // cursor column at the END of the committed prefix
```

Reset in `clear_state()` (already shown above).

Modify `advance_commit` (lines 242-249) to compute the increment over only the newly-committed slice:

```cpp
void advance_commit(const std::string &rendered, int w) {
  auto candidate = find_commit_boundary(rendered, committed_bytes_);
  if (candidate > committed_bytes_) {
    const std::string_view new_slice(
        rendered.data() + committed_bytes_, candidate - committed_bytes_);
    const auto sr = cursor_rows_for_suffix(new_slice, w, committed_col_);
    committed_rows_  += sr.added_rows;
    committed_col_    = sr.end_col;
    committed_bytes_  = candidate;
  }
}
```

Removed: `committed_rows_ = cursor_rows_for_rendered(rendered.substr(0, committed_bytes_), w);`.

### 6. Structural-change path: bounded recompute, not full-buffer
Lines 140-198 are the structural path. Two `cursor_rows_for_rendered` calls live here:

- **Line 168** — `live_rows = cursor_rows_for_rendered(rv.substr(0, live_start), w);`
  This recomputes the row count of the new rendered prefix up to the new `live_start`. `live_start` is at most the end of the last `\n\n` boundary before `shared`. There is no clean monotonic relationship with the previous `committed_rows_` (the new render diverges before the old commit point), so we **cannot** safely incrementalize this. Keep the full-prefix call here, but scope it: it only runs when `shared < committed_bytes_`, which is the rare "committed content was wrong" branch. Acceptable.

  However: ALSO re-seed `committed_col_` on this branch, because we are about to overwrite `committed_bytes_` and `committed_rows_` with values derived from `rv` (not `prev_rendered_`). Track the column the same way as the rows.

  Replace line 168 with a single forward pass that yields both rows and column:

  ```cpp
  if (shared < committed_bytes_) {
    live_start = 0;
    for (std::size_t s = 0; s + 1 < shared; ++s) {
      if (rv[s] == '\n' && rv[s + 1] == '\n')
        live_start = s + 2;
    }
    const auto sr = cursor_rows_for_suffix(rv.substr(0, live_start), w, 0);
    live_rows      = 1 + sr.added_rows;
    committed_col_ = sr.end_col;        // re-seed for the new prefix
  }
  ```

- **Lines 194-195** — `committed_rows_ = cursor_rows_for_rendered(rendered.substr(0, committed_bytes_), w);`
  After the structural redraw, `committed_bytes_` was just set to `new_commit`. There are two sub-cases:

  1. **`new_commit == live_start` (no new commit)**: nothing changed; do not recompute. `committed_rows_` and `committed_col_` keep their values, which were either preserved (the common branch where `shared >= committed_bytes_`) or re-seeded (the rebase branch above set them to match `rv.substr(0, live_start)`).
  2. **`new_commit > live_start`**: a slice from `live_start..new_commit` has just been committed. Use `cursor_rows_for_suffix` on that slice starting from `committed_col_`:
     ```cpp
     if (new_commit > live_start) {
       const std::string_view slice(
           rendered.data() + live_start, new_commit - live_start);
       const auto sr = cursor_rows_for_suffix(slice, w, committed_col_);
       committed_rows_ += sr.added_rows;
       committed_col_   = sr.end_col;
     }
     committed_bytes_ = new_commit;
     ```
  Removed: `committed_rows_ = cursor_rows_for_rendered(...);`.

- **Line 197** — `prev_cursor_rows_ = cursor_rows_for_rendered(prev_rendered_, w);`
  The structural path always rewrites the live tail by writing `rendered.data() + live_start` to the screen after the cursor moved to row `live_rows` and column 0. So after the write the cursor is at:
  ```
  rows = live_rows + cursor_rows_for_suffix(slice_written, w, 0).added_rows
  col  =             cursor_rows_for_suffix(slice_written, w, 0).end_col
  ```
  where `slice_written = rendered[live_start..end]`. Compute this once, reuse for both `prev_cursor_rows_` and `prev_col_`:
  ```cpp
  const std::string_view live_slice(
      rendered.data() + live_start, rendered.size() - live_start);
  const auto live_sr = cursor_rows_for_suffix(live_slice, w, 0);
  prev_cursor_rows_  = live_rows + live_sr.added_rows;
  prev_col_          = live_sr.end_col;
  prev_rendered_     = std::move(rendered);
  ```
  Removed: `prev_cursor_rows_ = cursor_rows_for_rendered(prev_rendered_, w);`.

  This pass is bounded by `rendered.size() - live_start` — i.e. the live region. Combined with Plan C (incremental render that bounds the live region by the open block size), the structural path becomes O(open_block) per delta.

### 7. Summary of which paths use which strategy

| Path                                       | Old cost                  | New cost                    | Strategy        |
|--------------------------------------------|---------------------------|-----------------------------|-----------------|
| Fast path `prev_cursor_rows_`              | O(rendered)               | O(suffix)                   | Incremental     |
| Fast path `committed_rows_` (advance_commit) | O(committed_bytes)      | O(new_commit - committed_bytes) | Incremental |
| First write `prev_cursor_rows_`            | O(rendered)               | O(rendered) (one-time)      | Incremental from col=0 |
| First write `committed_rows_`              | O(committed_bytes)        | O(new_commit)               | Incremental     |
| Structural `live_rows` (rebase branch)     | O(live_start)             | O(live_start)               | Bounded recompute (rare) |
| Structural `committed_rows_`               | O(committed_bytes)        | O(new_commit - live_start)  | Incremental from re-seeded col |
| Structural `prev_cursor_rows_`             | O(rendered)               | O(live_slice)               | Bounded recompute (live region only) |

No path scans the full rendered buffer per delta any more. Per-session cost drops from O(N²) to O(N) plus the per-delta render cost (which Plan C addresses separately).

## Constraints
- Do **not** change the signature of `cursor_rows_for_rendered`. Add `cursor_rows_for_suffix` as a sibling.
- Do **not** change the public `Renderer` interface, `dispatch_event`, or any other renderer's behavior.
- The new `cursor_rows_for_suffix` must be byte-equivalent to `cursor_rows_for_rendered` under the additive invariant:
  `cursor_rows_for_rendered(a + b, w) == cursor_rows_for_rendered(a, w) + cursor_rows_for_suffix(b, w, col_after(a, w)).added_rows`
  for all `a`, `b`, `w > 0`.
- `prev_col_` and `committed_col_` must always satisfy `0 <= col < w`. Verify by construction (the helper enforces this on every iteration via the `>= width` wrap).
- `clear_state()` and `on_turn_start()` must reset both new fields to 0.
- Do not let the structural path's recomputes scan more than the live region (post-`live_start` slice). The pre-`live_start` recompute is acceptable only on the rare rebase branch (`shared < committed_bytes_`), and even there it scans `live_start` bytes — bounded by the last `\n\n` before the divergence point, not the whole buffer.
- No allocation in the hot incremental path (the helper is a plain loop over a `string_view`).
- C++23, no warnings.

## Done criteria
1. `cursor_rows_for_suffix` exists in `terminal.h` and `terminal.cpp`, returns `{added_rows, end_col}`, and matches `cursor_rows_for_rendered` under the additive invariant on a fuzz-style test set covering: empty, ASCII only, ANSI escapes, UTF-8 narrow, wide chars, embedded `\n`, lines exactly `width` wide, lines wider than `width`. (Add a small unit test under `test/` if a terminal-test file already exists; otherwise verify by inspection — do **not** add a new test infrastructure as part of this plan.)
2. `DiffMarkdownRenderer` has `prev_col_` and `committed_col_` fields, both initialized to 0 and reset by `clear_state()`.
3. `on_text_delta` no longer calls `cursor_rows_for_rendered` on `prev_rendered_` or on a full prefix slice. The only remaining `cursor_rows_for_rendered` call site (if any) is gone — the structural path uses `cursor_rows_for_suffix` for everything except the bounded `rv.substr(0, live_start)` rebase recompute, which has been rewritten as a `cursor_rows_for_suffix(rv.substr(0, live_start), w, 0)` call (so it also uses the new helper).
4. `advance_commit` updates `committed_rows_` and `committed_col_` using `cursor_rows_for_suffix` over the new slice only.
5. Per-delta cost of row-count updates is bounded by `suffix.size()` on the fast path and by `live_slice.size()` on the structural path. No path is O(rendered.size()) per delta.
6. End-to-end behavior is unchanged: rendered output, cursor position after each delta, scrollback content. Sanity check by running an existing live-rendering session and confirming no visual regression.
7. Existing tests (if any cover terminal/row counting) pass without modification.
8. Optional self-check assertion (debug-only, easy to leave in): after each `on_text_delta`, in a debug build, `assert(prev_cursor_rows_ == cursor_rows_for_rendered(prev_rendered_, w));` and the analogous assertion for `committed_rows_`. Drop or `#ifdef NDEBUG`-guard before merge.

## Estimated size
~120 LOC net added: ~30 lines for `cursor_rows_for_suffix` (header + impl), ~10 lines of new fields/resets, ~80 lines of edits across the three paths in `on_text_delta` and `advance_commit`. Roughly 20 lines removed (the four `cursor_rows_for_rendered` call sites). Net ≈ +100 LOC.
