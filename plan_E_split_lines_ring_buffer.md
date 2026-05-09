# Plan E — ViewportRenderer: bounded-window line collection via ring buffer

## Goal
Stop materializing every physical row of the rendered buffer in `ViewportRenderer::repaint()`. Today `split_lines` walks the entire rendered string and allocates one `std::string` per physical row, then `repaint()` discards all but the last `content_rows`. For long responses this is hundreds of allocations on every delta, 95%+ of which are thrown away. Replace this with a single forward pass that retains at most `content_rows` lines using a fixed-size ring buffer, so allocations are bounded by the visible viewport — not the total response size.

## Reference
- `src/core/stream_renderer.cpp:390-427` — `ViewportRenderer::repaint()`; calls `split_lines`, slices the tail, builds the frame.
- `src/core/stream_renderer.cpp:463-485` — `split_lines(std::string_view, int)`; the function we are leaving alone (signature preserved) and replacing the *use of* in `repaint()`.
- `src/core/terminal.h:20-35` — `skip_ansi_sequence`, `advance_utf8`, `codepoint_width`; the same primitives `split_lines` uses, available for the new helper.
- `src/core/stream_renderer.cpp:283-286` — `on_text_delta` calls `repaint()` on every delta. `repaint()` is therefore the hot path.
- `src/core/stream_renderer.cpp:262` — `ViewportRenderer` class; the new helper lives here as a private static (parallel to `split_lines`).

## Current state assumptions
- `split_lines` is only called from `repaint()`. No external callers.
- `repaint()` only ever uses the **last** `content_rows` entries of the returned vector — the leading entries are discarded.
- `content_rows = h - 2` and is typically 20–80; a long response can have hundreds to thousands of physical rows.
- The rendered string contains:
  - LF (`\n`) hard line breaks.
  - ANSI escape sequences that must pass through verbatim and contribute zero columns.
  - UTF-8 codepoints, some of width 2 (CJK, emoji), some of width 0 (combining marks).
- A trailing non-empty line with no terminating `\n` is a real physical row (matches current `split_lines` behavior at `stream_renderer.cpp:483`).
- `content_rows <= 0` is already short-circuited by `repaint()` and we keep that guard.
- We must NOT change the signature of `split_lines` (per the brief). The change is local to `ViewportRenderer`.

## Deliverables

### 1. New private static helper on `ViewportRenderer`
Add a single forward-pass helper that returns at most `content_rows` lines (the tail), using a ring buffer of pre-allocated `std::string` slots. Place it next to `split_lines` in the `ViewportRenderer` class.

```cpp
// Single forward pass that retains only the last `max_lines` physical rows.
// Equivalent to `split_lines(s, width)` then taking the tail of size
// min(total, max_lines), but allocates O(max_lines) strings instead of O(total).
//
// Returns: pair<vector<string> lines, bool truncated_from_front>
//   lines      — between 0 and max_lines entries, in order (oldest → newest).
//   truncated  — true iff the input had more physical rows than max_lines
//                (i.e. some leading rows were dropped). Currently unused by
//                the caller but cheap to surface for future "↑ N more" hints.
//
// Algorithm (ring buffer):
//   slots     : vector<string> of capacity max_lines, filled lazily.
//   write_idx : next slot to overwrite (0..max_lines-1).
//   count     : number of slots written so far (saturates at max_lines).
//   When a line completes, append a *moved-out* string into slots[write_idx],
//   then write_idx = (write_idx+1) % max_lines, count = min(count+1, max_lines).
//   At end, the oldest live line is at (write_idx) when count==max_lines,
//   else at index 0; iterate `count` lines starting from that index modulo
//   max_lines.
//
// Per-byte logic mirrors split_lines exactly:
//   • '\n'                  → finalize current line, reset col.
//   • '\033'                → skip_ansi_sequence; copy bytes verbatim into
//                             the current line; do not touch col.
//   • normal codepoint      → advance via advance_utf8; width via
//                             codepoint_width; if col + cw > width && col>0
//                             finalize line first; append bytes; col += cw;
//                             if col >= width finalize line, col = 0.
//   • trailing non-empty cur at EOF → finalize as one more line.
//
// This guarantees identical wrapping to split_lines.
static std::pair<std::vector<std::string>, bool>
tail_lines(std::string_view s, int width, int max_lines);
```

Implementation sketch:

```cpp
static std::pair<std::vector<std::string>, bool>
tail_lines(std::string_view s, int width, int max_lines) {
  std::vector<std::string> ring;
  if (max_lines <= 0 || width <= 0) return {std::move(ring), false};
  ring.resize(static_cast<std::size_t>(max_lines));
  std::size_t write_idx = 0;
  std::size_t count     = 0;
  bool        truncated = false;

  std::string cur;
  int col = 0;

  auto finalize = [&] {
    if (count == static_cast<std::size_t>(max_lines)) truncated = true;
    ring[write_idx] = std::move(cur);
    cur.clear();
    write_idx = (write_idx + 1) % static_cast<std::size_t>(max_lines);
    if (count < static_cast<std::size_t>(max_lines)) ++count;
    col = 0;
  };

  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\n') { finalize(); ++i; continue; }
    if (s[i] == '\033') {
      const auto nxt = skip_ansi_sequence(s, i);
      if (nxt > i) { cur.append(s.data() + i, nxt - i); i = nxt; continue; }
    }
    const int  cw  = codepoint_width(s, i);
    const auto nxt = advance_utf8(s, i);
    if (col + cw > width && col > 0) { finalize(); }
    cur.append(s.data() + i, nxt - i);
    col += cw;
    if (col >= width) { finalize(); }
    i = nxt;
  }
  if (!cur.empty()) { finalize(); }

  // Linearize into output in oldest→newest order.
  std::vector<std::string> out;
  out.reserve(count);
  std::size_t start = (count == static_cast<std::size_t>(max_lines))
                          ? write_idx
                          : 0;
  for (std::size_t k = 0; k < count; ++k) {
    out.emplace_back(std::move(
        ring[(start + k) % static_cast<std::size_t>(max_lines)]));
  }
  return {std::move(out), truncated};
}
```

Edge cases handled by construction:
- **Input shorter than viewport (`total < content_rows`)**: `finalize` runs `total` times, `count` stays at `total`, `start = 0`, output contains all lines from row 0 to row `total-1` in order. `truncated == false`.
- **Empty input**: loop body never executes, `cur` empty, `finalize` never runs, output is empty.
- **`content_rows == 0`**: caller already short-circuits in `repaint()`. As a defensive measure the helper returns empty when `max_lines <= 0`.
- **Trailing `\n`**: same as today — current `split_lines` at line 483 only finalizes a non-empty `cur`. We match: a trailing newline closes a line via the `\n` branch (which finalizes whatever `cur` was), and the empty `cur` afterward is not finalized. (Note: this preserves the existing observable behavior; do not "fix" it as part of this plan.)

### 2. Replace the `split_lines`+slice in `repaint()` with `tail_lines`
Edit `ViewportRenderer::repaint()` to use the new helper and to write rows directly into the frame string in ring order. The control flow stays identical otherwise.

```cpp
void repaint() {
  const int w = term_width(fd_);
  const int h = term_height(fd_);
  const int content_rows = h - 2;
  if (content_rows <= 0) return;

  std::string content;
  if (!thinking_buffer_.empty()) {
    content += "[thinking]\n";
    content += thinking_buffer_;
    content += "\n\n";
  }
  content += raw_buffer_;

  auto rendered = render_visible_markdown(content);

  auto [lines, truncated] = tail_lines(rendered, w, content_rows);
  (void)truncated; // reserved for future "↑ N more" indicator

  std::string frame;
  // Reservation: home/erase + per-line bytes (already includes ANSI) +
  // "\r\n" between lines. `rendered.size()` is a safe upper bound on payload.
  frame.reserve(8 + rendered.size() + lines.size() * 2);
  frame += "\033[H\033[J";

  for (std::size_t i = 0; i < lines.size(); ++i) {
    frame += lines[i];
    if (i + 1 < lines.size()) frame += "\r\n";
  }

  ::write(fd_, frame.data(), frame.size());
  paint_status();
}
```

Notes:
- The `frame.reserve` heuristic is intentionally a slight over-estimate; previously `repaint` also reserved roughly `rendered.size() + content_rows*8`. Same order of magnitude; keep one or the other, do not micro-tune.
- The previous code only emitted `\r\n` between lines (no trailing `\r\n`); the new code preserves that.
- `lines.size()` is bounded by `content_rows`, so the loop runs at most `content_rows` iterations regardless of input length.

### 3. Leave `split_lines` untouched
Keep `split_lines` (`stream_renderer.cpp:463-485`) exactly as-is. It is no longer called from `repaint()` after this change. Do not remove it in this plan — leaving it removes an API churn risk if a follow-up renderer wants the materialized form. A separate cleanup commit (out of scope here) can delete it later if it has no callers.

If a static-analysis or unused-function warning appears, suppress it with a comment (`// retained for future renderers`) — do not delete.

## Constraints
- Do **not** change the signature of `split_lines`.
- Do **not** change `Renderer` interface or `dispatch_event`.
- Do **not** change wrapping semantics — column accounting, ANSI passthrough, wide-char handling, and trailing-line behavior must match `split_lines` byte-for-byte for any input that fits in `content_rows`. (For inputs longer than `content_rows`, the visible suffix must equal `split_lines(s,w).back()… split_lines(s,w).back() - content_rows + 1` of the legacy output.)
- The new helper is a private static of `ViewportRenderer` (same scope as `split_lines`). Do not expose it in `terminal.h`; this is layout-specific to the viewport.
- Allocation budget: at most `content_rows` `std::string` allocations per `repaint()`, plus the `frame` string. No `std::vector<std::string>` of size proportional to total physical rows.
- Do not introduce dependencies beyond what `stream_renderer.cpp` already pulls in.
- No behavior change for `RawStreamRenderer` or `DiffMarkdownRenderer`.
- Compiles under C++23, no compiler warnings at the project's existing warning level.

## Done criteria
1. `tail_lines` exists as a private static in `ViewportRenderer` and is the only producer of physical rows used by `repaint()`.
2. `repaint()` no longer calls `split_lines` and no longer materializes more than `content_rows` `std::string` line buffers.
3. For an input with `total >= content_rows` physical rows, the visible frame after `repaint` is byte-identical to what the previous code produced (same final ANSI state, same wrapping, same `\r\n` separators, same `\033[H\033[J` prologue).
4. For an input with `total < content_rows` rows, the frame contains all `total` lines in order from row 0, matching previous behavior.
5. Empty `rendered` produces a frame of just `\033[H\033[J` (matches previous behavior).
6. `split_lines` remains in the file and still compiles; no callers are required.
7. Manual smoke test: paste a multi-thousand-byte response into the agent (or replay a recorded session); `repaint()` is called per delta and CPU per delta is dominated by `render_visible_markdown` / cmark — not by line splitting. (Verify with `perf record` or simple `printf` timing if needed; this is acceptance, not a deliverable.)
8. Existing unit tests under `test/` (if any cover the viewport) pass without modification.

## Estimated size
~80 LOC net added (≈70 lines for `tail_lines`, ≈10 lines edited in `repaint`); 0 LOC removed. `split_lines` retained.
