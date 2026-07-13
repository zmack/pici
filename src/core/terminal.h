#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace pi::core {

int term_width(int fd);
int term_height(int fd);

// Return the display-column width of one terminal line, ignoring ANSI/VT
// escape sequences.
int display_columns(std::string_view line);

// Truncate one ANSI/VT line to `width` display columns without cutting a
// multibyte character or escape sequence. Embedded newlines are discarded.
std::string truncate_ansi_line(std::string_view line, int width);

// Set the terminal tab/window title. No bytes are written when fd is not a
// TTY. Control characters that could escape the OSC sequence are replaced.
void set_terminal_title(int fd, std::string_view title);

// Skip one ANSI/VT escape sequence starting at s[i].
//
// Handles:
//   CSI  \033[...X   — parameter bytes 0x30-0x3F, intermediate 0x20-0x2F,
//                       final byte 0x40-0x7E
//   OSC  \033]...ST  — terminated by ST (\033\\) or BEL (\007)
//   DCS/PM/APC       — \033[P^_] ... ST
//   Two-byte Fe      — \033 followed by a single byte in 0x40-0x5F that is
//                       not a multi-char introducer
//
// Returns the position after the sequence, or `i` if s[i] is not \033 or
// the sequence is malformed / unrecognised.
std::size_t skip_ansi_sequence(std::string_view s, std::size_t i);

// Advance past one UTF-8 codepoint at s[i].
// Returns the byte position after the codepoint, clamped to s.size().
// If the byte sequence is truncated (partial codepoint at end of string),
// returns s.size() so the caller can safely stop.
std::size_t advance_utf8(std::string_view s, std::size_t i);

// Display-column width of the UTF-8 codepoint at s[i].
//   0  — non-printing or combining (control chars, combining diacritics)
//   1  — narrow (most Latin, Cyrillic, Greek, etc.)
//   2  — wide (CJK ideographs, Hangul, fullwidth forms, most emoji)
//
// Based on Unicode Standard Annex #11 East Asian Width; covers the ranges
// most commonly produced by LLM output. Not locale-dependent.
int codepoint_width(std::string_view s, std::size_t i);

// Visual terminal rows occupied by `line` (no embedded \\n) at `width` cols.
// Accounts for ANSI/VT escapes, UTF-8, and wide characters.
int rows_for_line(std::string_view line, int width);

// Total visual rows from the start of `rendered` to the end at `width` cols.
// Counts every physical row, including those caused by terminal wrapping.
int cursor_rows_for_rendered(std::string_view rendered, int width);

// ── BlockBoundaryScanner
// ──────────────────────────────────────────────────────
//
// Incrementally scans raw markdown content (the append-only `content` string
// that ViewportRenderer builds from thinking + raw buffers) to find the last
// "stable block boundary" — a byte offset p where:
//   1. content[p-2..p-1] == "\n\n"
//   2. We are not inside an open code fence at that position.
//   3. The last completed non-blank line before the blank line had indent == 0.
//
// Call advance() with the full growing content string on each update; only
// bytes from scan_pos onward are processed (O(new bytes) per call).
// Reset with `= {}` between turns or when the prefix becomes stale.
struct BlockBoundaryScanner {
  std::size_t scan_pos{0};    // bytes consumed in prior advance() calls
  std::size_t last_stable{0}; // last confirmed stable boundary offset

  // Fence tracking (persists across advance() calls)
  bool inside_fence{false};
  char fence_char{0}; // '`' or '~' (valid when inside_fence)
  int fence_len{0};   // opening run length (≥3 when inside_fence)

  // Current-line analysis (reset at each '\n')
  int line_indent{0};         // leading spaces/tabs before first non-space
  bool saw_nonspace{false};   // have we seen a non-space char on this line
  int fence_run{0};           // consecutive fence_run_char at line start
  char fence_run_char{0};     // the char being counted
  bool line_only_fence{true}; // no non-fence, non-space chars after the run yet

  // Cross-line state
  bool last_nonblank_col0{
      true};                // last completed non-blank line had indent == 0
  bool last_char_nl{false}; // previous byte processed was '\n'

  void advance(std::string_view s);
};

} // namespace pi::core
