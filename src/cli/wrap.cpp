#include "cli/wrap.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace pi::cli {

std::size_t utf8_length(std::string_view text, std::size_t offset) {
  const auto first = static_cast<unsigned char>(text[offset]);
  if (first < 0x80U)
    return 1;
  if ((first & 0xE0U) == 0xC0U && offset + 1 < text.size())
    return 2;
  if ((first & 0xF0U) == 0xE0U && offset + 2 < text.size())
    return 3;
  if ((first & 0xF8U) == 0xF0U && offset + 3 < text.size())
    return 4;
  return 1;
}

std::uint32_t utf8_codepoint(std::string_view text, std::size_t offset,
                             std::size_t length) {
  const auto first = static_cast<unsigned char>(text[offset]);
  if (length == 1)
    return first;
  std::uint32_t value = first & ((1U << (8U - length - 1U)) - 1U);
  for (std::size_t i = 1; i < length; ++i) {
    value =
        (value << 6U) | (static_cast<unsigned char>(text[offset + i]) & 0x3FU);
  }
  return value;
}

int codepoint_width(std::uint32_t codepoint) {
  if (codepoint < 0x20U)
    return 0;
  if ((codepoint >= 0x300U && codepoint <= 0x36FU) ||
      (codepoint >= 0xFE00U && codepoint <= 0xFE0FU))
    return 0;
  if ((codepoint >= 0x1100U && codepoint <= 0x115FU) ||
      (codepoint >= 0x2329U && codepoint <= 0x232AU) ||
      (codepoint >= 0x2E80U && codepoint <= 0xA4CFU) ||
      (codepoint >= 0xAC00U && codepoint <= 0xD7A3U) ||
      (codepoint >= 0xF900U && codepoint <= 0xFAFFU) ||
      (codepoint >= 0xFE10U && codepoint <= 0xFE19U) ||
      (codepoint >= 0xFE30U && codepoint <= 0xFE6FU) ||
      (codepoint >= 0xFF00U && codepoint <= 0xFF60U) ||
      (codepoint >= 0x1F300U && codepoint <= 0x1FAFFU))
    return 2;
  return 1;
}

std::size_t ansi_escape_length(std::string_view text, std::size_t offset) {
  if (offset + 1 >= text.size() || text[offset] != '\033' ||
      text[offset + 1] != '[')
    return 0;
  auto end = offset + 2;
  while (end < text.size()) {
    const auto c = static_cast<unsigned char>(text[end++]);
    if (c >= '@' && c <= '~')
      return end - offset;
  }
  return 1;
}

bool is_wrap_space(char c) { return c == ' ' || c == '\t'; }

namespace {

struct Run {
  std::size_t end{0};
  std::size_t width{0};
};

// Scans forward from `offset`, which must already point at a byte matching
// `is_wrap_space(c) == want_whitespace` (callers normalize past any leading
// escapes first -- see plan_word_wrap), accumulating display width while
// consecutive non-escape bytes keep matching. ANSI escapes encountered
// along the way are skipped transparently (zero width, don't end the run).
//
// Note: an escape sequence that sits between the run's last matching byte
// and the first non-matching byte is bucketed into *this* run (its length
// is skipped as part of finding that the run has ended) rather than the
// next one. Since escapes are always still emitted verbatim by the replay
// loop that walks this plan (see InputRenderer::write_wrapped), and this
// only affects which side of an already-adjacent word/whitespace boundary
// a zero-width escape is nominally attributed to, this is a deliberate
// simplification rather than a correctness gap for the boundary itself.
Run scan_run(std::string_view text, std::size_t offset, bool want_whitespace) {
  Run run{.end = offset, .width = 0};
  std::size_t o = offset;
  while (o < text.size()) {
    if (const auto escape_length = ansi_escape_length(text, o);
        escape_length > 0) {
      o += escape_length;
      continue;
    }
    const bool ws = is_wrap_space(text[o]);
    if (ws != want_whitespace)
      break;
    if (ws) {
      ++o;
      run.width += 1;
    } else {
      const auto length = utf8_length(text, o);
      const auto width = codepoint_width(
          utf8_codepoint(text, o, std::min(length, text.size() - o)));
      o += length;
      run.width += static_cast<std::size_t>(std::max(width, 0));
    }
  }
  run.end = o;
  return run;
}

} // namespace

std::vector<WordWrapBreak> plan_word_wrap(std::string_view text,
                                          std::size_t columns,
                                          std::size_t start_column) {
  std::vector<WordWrapBreak> breaks;
  if (columns == 0 || text.empty())
    return breaks;

  std::size_t column = start_column;

  // Places a run of `width` display columns spanning text[start, end) (a
  // word or a whitespace run): if it fits at the current column, place it
  // in place; if it doesn't fit here but fits on a fresh row, break before
  // it; if it doesn't even fit a full fresh row, hard-wrap it
  // character-by-character -- the fallback for a token wider than the
  // whole row (e.g. a long URL).
  const auto place = [&](std::size_t start, std::size_t end,
                         std::size_t width) {
    if (width <= columns) {
      if (column > 0 && column + width > columns) {
        breaks.push_back({.content_end = start, .resume_offset = start});
        column = 0;
      }
      column += width;
      return;
    }
    if (column > 0) {
      breaks.push_back({.content_end = start, .resume_offset = start});
      column = 0;
    }
    std::size_t pos = start;
    std::size_t consumed = 0;
    while (pos < end) {
      if (const auto escape_length = ansi_escape_length(text, pos);
          escape_length > 0) {
        pos += escape_length;
        continue;
      }
      const auto length = utf8_length(text, pos);
      const auto w = static_cast<std::size_t>(
          std::max(codepoint_width(utf8_codepoint(
                       text, pos, std::min(length, text.size() - pos))),
                   0));
      if (consumed > 0 && consumed + w > columns) {
        breaks.push_back({.content_end = pos, .resume_offset = pos});
        consumed = 0;
      }
      consumed += w;
      pos += length;
    }
    column = consumed;
  };

  std::size_t offset = 0;
  while (offset < text.size()) {
    // Commit past any leading escapes before classifying/placing this
    // token. Since the replay loop always prints an escape sequence as
    // soon as it reaches one, regardless of any pending break, every break
    // below needs `content_end` to point at real (non-escape) content --
    // otherwise the replay loop's offset would skip straight past
    // content_end via its own escape handling and never see the break.
    while (true) {
      const auto escape_length = ansi_escape_length(text, offset);
      if (escape_length == 0)
        break;
      offset += escape_length;
    }
    if (offset >= text.size())
      break;

    if (is_wrap_space(text[offset])) {
      const auto ws = scan_run(text, offset, true);
      if (ws.end >= text.size()) {
        place(offset, ws.end, ws.width);
        break;
      }
      const auto word = scan_run(text, ws.end, false);
      if (word.width <= columns && column + ws.width + word.width > columns) {
        // The word fits on a fresh row but not after this whitespace on
        // the current one: drop the whitespace (standard word-wrap
        // convention) and break right before it.
        breaks.push_back({.content_end = offset, .resume_offset = ws.end});
        column = 0;
        offset = ws.end;
        continue;
      }
      place(offset, ws.end, ws.width);
      offset = ws.end;
      continue;
    }

    const auto word = scan_run(text, offset, false);
    place(offset, word.end, word.width);
    offset = word.end;
  }

  return breaks;
}

} // namespace pi::cli
