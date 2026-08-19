#include "cli/readline.h"

#include "cli/wrap.h"
#include "core/terminal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <system_error>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pi::cli {

namespace {

void close_fd(int &fd) noexcept {
  if (fd >= 0) {
    ::close(fd);
    fd = -1;
  }
}

void drain_nonblocking_fd(int fd) noexcept {
  if (fd < 0)
    return;
  std::array<unsigned char, 128> buffer{};
  while (true) {
    const auto result = ::read(fd, buffer.data(), buffer.size());
    if (result > 0)
      continue;
    if (result < 0 && errno == EINTR)
      continue;
    return;
  }
}

void configure_wake_fd(int fd) {
  auto descriptor_flags = ::fcntl(fd, F_GETFD);
  if (descriptor_flags < 0 ||
      ::fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0)
    throw std::system_error(errno, std::generic_category(),
                            "fcntl(FD_CLOEXEC)");

  auto status_flags = ::fcntl(fd, F_GETFL);
  if (status_flags < 0 || ::fcntl(fd, F_SETFL, status_flags | O_NONBLOCK) < 0)
    throw std::system_error(errno, std::generic_category(),
                            "fcntl(O_NONBLOCK)");
}

int poll_retry(struct pollfd *descriptors, nfds_t count, int timeout_ms) {
  while (true) {
    const auto result = ::poll(descriptors, count, timeout_ms);
    if (result < 0 && errno == EINTR)
      continue;
    return result;
  }
}

} // namespace

ReadlineWake::ReadlineWake() {
  std::array<int, 2> descriptors = {-1, -1};
  if (::pipe(descriptors.data()) != 0)
    throw std::system_error(errno, std::generic_category(), "pipe");

  try {
    configure_wake_fd(descriptors[0]);
    configure_wake_fd(descriptors[1]);
  } catch (...) {
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    throw;
  }

  read_fd_ = descriptors[0];
  write_fd_ = descriptors[1];
}

ReadlineWake::~ReadlineWake() {
  close_fd(read_fd_);
  close_fd(write_fd_);
}

ReadlineWake::ReadlineWake(ReadlineWake &&other) noexcept
    : read_fd_(std::exchange(other.read_fd_, -1)),
      write_fd_(std::exchange(other.write_fd_, -1)) {}

ReadlineWake &ReadlineWake::operator=(ReadlineWake &&other) noexcept {
  if (this != &other) {
    close_fd(read_fd_);
    close_fd(write_fd_);
    read_fd_ = std::exchange(other.read_fd_, -1);
    write_fd_ = std::exchange(other.write_fd_, -1);
  }
  return *this;
}

bool ReadlineWake::notify() const noexcept {
  if (write_fd_ < 0)
    return false;

  constexpr unsigned char signal = 1;
  while (true) {
    const auto result = ::write(write_fd_, &signal, sizeof(signal));
    if (result == 1)
      return true;
    if (result < 0 && errno == EINTR)
      continue;
    return result < 0 && errno == EAGAIN;
  }
}

void ReadlineWake::drain() const noexcept { drain_nonblocking_fd(read_fd_); }

namespace {

// RAII guard: put terminal into raw mode while alive.
// ISIG is kept enabled so Ctrl+C still delivers SIGINT.
struct RawMode {
  int fd{-1};
  struct termios saved {};
  bool active{false};

  RawMode() = default;
  RawMode(const RawMode &) = delete;
  RawMode &operator=(const RawMode &) = delete;
  RawMode(RawMode &&) = delete;
  RawMode &operator=(RawMode &&) = delete;
  ~RawMode() { leave(); }

  bool enter(int fdesc) {
    if (isatty(fdesc) == 0)
      return false;
    if (tcgetattr(fdesc, &saved) != 0)
      return false;
    struct termios raw = saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON);
    // Clear ICRNL so \r (Enter) and \n (Ctrl+J) arrive as distinct bytes.
    // With it set, the line discipline translates \r to \n before pici ever
    // sees it, making Enter indistinguishable from Ctrl+J — and making
    // Alt+Enter's second byte (a literal \r) collapse into the same byte as
    // Alt+Ctrl+J, so a newline-insert binding can't tell the two apart.
    raw.c_iflag &= ~static_cast<tcflag_t>(ICRNL);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fdesc, TCSAFLUSH, &raw) != 0)
      return false;
    fd = fdesc;
    active = true;
    // Bracketed paste: the terminal wraps pasted content in ESC[200~ /
    // ESC[201~ markers so it can be read as one block instead of being
    // indistinguishable from typed keystrokes.
    std::cout << "\033[?2004h" << std::flush;
    return true;
  }

  void leave() {
    if (active) {
      std::cout << "\033[?2004l" << std::flush;
      tcsetattr(fd, TCSAFLUSH, &saved);
      active = false;
    }
  }
};

std::size_t terminal_columns() {
  // NOLINTNEXTLINE(misc-include-cleaner): ioctl declarations vary by platform.
  struct winsize size {};
  // NOLINTNEXTLINE(misc-include-cleaner): ioctl declarations vary by platform.
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
    return size.ws_col;
  return 80;
}

// Applied to the whole prompt/input row so it reads as a distinct box
// against the surrounding terminal background. An actual background color
// is required rather than reverse video (SGR 7): terminals fill cells
// touched by an erase-in-line ("\033[K") using the active background color,
// but not the reverse-video bit, so reverse video alone would only tint the
// glyphs actually written, not the rest of the row. SGR 100 (bright-black
// background) renders as a distinguishable mid-gray under most terminal
// color themes, dark and light alike.
constexpr std::string_view kInputAreaBackground = "\033[100m";

class InputRenderer {
public:
  InputRenderer(std::string_view prompt, std::string_view status_line)
      : prompt_(prompt), status_line_(status_line) {
    while (leading_newlines_ < prompt_.size() &&
           prompt_[leading_newlines_] == '\n') {
      ++leading_newlines_;
    }
    prompt_.erase(0, leading_newlines_);
  }

  void redraw(const std::string &buf, std::size_t cursor,
              bool show_cursor = true) {
    clear_previous();

    if (first_draw_) {
      for (std::size_t i = 0; i < leading_newlines_; ++i)
        std::cout << '\n';
      std::cout << '\r';
      first_draw_ = false;
    }

    const auto columns = terminal_columns();
    const std::size_t status_rows = status_line_.empty() ? 0 : 1;

    // Composer height cap: measure the prompt+buffer layout with all output
    // suppressed first, so the visible viewport (see below) can be decided
    // before anything is actually painted. Row numbering here starts at 1
    // for the prompt's own first row, independent of whether a status line
    // will additionally be drawn above it in the real pass.
    std::size_t measured_rows = 1;
    std::size_t measured_column = 0;
    CursorPosition measured_cursor;
    write_wrapped(prompt_, columns, measured_rows, measured_column,
                  std::string_view::npos, nullptr, 1, 0);
    write_wrapped(buf, columns, measured_rows, measured_column, cursor,
                  &measured_cursor, 1, 0);
    std::size_t composer_total_rows = measured_rows;
    if (show_cursor && measured_column == columns)
      ++composer_total_rows;

    // At most kMaxComposerRows of that layout are ever painted, keeping
    // rendered_rows_ (used by clear_previous()'s relative-motion erase
    // logic) bounded regardless of draft length — a draft taller than the
    // terminal would otherwise desync that bookkeeping against the
    // terminal's own scrolling. The window always keeps the cursor's row
    // visible (or, with the cursor hidden, stays anchored to the tail);
    // earlier rows are simply not drawn — the buffer itself is untouched.
    std::size_t composer_viewport_start = 1;
    std::size_t composer_viewport_end = composer_total_rows;
    if (composer_total_rows > core::kMaxComposerRows) {
      const std::size_t max_start =
          composer_total_rows - core::kMaxComposerRows + 1;
      const std::size_t anchor_row =
          show_cursor ? measured_cursor.row : composer_total_rows;
      std::size_t preferred_start = 1;
      if (anchor_row + 1 > core::kMaxComposerRows)
        preferred_start = anchor_row + 1 - core::kMaxComposerRows;
      composer_viewport_start = std::min(preferred_start, max_start);
      if (composer_viewport_start < 1)
        composer_viewport_start = 1;
      composer_viewport_end =
          composer_viewport_start + core::kMaxComposerRows - 1;
    }
    // Translate into the absolute row numbering the real pass below uses,
    // which starts one row later whenever a status line is drawn first.
    const std::size_t viewport_start = composer_viewport_start + status_rows;
    const std::size_t viewport_end = composer_viewport_end + status_rows;

    std::size_t rows = 1;
    std::size_t column = 0;
    if (!status_line_.empty()) {
      std::cout << '\r'
                << core::truncate_ansi_line(status_line_,
                                            static_cast<int>(columns))
                << "\033[K\r\n";
      ++rows;
    }
    std::cout << "\033[?7l";
    std::cout << kInputAreaBackground;
    write_wrapped(prompt_, columns, rows, column, std::string_view::npos,
                  nullptr, viewport_start, viewport_end);
    CursorPosition cursor_position;
    write_wrapped(buf, columns, rows, column, cursor, &cursor_position,
                  viewport_start, viewport_end);

    if (show_cursor && column == columns) {
      if (rows < viewport_end)
        std::cout << "\033[K\r\n";
      ++rows;
      column = 0;
    }

    std::cout << "\033[K"; // fill remainder of the final row with the tint
    // rows_relative / cursor_row_relative below re-express the (possibly
    // very large) absolute row numbers in terms of what's actually on
    // screen: the status line (if any) plus at most kMaxComposerRows
    // composer rows, so the vertical-motion math and rendered_rows_ stay in
    // the same bounded space clear_previous() expects.
    const std::size_t printed_final_row = std::min(rows, viewport_end);
    const std::size_t composer_rows_shown =
        printed_final_row - viewport_start + 1;
    const std::size_t rows_relative = status_rows + composer_rows_shown;
    if (show_cursor) {
      const std::size_t cursor_row_relative =
          std::clamp(cursor_position.row, viewport_start, viewport_end) -
          composer_viewport_start + 1;
      if (rows_relative > cursor_row_relative)
        std::cout << "\033[" << rows_relative - cursor_row_relative << 'A';
      else if (cursor_row_relative > rows_relative)
        std::cout << "\033[" << cursor_row_relative - rows_relative << 'B';
      std::cout << '\r';
      // Guard against ever emitting CUF with n >= columns: CSI n C clamps
      // at the terminal's rightmost cell, so a stray out-of-range position
      // (which should no longer occur — see write_wrapped's capture sites)
      // would silently land the cursor glyph on top of the last real
      // character instead of failing loudly.
      const bool cursor_in_row =
          cursor_position.column > 0 && cursor_position.column < columns;
      if (cursor_in_row)
        std::cout << "\033[" << cursor_position.column << 'C';
      // Toggle reverse video (SGR 7/27) for just the cursor cell without
      // dropping the input area's background tint.
      std::cout << "\033[7m \033[27m";
      if (cursor_in_row)
        std::cout << "\033[D";
      cursor_row_ = cursor_row_relative;
    } else {
      cursor_row_ = rows_relative;
    }
    std::cout << "\033[0m"; // leave the tinted box before yielding control
    std::cout << "\033[?7h" << std::flush;
    rendered_rows_ = rows_relative;
    rendered_column_ = column;
  }

  // External output (completion candidates) invalidates the cursor anchor.
  void invalidate() { rendered_rows_ = 0; }

  // A full-screen renderer repainted its own layout and repositioned the
  // real terminal cursor out from under us (e.g. after a resize). Forget the
  // relative-motion bookkeeping entirely and re-run the first-draw anchoring
  // (leading newlines) on the next redraw(), exactly as if this were a fresh
  // prompt.
  void reanchor() {
    first_draw_ = true;
    rendered_rows_ = 0;
    rendered_column_ = 0;
    cursor_row_ = 1;
  }

private:
  struct CursorPosition {
    std::size_t row{1};
    std::size_t column{0};
  };

  void clear_previous() const {
    if (rendered_rows_ == 0)
      return;

    if (rendered_rows_ > cursor_row_)
      std::cout << "\033[" << rendered_rows_ - cursor_row_ << 'B';
    std::cout << '\r';
    if (rendered_column_ > 0)
      std::cout << "\033[" << rendered_column_ << 'C';
    if (rendered_rows_ > 1)
      std::cout << "\033[" << rendered_rows_ - 1 << 'A';
    std::cout << '\r';
    for (std::size_t row = 0; row < rendered_rows_; ++row) {
      std::cout << "\033[2K";
      if (row + 1 < rendered_rows_)
        std::cout << "\033[B\r";
    }
    if (rendered_rows_ > 1)
      std::cout << "\033[" << rendered_rows_ - 1 << 'A';
    std::cout << '\r';
  }

  static void write_wrapped(std::string_view text, std::size_t columns,
                            std::size_t &rows, std::size_t &column,
                            std::size_t cursor_offset,
                            CursorPosition *cursor_position,
                            std::size_t viewport_start,
                            std::size_t viewport_end) {
    const auto capture = [&] {
      if (cursor_position != nullptr) {
        cursor_position->row = rows;
        cursor_position->column = column;
      }
    };
    // Every terminal write below is gated against the composer's
    // height-cap viewport (see redraw()): rows outside [viewport_start,
    // viewport_end] still update rows/column and still get their cursor
    // captured (so wrap decisions and the recorded cursor position stay
    // correct regardless of what's actually painted), but nothing reaches
    // the terminal for them. Passing an empty range (viewport_end <
    // viewport_start) suppresses all output — used for redraw()'s
    // measurement-only pass, which decides where the viewport should sit
    // before anything is drawn for real.
    const auto row_in_viewport = [&] {
      return rows >= viewport_start && rows <= viewport_end;
    };
    // A row-ending transition (hard \n or a width-triggered wrap) should
    // only be painted if it moves onto another row still inside the
    // viewport — printing it while leaving the last visible row would push
    // the real terminal cursor one row past the cap.
    const auto transition_in_viewport = [&] {
      return rows >= viewport_start && rows < viewport_end;
    };
    // Word-wrap plan for the printable run currently being painted (see
    // plan_word_wrap in cli/wrap.h). Recomputed lazily whenever `offset`
    // reaches the end of the run it covers — a run spans from one hard
    // control character ('\n'/'\r') to the next (or text.size()), matching
    // the segments plan_word_wrap itself expects (no embedded '\n'). The
    // plan is a pure function of (text, columns, start_column), so this
    // measurement-only pass and the real paint pass below — and the cursor
    // capture in both — all derive the exact same break points from it,
    // which is what actually keeps the paint loop and the cursor
    // calculation from disagreeing about where a wrap happens (the M0/M2
    // invariant).
    std::vector<WordWrapBreak> plan;
    std::size_t plan_run_start = 0;
    std::size_t plan_run_end = 0;
    std::size_t plan_index = 0;
    bool have_plan = false;

    for (std::size_t offset = 0; offset < text.size();) {
      if (const auto escape_length = ansi_escape_length(text, offset);
          escape_length > 0) {
        if (offset == cursor_offset)
          capture();
        const auto escape = text.substr(offset, escape_length);
        if (row_in_viewport())
          std::cout.write(escape.data(),
                          static_cast<std::streamsize>(escape.size()));
        offset += escape_length;
        continue;
      }

      if (text[offset] == '\n') {
        if (offset == cursor_offset) {
          capture();
          // A hard newline always starts a new row, regardless of how full
          // the preceding row was — if the pre-break column exactly filled
          // the row, normalize to the start of the row the newline is
          // about to create. Without this, a cursor sitting right at an
          // embedded newline whose preceding row was exactly full would
          // capture a position one column past that row's last cell,
          // matching the same class of bug M0 fixed for plain wrapped text
          // (see the word-wrap branch and the end-of-text tail below).
          if (cursor_position != nullptr &&
              cursor_position->column == columns) {
            ++cursor_position->row;
            cursor_position->column = 0;
          }
        }
        // Fill the remainder of the row before wrapping so the input area's
        // background tint covers the whole box, not just the glyphs.
        if (transition_in_viewport())
          std::cout << "\033[K\r\n";
        ++rows;
        column = 0;
        ++offset;
        continue;
      }
      if (text[offset] == '\r') {
        if (offset == cursor_offset)
          capture();
        if (row_in_viewport())
          std::cout << '\r';
        column = 0;
        ++offset;
        continue;
      }

      // Printable content: the word-wrap breaks for the run starting here
      // are planned once, as a pure function of the text ahead, the
      // terminal width, and the column this run starts at, then just
      // replayed character by character below — so the plan and the paint
      // loop can never disagree about where a break falls.
      if (!have_plan || offset >= plan_run_end) {
        plan_run_start = offset;
        plan_run_end = offset;
        while (plan_run_end < text.size() && text[plan_run_end] != '\n' &&
               text[plan_run_end] != '\r')
          ++plan_run_end;
        plan = plan_word_wrap(
            text.substr(plan_run_start, plan_run_end - plan_run_start), columns,
            column);
        plan_index = 0;
        have_plan = true;
      }

      if (plan_index < plan.size() &&
          offset == plan_run_start + plan[plan_index].content_end) {
        const auto resume_offset =
            plan_run_start + plan[plan_index].resume_offset;
        // Same invariant as the '\n' branch above: the wrap decision for
        // the content at `offset` is made — and the row bumped — before
        // any cursor capture for it, so a capture here always lands on the
        // new row's start rather than one column past the row this content
        // is being wrapped off of.
        if (transition_in_viewport())
          std::cout << "\033[K\r\n";
        ++rows;
        column = 0;
        // Everything in [offset, resume_offset) is dropped trailing
        // whitespace that's never painted (standard word-wrap convention)
        // — if the cursor sits anywhere in that range, it normalizes to
        // the row it just moved onto, same as the M0/M1 full-row cases.
        if (cursor_offset >= offset && cursor_offset < resume_offset)
          capture();
        ++plan_index;
        offset = resume_offset;
        continue;
      }

      const auto length = utf8_length(text, offset);
      const auto width = codepoint_width(
          utf8_codepoint(text, offset, std::min(length, text.size() - offset)));
      if (offset == cursor_offset)
        capture();

      const auto character = text.substr(offset, length);
      if (row_in_viewport())
        std::cout.write(character.data(),
                        static_cast<std::streamsize>(character.size()));
      column += static_cast<std::size_t>(std::max(width, 0));
      offset += length;
    }
    if (cursor_position != nullptr && cursor_offset == text.size()) {
      cursor_position->row = rows;
      cursor_position->column = column;
      // The cursor sits after the last character, with no further
      // character to decide a wrap for. If that character exactly filled
      // the row, this position is one column past the last valid cell —
      // normalize to the start of the next row, matching what redraw()'s
      // synthetic blank-row insertion (triggered by that same
      // column == columns case) actually paints.
      if (cursor_position->column == columns) {
        ++cursor_position->row;
        cursor_position->column = 0;
      }
    }
  }

  std::string prompt_;
  std::string status_line_;
  std::size_t leading_newlines_{0};
  std::size_t rendered_rows_{0};
  std::size_t rendered_column_{0};
  std::size_t cursor_row_{1};
  bool first_draw_{true};
};

std::size_t previous_utf8_offset(std::string_view buf, std::size_t cursor) {
  if (cursor == 0)
    return 0;
  --cursor;
  while (cursor > 0 &&
         (static_cast<unsigned char>(buf[cursor]) & 0xC0U) == 0x80U)
    --cursor;
  return cursor;
}

std::size_t next_utf8_offset(std::string_view buf, std::size_t cursor) {
  if (cursor >= buf.size())
    return buf.size();
  return std::min(buf.size(), cursor + utf8_length(buf, cursor));
}

// --- Shared editing primitives -------------------------------------------
//
// Line-boundary, word-boundary, and logical-row primitives used by the M3
// key bindings below (Home/End/Ctrl+A/Ctrl+E, word-left/right, Ctrl+W/U/K/Y,
// Up/Down). Kept as free functions on (buf, cursor) rather than inlined into
// the key-dispatch chain so M5's vim mode can reuse them directly instead of
// re-deriving the same logic.

// Start offset of the '\n'-delimited logical line containing `cursor` (the
// buffer index right after the preceding '\n', or 0 if there is none).
std::size_t line_start(std::string_view buf, std::size_t cursor) {
  if (cursor == 0)
    return 0;
  const auto newline = buf.rfind('\n', cursor - 1);
  return newline == std::string_view::npos ? 0 : newline + 1;
}

// End offset of the '\n'-delimited logical line containing `cursor` (the
// index of the next '\n', or buf.size() if this is the last line).
std::size_t line_end(std::string_view buf, std::size_t cursor) {
  const auto newline = buf.find('\n', cursor);
  return newline == std::string_view::npos ? buf.size() : newline;
}

// Boundary classification for word motion: is_wrap_space (cli/wrap.h)
// supplies the same whitespace definition write_wrapped uses for word-wrap,
// plus '\n' -- which is_wrap_space deliberately excludes, since every other
// caller of it treats '\n' as its own hard control character rather than
// whitespace -- so word motion stops at a line break instead of splicing
// the last word of one logical line onto the first word of the next.
bool is_word_boundary_byte(char c) { return is_wrap_space(c) || c == '\n'; }

// Start of the word behind the cursor: skip any boundary bytes immediately
// before the cursor, then skip back over the word itself. Standard
// "backward-word" (bash's M-b / most editors' Ctrl+Left); also the
// deletion span for Ctrl+W (unix-word-rubout), which erases exactly this
// range.
std::size_t previous_word_boundary(std::string_view buf, std::size_t cursor) {
  auto offset = cursor;
  while (offset > 0 &&
         is_word_boundary_byte(buf[previous_utf8_offset(buf, offset)]))
    offset = previous_utf8_offset(buf, offset);
  while (offset > 0 &&
         !is_word_boundary_byte(buf[previous_utf8_offset(buf, offset)]))
    offset = previous_utf8_offset(buf, offset);
  return offset;
}

// End of the word ahead of the cursor: skip any boundary bytes at the
// cursor, then skip forward over the word itself. Standard "forward-word"
// (bash's M-f / most editors' Ctrl+Right).
std::size_t next_word_boundary(std::string_view buf, std::size_t cursor) {
  auto offset = cursor;
  while (offset < buf.size() && is_word_boundary_byte(buf[offset]))
    offset = next_utf8_offset(buf, offset);
  while (offset < buf.size() && !is_word_boundary_byte(buf[offset]))
    offset = next_utf8_offset(buf, offset);
  return offset;
}

// Equivalent offset one logical line up, preserving column (measured in
// codepoints from the line start) where possible and clamping to the
// target line's length when it is shorter. Deliberately not wrap-plan
// aware -- this is logical-\n-delimited-line movement only, matching the
// M3 scope decision in composer-textarea-rewrite.md (visual/wrapped-row
// movement would couple buffer navigation to plan_word_wrap unnecessarily).
// A cursor already on the first line is left unchanged.
std::size_t previous_line_offset(std::string_view buf, std::size_t cursor) {
  const auto current_start = line_start(buf, cursor);
  if (current_start == 0)
    return cursor;
  std::size_t column = 0;
  for (std::size_t offset = current_start; offset < cursor;
       offset = next_utf8_offset(buf, offset))
    ++column;
  const auto previous_end = current_start - 1; // the '\n' ending that line
  const auto previous_start = line_start(buf, previous_end);
  auto offset = previous_start;
  for (std::size_t taken = 0; taken < column && offset < previous_end; ++taken)
    offset = next_utf8_offset(buf, offset);
  return offset;
}

// Equivalent offset one logical line down; see previous_line_offset above.
// A cursor already on the last line is left unchanged.
std::size_t next_line_offset(std::string_view buf, std::size_t cursor) {
  const auto current_start = line_start(buf, cursor);
  const auto current_end = line_end(buf, cursor);
  if (current_end >= buf.size())
    return cursor;
  std::size_t column = 0;
  for (std::size_t offset = current_start; offset < cursor;
       offset = next_utf8_offset(buf, offset))
    ++column;
  const auto next_start = current_end + 1; // skip the '\n'
  const auto next_end = line_end(buf, next_start);
  auto offset = next_start;
  for (std::size_t taken = 0; taken < column && offset < next_end; ++taken)
    offset = next_utf8_offset(buf, offset);
  return offset;
}

// Apply completions to buf, redrawing the line as needed.
void apply_completions(InputRenderer &renderer, std::string &buf,
                       std::vector<std::string> completions) {
  if (completions.empty()) {
    std::cout << '\a' << std::flush; // bell
    return;
  }

  // Sort for stable display and prefix calculation
  std::ranges::sort(completions);

  if (completions.size() == 1) {
    buf = completions[0];
    buf += ' ';
    renderer.redraw(buf, buf.size());
    return;
  }

  // Longest common prefix across all candidates
  std::string prefix = completions[0];
  for (std::size_t i = 1; i < completions.size(); ++i) {
    std::size_t j = 0;
    while (j < prefix.size() && j < completions[i].size() &&
           prefix[j] == completions[i][j])
      ++j;
    prefix.resize(j);
  }

  if (prefix.size() > buf.size()) {
    buf = prefix;
    renderer.redraw(buf, buf.size());
    return;
  }

  // Already at common prefix: print candidates below, then redraw prompt.
  // Clear the synthetic input cursor before moving down so it does not remain
  // painted at the old insertion point.
  renderer.redraw(buf, buf.size(), false);
  std::cout << "\r\n";
  for (const auto &c : completions)
    std::cout << "  " << c << '\n';
  renderer.invalidate();
  renderer.redraw(buf, buf.size());
}

struct EscapeSequenceResult {
  std::string sequence;
  bool wake{false};
};

EscapeSequenceResult read_escape_sequence(int wake_fd) {
  std::string seq;
  unsigned char c = 0;
  std::array<pollfd, 2> descriptors{};
  descriptors[0] = {.fd = STDIN_FILENO, .events = POLLIN};
  const nfds_t descriptor_count = wake_fd >= 0 ? 2 : 1;
  if (wake_fd >= 0)
    descriptors[1] = {.fd = wake_fd, .events = POLLIN};

  auto poll_result = poll_retry(descriptors.data(), descriptor_count, 25);
  if (poll_result <= 0)
    return {.sequence = std::move(seq)};
  const bool wake_ready = wake_fd >= 0 && (descriptors[1].revents &
                                           (POLLIN | POLLHUP | POLLERR)) != 0;
  if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
    if (wake_ready)
      drain_nonblocking_fd(wake_fd);
    return wake_ready ? EscapeSequenceResult{.wake = true}
                      : EscapeSequenceResult{.sequence = std::move(seq)};
  }
  if (::read(STDIN_FILENO, &c, 1) <= 0)
    return {.sequence = std::move(seq)};
  seq += static_cast<char>(c);

  if (c != '[' && c != 'O')
    return {.sequence = std::move(seq)};

  // SGR mouse reports ("[<Cb;Cx;Cy M") carry two decimal coordinates and can
  // comfortably exceed the 8-byte budget that's ample for plain cursor/page
  // keys, so give escape sequences more room to be read in full.
  while (seq.size() < 32) {
    descriptors[0].revents = 0;
    if (descriptor_count > 1)
      descriptors[1].revents = 0;
    poll_result = poll_retry(descriptors.data(), descriptor_count, 25);
    if (poll_result <= 0)
      break;
    const bool wake_ready = wake_fd >= 0 && (descriptors[1].revents &
                                             (POLLIN | POLLHUP | POLLERR)) != 0;
    if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
      if (wake_ready)
        drain_nonblocking_fd(wake_fd);
      return wake_ready ? EscapeSequenceResult{.wake = true}
                        : EscapeSequenceResult{.sequence = std::move(seq)};
    }
    if (::read(STDIN_FILENO, &c, 1) <= 0)
      break;
    seq += static_cast<char>(c);
    if ((c >= '@' && c <= '~'))
      break;
  }
  return {.sequence = std::move(seq)};
}

struct BracketedPasteResult {
  std::string content;
  bool wake{false};
};

// Reads raw bytes following a "[200~" paste-start marker (already consumed
// by read_escape_sequence) until the "[201~" paste-end marker is seen.
// Every byte in between — including literal \n/\r from the pasted text — is
// data, never reinterpreted as Enter, Alt+Enter, or another escape
// sequence: bracketed paste exists precisely so pasted content can be told
// apart from typed keystrokes and inserted as one inert block.
BracketedPasteResult read_bracketed_paste(int wake_fd) {
  BracketedPasteResult result;
  static constexpr std::string_view kEndMarker = "\033[201~";
  std::array<pollfd, 2> descriptors{};
  descriptors[0] = {.fd = STDIN_FILENO, .events = POLLIN};
  const nfds_t descriptor_count = wake_fd >= 0 ? 2 : 1;
  if (wake_fd >= 0)
    descriptors[1] = {.fd = wake_fd, .events = POLLIN};

  while (true) {
    if (result.content.size() >= kEndMarker.size() &&
        std::string_view(result.content).ends_with(kEndMarker)) {
      result.content.resize(result.content.size() - kEndMarker.size());
      return result;
    }
    descriptors[0].revents = 0;
    if (descriptor_count > 1)
      descriptors[1].revents = 0;
    // Unlike read_escape_sequence's short probe window, a paste has no
    // fixed size and the terminal may deliver it in several chunks — block
    // until more input (or a wake) actually arrives instead of timing out.
    if (poll_retry(descriptors.data(), descriptor_count, -1) <= 0)
      continue;
    const bool wake_ready = wake_fd >= 0 && (descriptors[1].revents &
                                             (POLLIN | POLLHUP | POLLERR)) != 0;
    if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
      if (wake_ready) {
        result.wake = true;
        return result;
      }
      continue;
    }
    unsigned char c = 0;
    const auto n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      return result; // EOF/error mid-paste: stop with whatever was read.
    }
    result.content += static_cast<char>(c);
  }
}

bool handle_escape_sequence(std::string_view seq, const ControlFn &control_fn,
                            std::string_view buf, std::size_t &cursor) {
  if (seq == "[D") {
    cursor = previous_utf8_offset(buf, cursor);
    return true;
  }
  if (seq == "[C") {
    cursor = next_utf8_offset(buf, cursor);
    return true;
  }
  // Ctrl+Left / Ctrl+Right: word-left / word-right. xterm-compatible
  // terminals always report a modifier on an arrow key in the CSI
  // "1;<mod>" form (never the unmodified SS3 "O" form), so there's no
  // "O"-prefixed equivalent to add alongside this.
  if (seq == "[1;5D") {
    cursor = previous_word_boundary(buf, cursor);
    return true;
  }
  if (seq == "[1;5C") {
    cursor = next_word_boundary(buf, cursor);
    return true;
  }
  // Plain Up/Down: move the cursor one logical line within the buffer
  // (previous_line_offset/next_line_offset), preserving column where
  // possible. This supersedes the old scroll_line_up/down binding here --
  // see the Ctrl+Up/Ctrl+Down bindings below, which took it over.
  if (seq == "[A") {
    cursor = previous_line_offset(buf, cursor);
    return true;
  }
  if (seq == "[B") {
    cursor = next_line_offset(buf, cursor);
    return true;
  }
  // Plain Home/End: current logical line's start/end, not the whole
  // buffer. Ctrl+Home/Ctrl+End below took over the old scroll_top/
  // scroll_bottom binding this used to have.
  if (seq == "[H" || seq == "OH" || seq == "[1~") {
    cursor = line_start(buf, cursor);
    return true;
  }
  if (seq == "[F" || seq == "OF" || seq == "[4~") {
    cursor = line_end(buf, cursor);
    return true;
  }

  if (!control_fn)
    return false;

  // SGR mouse report: "[<Cb;Cx;Cy" then 'M' (press) or 'm' (release).
  // Column/row (Cx/Cy) don't matter for scrolling, only the button code
  // (Cb): bit 0x40 marks the "wheel" button group, and bit 0x01 within that
  // group distinguishes down (odd) from up (even) — this holds regardless
  // of any modifier-key bits also set in Cb. Only trigger on the press
  // ('M'); wheel devices don't need the matching release to be handled.
  if (seq.starts_with("[<")) {
    std::size_t pos = 2;
    const auto read_number = [&](std::size_t &p) -> long {
      long value = 0;
      bool any = false;
      while (p < seq.size() && seq[p] >= '0' && seq[p] <= '9') {
        value = (value * 10) + (seq[p] - '0');
        ++p;
        any = true;
      }
      return any ? value : -1;
    };
    const long cb = read_number(pos);
    if (cb < 0 || pos >= seq.size() || seq[pos] != ';')
      return false;
    ++pos;
    if (read_number(pos) < 0 || pos >= seq.size() || seq[pos] != ';')
      return false;
    ++pos;
    if (read_number(pos) < 0 || pos >= seq.size())
      return false;
    if (seq[pos] != 'M' || (cb & 0x40) == 0)
      return false;
    constexpr int kLinesPerNotch = 3;
    const auto action = (cb & 0x01) != 0 ? ControlAction::scroll_line_down
                                         : ControlAction::scroll_line_up;
    for (int i = 0; i < kLinesPerNotch; ++i)
      control_fn(action);
    return true;
  }

  // Ctrl+Up / Ctrl+Down: transcript line-scroll -- what plain Up/Down did
  // before Up/Down became buffer cursor movement above.
  if (seq == "[1;5A") {
    control_fn(ControlAction::scroll_line_up);
    return true;
  }
  if (seq == "[1;5B") {
    control_fn(ControlAction::scroll_line_down);
    return true;
  }
  if (seq == "[5~") {
    control_fn(ControlAction::scroll_page_up);
    return true;
  }
  if (seq == "[6~") {
    control_fn(ControlAction::scroll_page_down);
    return true;
  }
  // Ctrl+Home / Ctrl+End: transcript top/bottom -- what plain Home/End did
  // before. Cover the same variant families as the plain bindings above
  // ("H"/"F"-style and "~"-style), since terminals aren't consistent about
  // which unmodified form they use either.
  if (seq == "[1;5H" || seq == "[1;5~") {
    control_fn(ControlAction::scroll_top);
    return true;
  }
  if (seq == "[1;5F" || seq == "[4;5~") {
    control_fn(ControlAction::scroll_bottom);
    return true;
  }

  return false;
}

} // namespace

ReadlineResult readline(std::string_view prompt, const CompleteFn &complete_fn,
                        const ControlFn &control_fn,
                        std::string_view status_line,
                        std::string_view initial_draft,
                        std::size_t initial_cursor, int wake_fd,
                        bool clear_on_submit,
                        const std::function<void()> &on_resize) {
  // Non-TTY fallback: just use getline (pipes, scripts, tests)
  if (isatty(STDIN_FILENO) == 0) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line))
      return {.reason = ReadlineExit::eof};
    const auto cursor = line.size();
    return {.reason = ReadlineExit::submitted,
            .text = std::move(line),
            .cursor = cursor};
  }

  RawMode raw;
  if (!raw.enter(STDIN_FILENO)) {
    // Couldn't enter raw mode — fall back
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line))
      return {.reason = ReadlineExit::eof};
    const auto cursor = line.size();
    return {.reason = ReadlineExit::submitted,
            .text = std::move(line),
            .cursor = cursor};
  }

  std::string buf(initial_draft);
  std::size_t cursor = initial_cursor == std::string_view::npos
                           ? buf.size()
                           : std::min(initial_cursor, buf.size());
  while (cursor > 0 &&
         (static_cast<unsigned char>(buf[cursor]) & 0xC0U) == 0x80U)
    --cursor;

  // Single most-recent-kill slot for Ctrl+W/Ctrl+U/Ctrl+K/Ctrl+Y. Scoped to
  // this call, matching M1's decision not to introduce a ReadlineState that
  // persists across readline() calls -- nothing needs the kill buffer to
  // outlive one prompt. Each kill overwrites it (last-kill-wins); this is
  // not a ring of multiple kills.
  std::string kill_buffer;

  InputRenderer renderer(prompt, status_line);
  renderer.redraw(buf, cursor);

  core::install_resize_handler();
  auto last_resize_generation = core::resize_generation();

  auto finish = [&](ReadlineExit reason) {
    if (clear_on_submit && reason == ReadlineExit::submitted) {
      // The caller's own transcript will echo this line; leaving it drawn
      // here too would duplicate it and leave stale text on screen for the
      // whole turn. Clear the box back to an empty, ready-for-next-input
      // state instead of committing a trailing newline. The submitted text
      // itself is still returned below — only the on-screen box is emptied.
      renderer.redraw(std::string{}, 0, false);
      raw.leave();
      std::cout << std::flush;
    } else {
      renderer.redraw(buf, cursor, false);
      raw.leave();
      std::cout << "\r\n" << std::flush;
    }
    return ReadlineResult{
        .reason = reason, .text = std::move(buf), .cursor = cursor};
  };

  while (true) {
    std::array<pollfd, 2> descriptors{};
    descriptors[0] = {.fd = STDIN_FILENO, .events = POLLIN};
    nfds_t descriptor_count = 1;
    if (wake_fd >= 0) {
      descriptors[1] = {.fd = wake_fd, .events = POLLIN};
      descriptor_count = 2;
    }

    int poll_result = 0;
    while (true) {
      poll_result = ::poll(descriptors.data(), descriptor_count, -1);
      if (poll_result >= 0)
        break;
      if (errno != EINTR)
        break;
      // A signal (SIGWINCH among others) interrupted the blocking poll.
      // Notice a genuine resize here rather than silently retrying, since
      // nothing else observes it while this thread sits idle in readline().
      const auto generation = core::resize_generation();
      if (generation != last_resize_generation) {
        last_resize_generation = generation;
        if (on_resize)
          on_resize();
        renderer.reanchor();
        renderer.redraw(buf, cursor);
      }
    }
    if (poll_result < 0)
      return finish(ReadlineExit::eof);

    const bool wake_ready = wake_fd >= 0 && (descriptors[1].revents &
                                             (POLLIN | POLLHUP | POLLERR)) != 0;
    if (wake_ready && descriptors[0].revents == 0) {
      // Give a submission already crossing the wake boundary one bounded
      // chance to become readable. This makes wake/submission races
      // deterministic without delaying ordinary wake delivery.
      pollfd stdin_probe{.fd = STDIN_FILENO, .events = POLLIN, .revents = 0};
      const auto probe_result = poll_retry(&stdin_probe, 1, 10);
      if (probe_result > 0)
        descriptors[0].revents = stdin_probe.revents;
    }
    if ((descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
      unsigned char c = 0;
      const auto n = ::read(STDIN_FILENO, &c, 1);
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      if (n == 0) {
        if (wake_ready) {
          drain_nonblocking_fd(wake_fd);
          return finish(ReadlineExit::mailbox_wake);
        }
        return finish(ReadlineExit::eof);
      }
      if (n < 0)
        return finish(ReadlineExit::eof);

      if (c == '\r' || c == '\n')
        return finish(ReadlineExit::submitted);

      if (c == '\x04') // Ctrl+D — EOF
        return finish(ReadlineExit::eof);

      if (c == '\x1b') {
        const auto escape = read_escape_sequence(wake_fd);
        if (escape.wake)
          return finish(ReadlineExit::mailbox_wake);
        if (escape.sequence == "\r") {
          // Alt+Enter (legacy ESC + \r, distinguishable now that ICRNL is
          // cleared): insert a newline instead of submitting.
          buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(cursor), '\n');
          ++cursor;
          renderer.redraw(buf, cursor);
        } else if (escape.sequence == "[200~") {
          // Bracketed paste: the block between start/end markers is always
          // an inert insert, regardless of embedded \n/\r — it must never
          // submit, even if the pasted text ends in a newline.
          auto paste = read_bracketed_paste(wake_fd);
          if (!paste.content.empty()) {
            buf.insert(cursor, paste.content);
            cursor += paste.content.size();
          }
          if (paste.wake)
            return finish(ReadlineExit::mailbox_wake);
          renderer.redraw(buf, cursor);
        } else if (escape.sequence == "b") {
          // Alt+B (legacy ESC + 'b', same encoding family as Alt+Enter
          // above): word-left. Second binding for the same operation as
          // Ctrl+Left, for terminals/multiplexers that don't pass the CSI
          // modifier form through cleanly.
          cursor = previous_word_boundary(buf, cursor);
          renderer.redraw(buf, cursor);
        } else if (escape.sequence == "f") {
          // Alt+F: word-right, mirroring Alt+B above.
          cursor = next_word_boundary(buf, cursor);
          renderer.redraw(buf, cursor);
        } else if (handle_escape_sequence(escape.sequence, control_fn, buf,
                                          cursor)) {
          renderer.redraw(buf, cursor);
        }
      } else if (c == '\t') { // Tab — complete
        if (complete_fn) {
          auto candidates = complete_fn(buf);
          apply_completions(renderer, buf, std::move(candidates));
          cursor = buf.size();
        }
      } else if (c == '\x7f' || c == '\x08') { // Backspace / DEL
        if (cursor > 0) {
          const auto start = previous_utf8_offset(buf, cursor);
          buf.erase(start, cursor - start);
          cursor = start;
          renderer.redraw(buf, cursor);
        }
      } else if (c == '\x01') { // Ctrl+A — line start (same as Home)
        cursor = line_start(buf, cursor);
        renderer.redraw(buf, cursor);
      } else if (c == '\x05') { // Ctrl+E — line end (same as End)
        cursor = line_end(buf, cursor);
        renderer.redraw(buf, cursor);
      } else if (c == '\x17') { // Ctrl+W — delete word behind cursor
        const auto start = previous_word_boundary(buf, cursor);
        if (start < cursor) {
          kill_buffer.assign(buf, start, cursor - start);
          buf.erase(start, cursor - start);
          cursor = start;
          renderer.redraw(buf, cursor);
        }
      } else if (c == '\x15') { // Ctrl+U — kill to line start
        const auto start = line_start(buf, cursor);
        if (start < cursor) {
          kill_buffer.assign(buf, start, cursor - start);
          buf.erase(start, cursor - start);
          cursor = start;
          renderer.redraw(buf, cursor);
        }
      } else if (c == '\x0b') { // Ctrl+K — kill to line end
        const auto end = line_end(buf, cursor);
        if (end > cursor) {
          kill_buffer.assign(buf, cursor, end - cursor);
          buf.erase(cursor, end - cursor);
          renderer.redraw(buf, cursor);
        }
      } else if (c == '\x19') { // Ctrl+Y — yank last kill
        if (!kill_buffer.empty()) {
          buf.insert(cursor, kill_buffer);
          cursor += kill_buffer.size();
          renderer.redraw(buf, cursor);
        }
      } else if (c >= 0x20 || (c & 0x80U) != 0U) {
        buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(cursor),
                   static_cast<char>(c));
        ++cursor;
        renderer.redraw(buf, cursor);
      }

      // If both descriptors were ready, input wins for this iteration. A
      // submitted line is therefore never lost; a printable byte is visible
      // in the draft before the coalesced wake is handled on the next poll.
      continue;
    }

    if (wake_ready) {
      drain_nonblocking_fd(wake_fd);
      return finish(ReadlineExit::mailbox_wake);
    }
  }
}

} // namespace pi::cli
