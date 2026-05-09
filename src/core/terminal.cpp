#include "core/terminal.h"

#include <algorithm>
#include <cstdint>

namespace pi::core {
namespace {

// ── UTF-8 decode ─────────────────────────────────────────────────────────────

static char32_t decode_utf8(std::string_view s, std::size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if ((c & 0x80) == 0x00) return c;
  if ((c & 0xE0) == 0xC0) {
    char32_t cp = c & 0x1F;
    if (i + 1 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i+1]) & 0x3F);
    return cp;
  }
  if ((c & 0xF0) == 0xE0) {
    char32_t cp = c & 0x0F;
    if (i + 1 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i+1]) & 0x3F);
    if (i + 2 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i+2]) & 0x3F);
    return cp;
  }
  if ((c & 0xF8) == 0xF0) {
    char32_t cp = c & 0x07;
    if (i + 1 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i+1]) & 0x3F);
    if (i + 2 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i+2]) & 0x3F);
    if (i + 3 < s.size()) cp = (cp << 6) | (static_cast<unsigned char>(s[i+3]) & 0x3F);
    return cp;
  }
  return 0xFFFD;
}

// ── East Asian Width ─────────────────────────────────────────────────────────
// Ranges from Unicode Standard Annex #11 (Wide + Fullwidth categories).
// Covers the characters most commonly produced by LLM responses.

static bool is_wide(char32_t cp) {
  struct Range { char32_t lo, hi; };
  static constexpr Range kWide[] = {
    {0x1100, 0x115F},   // Hangul Jamo
    {0x2329, 0x232A},   // Angle brackets (wide)
    {0x2E80, 0x303E},   // CJK Radicals, Kangxi, Bopomofo, Hiragana preamble
    {0x3041, 0x33FF},   // Hiragana, Katakana, Bopomofo, Hangul compat, CJK compat
    {0x3400, 0x4DBF},   // CJK Extension A
    {0x4E00, 0x9FFF},   // CJK Unified Ideographs (the main block)
    {0xA000, 0xA4CF},   // Yi Syllables + Radicals
    {0xA960, 0xA97F},   // Hangul Jamo Extended-A
    {0xAC00, 0xD7AF},   // Hangul Syllables
    {0xF900, 0xFAFF},   // CJK Compatibility Ideographs
    {0xFE10, 0xFE1F},   // Vertical Forms
    {0xFE30, 0xFE6F},   // CJK Compatibility Forms + Small Forms
    {0xFF01, 0xFF60},   // Fullwidth ASCII + Halfwidth Katakana (fullwidth half)
    {0xFFE0, 0xFFE6},   // Fullwidth currency signs
    {0x1B000, 0x1B0FF}, // Kana Supplement
    {0x1F200, 0x1F2FF}, // Enclosed Ideographic Supplement
    {0x1F300, 0x1F64F}, // Misc Symbols, Dingbats, Emoticons
    {0x1F900, 0x1F9FF}, // Supplemental Symbols and Pictographs
    {0x1FA00, 0x1FAFF}, // Chess / Extended Pictographs
    {0x20000, 0x2A6DF}, // CJK Extension B
    {0x2A700, 0x2CEAF}, // CJK Extension C, D, E
    {0x2CEB0, 0x2EBEF}, // CJK Extension F
    {0x2F800, 0x2FA1F}, // CJK Compatibility Ideographs Supplement
    {0x30000, 0x3134F}, // CJK Extension G
  };
  for (const auto &r : kWide)
    if (cp >= r.lo && cp <= r.hi) return true;
  return false;
}

// Combining/zero-width characters — contribute 0 display columns.
static bool is_combining(char32_t cp) {
  struct Range { char32_t lo, hi; };
  static constexpr Range kCombining[] = {
    {0x0300, 0x036F},   // Combining Diacritical Marks
    {0x0483, 0x0489},
    {0x0591, 0x05BD},   // Hebrew cantillation
    {0x064B, 0x065F},   // Arabic combining
    {0x1DC0, 0x1DFF},   // Combining Diacritical Marks Supplement
    {0x20D0, 0x20FF},   // Combining Diacritical Marks for Symbols
    {0xFE20, 0xFE2F},   // Combining Half Marks
    {0xE0100, 0xE01EF}, // Variation Selectors Supplement
  };
  for (const auto &r : kCombining)
    if (cp >= r.lo && cp <= r.hi) return true;
  return false;
}

} // namespace

// ── Public API ────────────────────────────────────────────────────────────────

std::size_t skip_ansi_sequence(std::string_view s, std::size_t i) {
  if (i >= s.size() || s[i] != '\033') return i;
  if (i + 1 >= s.size()) return i;

  const char next = s[i + 1];

  // CSI: \033[ param* inter* final
  if (next == '[') {
    i += 2;
    while (i < s.size()) {
      const unsigned char c = static_cast<unsigned char>(s[i]);
      if (c >= 0x40 && c <= 0x7E) return i + 1; // final byte
      ++i;
    }
    return i; // truncated — consumed what we can
  }

  // OSC: \033] ... BEL  or  \033] ... ST (\033\\)
  if (next == ']') {
    i += 2;
    while (i < s.size()) {
      if (s[i] == '\007') return i + 1;
      if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '\\') return i + 2;
      ++i;
    }
    return i;
  }

  // DCS / PM / APC: \033[P^_] ... ST
  if (next == 'P' || next == '^' || next == '_') {
    i += 2;
    while (i < s.size()) {
      if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '\\') return i + 2;
      ++i;
    }
    return i;
  }

  // Two-byte Fe sequences: \033 + single byte in 0x40-0x5F
  // (excludes [, ], P, ^, _ which are multi-char introducers handled above)
  const unsigned char nb = static_cast<unsigned char>(next);
  if (nb >= 0x40 && nb <= 0x5F) return i + 2;

  return i; // unrecognised — don't skip
}

std::size_t advance_utf8(std::string_view s, std::size_t i) {
  if (i >= s.size()) return i;
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if ((c & 0x80) == 0x00) return i + 1;
  if ((c & 0xE0) == 0xC0) return std::min(i + 2, s.size());
  if ((c & 0xF0) == 0xE0) return std::min(i + 3, s.size());
  if ((c & 0xF8) == 0xF0) return std::min(i + 4, s.size());
  return i + 1; // invalid lead byte — step one byte
}

int codepoint_width(std::string_view s, std::size_t i) {
  if (i >= s.size()) return 0;
  const char32_t cp = decode_utf8(s, i);
  // Non-printing C0/C1 controls and DEL
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0)) return 0;
  if (is_combining(cp)) return 0;
  if (is_wide(cp)) return 2;
  return 1;
}

int rows_for_line(std::string_view line, int width) {
  if (width <= 0) return 1;
  int col = 0, rows = 1;
  for (std::size_t i = 0; i < line.size();) {
    if (line[i] == '\033') {
      const auto next = skip_ansi_sequence(line, i);
      if (next > i) { i = next; continue; }
    }
    const int cw = codepoint_width(line, i);
    i = advance_utf8(line, i);
    col += cw;
    if (col >= width) { ++rows; col = 0; }
  }
  return rows;
}

int cursor_rows_for_rendered(std::string_view rendered, int width) {
  if (width <= 0) return 1;
  int rows = 1, col = 0;
  for (std::size_t i = 0; i < rendered.size();) {
    if (rendered[i] == '\033') {
      const auto next = skip_ansi_sequence(rendered, i);
      if (next > i) { i = next; continue; }
    }
    if (rendered[i] == '\n') { ++rows; col = 0; ++i; continue; }
    const int cw = codepoint_width(rendered, i);
    i = advance_utf8(rendered, i);
    col += cw;
    if (col >= width) { ++rows; col = 0; }
  }
  return rows;
}

void BlockBoundaryScanner::advance(std::string_view s) {
  for (std::size_t i = scan_pos; i < s.size(); ++i) {
    const char c = s[i];

    if (c == '\n') {
      const bool blank = !saw_nonspace;

      // Two consecutive newlines ending a blank line = \n\n boundary candidate.
      // No i+1 < s.size() guard: the boundary IS stable even if nothing follows
      // yet; G2's cache will render an empty suffix (render_visible_markdown("")
      // returns "") harmlessly. The guard would silently drop boundaries across
      // incremental advance() calls.
      if (blank && last_char_nl && !inside_fence && last_nonblank_col0)
        last_stable = i + 1;

      if (!blank) {
        last_nonblank_col0 = (line_indent == 0);

        // Fence open/close detection.
        // Opener: line_indent ≤ 3, run ≥ 3 backticks/tildes.
        //   Info string (e.g. "py" in ```py) is OK — line_only_fence may be false.
        // Closer: same char, run ≥ opening length, and line_only_fence (no
        //   non-fence, non-space content — trailing spaces are allowed).
        if (line_indent <= 3 && fence_run >= 3) {
          if (!inside_fence) {
            inside_fence   = true;
            fence_char     = fence_run_char;
            fence_len      = fence_run;
          } else if (fence_run_char == fence_char &&
                     fence_run >= fence_len &&
                     line_only_fence) {
            inside_fence   = false;
            fence_char     = 0;
            fence_len      = 0;
          }
        }
      }

      last_char_nl    = true;
      line_indent     = 0;
      saw_nonspace    = false;
      fence_run       = 0;
      fence_run_char  = 0;
      line_only_fence = true;

    } else {
      last_char_nl = false;

      if (!saw_nonspace) {
        if (c == ' ' || c == '\t') {
          ++line_indent;
        } else {
          saw_nonspace = true;
          // Start fence-run tracking only if indent is small enough.
          // Tab is counted as 1 indent unit (conservative; CommonMark uses 4
          // columns for tab, but LLM output rarely has tab-indented fences).
          if (line_indent <= 3 && (c == '`' || c == '~')) {
            fence_run      = 1;
            fence_run_char = c;
            line_only_fence = true;
          } else {
            fence_run      = 0;
            line_only_fence = false;
          }
        }
      } else if (line_only_fence) {
        if (c == fence_run_char) {
          ++fence_run;
        } else if (c != ' ') {
          // Non-fence, non-space char: this line is not a pure fence line.
          // Spaces after the run are legal on closing fence lines (and harmless
          // on openers), so we keep line_only_fence=true for spaces.
          line_only_fence = false;
        }
      }
    }
  }
  scan_pos = s.size();
}

} // namespace pi::core
