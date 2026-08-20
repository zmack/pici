#include "cli/vim_mode.h"

#include "cli/readline.h"
#include "cli/wrap.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace pi::cli {

namespace {

// Matches readline.cpp's private is_word_boundary_byte (is_wrap_space,
// cli/wrap.h, plus '\n' -- which is_wrap_space deliberately excludes, since
// '\n' is a hard line-boundary control character elsewhere, not inter-word
// whitespace). Kept as a small local duplicate rather than exporting the
// private original: this is the only place outside readline.cpp that needs
// it, and the two are simple enough that keeping them in sync by eye is
// cheaper than plumbing another cross-TU declaration for one line of logic.
bool is_word_boundary(char c) { return is_wrap_space(c) || c == '\n'; }

// Vim's 'w' motion: start of the next word. Skips the remainder of the
// current word (if the cursor sits inside one), then skips forward over any
// run of whitespace/newlines to land on the first byte of the next word (or
// buf.size() if there isn't one).
//
// Deliberately NOT the same traversal as readline.cpp's next_word_boundary
// (reused below, adjusted, for 'e'): that function skips boundary bytes
// *first* and then consumes a whole word, which is correct for
// bash/Emacs-style forward-word (M3's Ctrl+Right/Alt+F binding) but would
// overshoot 'w' -- from cursor on whitespace, it would skip the *entire*
// next word instead of stopping at its start. 'w' needs the opposite
// order: consume the current word (if any) first, then stop as soon as a
// non-boundary byte is reached. The returned offset is already the exact
// index of the next word's first character -- both a correct plain-motion
// cursor target and (per Vim's own classification of 'w' as an exclusive
// motion) a correct operator span endpoint with no further adjustment.
std::size_t word_start_forward(std::string_view buf, std::size_t cursor) {
  auto offset = cursor;
  if (offset < buf.size() && !is_word_boundary(buf[offset])) {
    while (offset < buf.size() && !is_word_boundary(buf[offset]))
      offset = next_utf8_offset(buf, offset);
  }
  while (offset < buf.size() && is_word_boundary(buf[offset]))
    offset = next_utf8_offset(buf, offset);
  return offset;
}

// Vim's 'e' motion: end of the current/next word -- landing on the index of
// the word's own *last* character, not one past it. This deliberately
// differs from how 'w'/'0'/'$' encode their targets (one past the relevant
// content, ready to use directly as an operator's exclusive span endpoint):
// this UI's cursor overlay (readline.cpp's InputRenderer::redraw) paints
// its reverse-video block directly at cursor_offset's screen cell, so a
// "one past" position here would visibly land the block on the word's
// trailing whitespace instead of its last letter -- wrong, and a worse
// experience than getting this motion right, per the M5 milestone's own
// guidance. If the cursor doesn't already sit on the last character of its
// current word, this stops there; if it already does (or sits on
// whitespace), it advances to the end of the *next* word instead -- Vim's
// well-known "repeated e skips a whole word forward" behavior. Verified by
// hand against real Vim for: mid-word, already-at-word-end (single- and
// multi-character words), and cursor-on-whitespace starting positions --
// see test_e_end_of_word_distinct_from_w and friends in test_vim_mode.cpp.
//
// Because this lands *on* the character rather than past it, Vim itself
// classifies 'e' as an "inclusive" motion (an operator combined with it
// must still include that landing character) unlike 'w's "exclusive"
// classification (an operator combined with it excludes the landing
// character) -- see resolve_motion below, which is what actually
// implements that distinction for d/c. This is the "common Vim-motion
// gotcha" callers combining word motions with operators most often get
// wrong.
std::size_t word_end_forward(std::string_view buf, std::size_t cursor) {
  if (cursor >= buf.size())
    return cursor; // nothing under/after the cursor to move onto
  auto offset = next_utf8_offset(buf, cursor); // always step forward first
  if (offset >= buf.size())
    return cursor; // cursor was already on the last character; nowhere to go
  while (offset < buf.size() && is_word_boundary(buf[offset]))
    offset = next_utf8_offset(buf, offset);
  if (offset >= buf.size())
    return cursor; // nothing but trailing whitespace ahead: no next word
  while (true) {
    const auto next = next_utf8_offset(buf, offset);
    if (next >= buf.size() || is_word_boundary(buf[next]))
      break;
    offset = next;
  }
  return offset;
}

// A motion's operator-span target, plus whether an operator combining with
// it must include the landing character itself ("inclusive", Vim's own
// term) or stop just before it ("exclusive"). Every motion in this
// milestone's scope is exclusive except 'e' -- see word_end_forward's
// comment above for why 'e' needs this distinct handling, and
// handle_normal_key below for where the inclusive flag actually extends
// the erase span by one character.
struct MotionTarget {
  std::size_t offset{0};
  bool inclusive{false};
};

// Resolves the single-byte characterwise motions this milestone covers
// (used only from the operator-pending branch of handle_normal_key below;
// plain Normal-mode cursor movement calls each motion's function directly).
// j/k are intentionally excluded here -- they're plain Normal-mode motions
// but not operator-combinable. Vim's linewise d/c + j/k semantics
// (operating on two whole lines at once) would need meaningfully more
// special-casing than the doubled dd/cc case already covers, which is
// exactly the "skip it" side of the judgment call the M5 milestone scope
// calls for.
std::optional<MotionTarget> resolve_motion(char motion, std::string_view buf,
                                           std::size_t cursor) {
  switch (motion) {
  case 'h':
    return MotionTarget{.offset = previous_utf8_offset(buf, cursor)};
  case 'l':
    return MotionTarget{.offset = next_utf8_offset(buf, cursor)};
  case '0':
    return MotionTarget{.offset = line_start(buf, cursor)};
  case '$':
    return MotionTarget{.offset = line_end(buf, cursor)};
  case 'w':
    return MotionTarget{.offset = word_start_forward(buf, cursor)};
  case 'b':
    return MotionTarget{.offset = previous_word_boundary(buf, cursor)};
  case 'e':
    return MotionTarget{.offset = word_end_forward(buf, cursor),
                        .inclusive = true};
  default:
    return std::nullopt;
  }
}

} // namespace

void VimEngine::apply_dd(std::string &buf, std::size_t &cursor,
                         std::string &kill_buffer) {
  auto start = line_start(buf, cursor);
  auto end = line_end(buf, cursor);
  bool absorbed_previous_newline = false;
  if (end < buf.size()) {
    // Not the last line: absorb this line's own trailing '\n' too, so the
    // line that follows takes this line's place instead of a blank line
    // being left behind.
    end = next_utf8_offset(buf, end);
  } else if (start > 0) {
    // Last line with no trailing newline of its own: absorb the *previous*
    // line's newline instead, so no blank line is left dangling at the end.
    --start;
    absorbed_previous_newline = true;
  }
  kill_buffer.assign(buf, start, end - start);
  buf.erase(start, end - start);
  cursor = absorbed_previous_newline ? line_start(buf, start)
                                     : std::min(start, buf.size());
}

void VimEngine::apply_cc(std::string &buf, std::size_t &cursor,
                         std::string &kill_buffer) {
  // Unlike dd, cc keeps the line itself -- as an empty line to type into --
  // removing only its content; buffer structure (newlines) is untouched.
  const auto start = line_start(buf, cursor);
  const auto end = line_end(buf, cursor);
  if (end > start) {
    kill_buffer.assign(buf, start, end - start);
    buf.erase(start, end - start);
  }
  cursor = start;
}

VimEngine::Result VimEngine::handle_normal_key(unsigned char c,
                                               std::string &buf,
                                               std::size_t &cursor,
                                               std::string &kill_buffer) {
  Result result;
  const char ch = static_cast<char>(c);

  if (pending_ != PendingOp::none) {
    const auto op = pending_;
    pending_ = PendingOp::none;

    // Doubled operator letter (dd/cc): whole-current-line operation, the
    // standard Vim convention for "this operator applied to the whole
    // line."
    if ((op == PendingOp::delete_op && ch == 'd') ||
        (op == PendingOp::change_op && ch == 'c')) {
      if (op == PendingOp::delete_op)
        apply_dd(buf, cursor, kill_buffer);
      else
        apply_cc(buf, cursor, kill_buffer);
      result.changed = true;
      if (op == PendingOp::change_op)
        mode_ = VimMode::Insert;
      return result;
    }

    const auto motion = resolve_motion(ch, buf, cursor);
    if (!motion)
      // Unrecognized key while an operator was pending: cancel the pending
      // operator silently (matching real Vim's "invalid motion cancels the
      // pending operator" behavior) rather than misapplying it or leaving
      // it stuck waiting forever for a motion that will never come. The key
      // itself is still consumed -- not reinterpreted as a fresh command.
      return result;

    // Inclusive motions (only 'e' in this milestone's scope -- see
    // word_end_forward's comment) land the cursor *on* their target
    // character for plain movement, but an operator combined with them
    // must still delete/change that character too -- extend the span by
    // one codepoint. Guarded on the motion having actually moved forward:
    // word_end_forward returns the cursor unchanged when there's no next
    // word to move to, and that genuine no-op must stay a no-op rather than
    // manufacturing a one-character span out of nothing.
    auto target = motion->offset;
    if (motion->inclusive && target > cursor)
      target = next_utf8_offset(buf, target);

    const auto start = std::min(cursor, target);
    const auto end = std::max(cursor, target);
    if (end > start) {
      kill_buffer.assign(buf, start, end - start);
      buf.erase(start, end - start);
      cursor = start;
      result.changed = true;
    }
    if (op == PendingOp::change_op) {
      mode_ = VimMode::Insert;
      result.changed = true;
    }
    return result;
  }

  switch (ch) {
  case 'h':
    cursor = previous_utf8_offset(buf, cursor);
    result.changed = true;
    break;
  case 'l':
    cursor = next_utf8_offset(buf, cursor);
    result.changed = true;
    break;
  case 'j':
    cursor = next_line_offset(buf, cursor);
    result.changed = true;
    break;
  case 'k':
    cursor = previous_line_offset(buf, cursor);
    result.changed = true;
    break;
  case '0':
    cursor = line_start(buf, cursor);
    result.changed = true;
    break;
  case '$':
    cursor = line_end(buf, cursor);
    result.changed = true;
    break;
  case 'w':
    cursor = word_start_forward(buf, cursor);
    result.changed = true;
    break;
  case 'b':
    cursor = previous_word_boundary(buf, cursor);
    result.changed = true;
    break;
  case 'e':
    cursor = word_end_forward(buf, cursor);
    result.changed = true;
    break;
  case 'd':
    pending_ = PendingOp::delete_op;
    break;
  case 'c':
    pending_ = PendingOp::change_op;
    break;
  case 'i': // Insert before cursor: cursor is already there.
    mode_ = VimMode::Insert;
    result.changed = true;
    break;
  case 'a': // Insert after cursor.
    cursor = next_utf8_offset(buf, cursor);
    mode_ = VimMode::Insert;
    result.changed = true;
    break;
  case 'A': // Insert at line end.
    cursor = line_end(buf, cursor);
    mode_ = VimMode::Insert;
    result.changed = true;
    break;
  case 'I': // Insert at line start.
    cursor = line_start(buf, cursor);
    mode_ = VimMode::Insert;
    result.changed = true;
    break;
  default:
    break; // Uncovered key: a harmless no-op, never eaten silently.
  }
  return result;
}

} // namespace pi::cli
