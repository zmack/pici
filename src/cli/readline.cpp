#include "cli/readline.h"

#include "core/terminal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fdesc, TCSAFLUSH, &raw) != 0)
      return false;
    fd = fdesc;
    active = true;
    return true;
  }

  void leave() {
    if (active) {
      tcsetattr(fd, TCSAFLUSH, &saved);
      active = false;
    }
  }
};

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
    write_wrapped(prompt_, columns, rows, column);
    CursorPosition cursor_position;
    write_wrapped(buf, columns, rows, column, cursor, &cursor_position);

    if (show_cursor && column == columns) {
      std::cout << "\033[K\r\n";
      ++rows;
      column = 0;
    }

    std::cout << "\033[K"; // fill remainder of the final row with the tint
    if (show_cursor) {
      if (rows > cursor_position.row)
        std::cout << "\033[" << rows - cursor_position.row << 'A';
      else if (cursor_position.row > rows)
        std::cout << "\033[" << cursor_position.row - rows << 'B';
      std::cout << '\r';
      if (cursor_position.column > 0)
        std::cout << "\033[" << cursor_position.column << 'C';
      // Toggle reverse video (SGR 7/27) for just the cursor cell without
      // dropping the input area's background tint.
      std::cout << "\033[7m \033[27m";
      if (cursor_position.column > 0)
        std::cout << "\033[D";
      cursor_row_ = cursor_position.row;
    } else {
      cursor_row_ = rows;
    }
    std::cout << "\033[0m"; // leave the tinted box before yielding control
    std::cout << "\033[?7h" << std::flush;
    rendered_rows_ = rows;
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
                            std::size_t cursor_offset = std::string_view::npos,
                            CursorPosition *cursor_position = nullptr) {
    for (std::size_t offset = 0; offset < text.size();) {
      if (cursor_position != nullptr && offset == cursor_offset) {
        cursor_position->row = rows;
        cursor_position->column = column;
      }
      if (const auto escape_length = ansi_escape_length(text, offset);
          escape_length > 0) {
        const auto escape = text.substr(offset, escape_length);
        std::cout.write(escape.data(),
                        static_cast<std::streamsize>(escape.size()));
        offset += escape_length;
        continue;
      }

      if (text[offset] == '\n') {
        // Fill the remainder of the row before wrapping so the input area's
        // background tint covers the whole box, not just the glyphs.
        std::cout << "\033[K\r\n";
        ++rows;
        column = 0;
        ++offset;
        continue;
      }
      if (text[offset] == '\r') {
        std::cout << '\r';
        column = 0;
        ++offset;
        continue;
      }

      const auto length = utf8_length(text, offset);
      const auto width = codepoint_width(
          utf8_codepoint(text, offset, std::min(length, text.size() - offset)));
      if (width > 0 && column > 0 && column + width > columns) {
        std::cout << "\033[K\r\n";
        ++rows;
        column = 0;
      }

      const auto character = text.substr(offset, length);
      std::cout.write(character.data(),
                      static_cast<std::streamsize>(character.size()));
      column += static_cast<std::size_t>(std::max(width, 0));
      offset += length;
    }
    if (cursor_position != nullptr && cursor_offset == text.size()) {
      cursor_position->row = rows;
      cursor_position->column = column;
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

  while (seq.size() < 8) {
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

  if (!control_fn)
    return false;

  if (seq == "[A") {
    control_fn(ControlAction::scroll_line_up);
    return true;
  }
  if (seq == "[B") {
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
  if (seq == "[H" || seq == "OH" || seq == "[1~") {
    control_fn(ControlAction::scroll_top);
    return true;
  }
  if (seq == "[F" || seq == "OF" || seq == "[4~") {
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
        if (handle_escape_sequence(escape.sequence, control_fn, buf, cursor))
          renderer.redraw(buf, cursor);
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
