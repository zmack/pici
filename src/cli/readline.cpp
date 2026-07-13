#include "cli/readline.h"

#include "core/terminal.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pi::cli {

namespace {

// RAII guard: put terminal into raw mode while alive.
// ISIG is kept enabled so Ctrl+C still delivers SIGINT.
struct RawMode {
  int fd{-1};
  struct termios saved{};
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
  if (codepoint == 0 || codepoint < 0x20U)
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
  struct winsize size{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
    return size.ws_col;
  return 80;
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

  void redraw(const std::string &buf, bool show_cursor = true) {
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
    write_wrapped(prompt_, columns, rows, column);
    write_wrapped(buf, columns, rows, column);

    if (show_cursor && column == columns) {
      std::cout << "\r\n";
      ++rows;
      column = 0;
    }

    std::cout << "\033[K";
    if (show_cursor) {
      std::cout << "\033[7m \033[0m";
      if (column > 0)
        std::cout << "\033[D";
    }
    std::cout << "\033[?7h" << std::flush;
    rendered_rows_ = rows;
  }

  // External output (completion candidates) invalidates the cursor anchor.
  void invalidate() { rendered_rows_ = 0; }

private:
  void clear_previous() {
    if (rendered_rows_ == 0)
      return;

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

  void write_wrapped(std::string_view text, std::size_t columns,
                     std::size_t &rows, std::size_t &column) {
    for (std::size_t offset = 0; offset < text.size();) {
      if (const auto escape_length = ansi_escape_length(text, offset);
          escape_length > 0) {
        std::cout.write(text.data() + static_cast<std::streamoff>(offset),
                        static_cast<std::streamsize>(escape_length));
        offset += escape_length;
        continue;
      }

      if (text[offset] == '\n') {
        std::cout << "\r\n";
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
        std::cout << "\r\n";
        ++rows;
        column = 0;
      }

      std::cout.write(text.data() + static_cast<std::streamoff>(offset),
                      static_cast<std::streamsize>(length));
      column += static_cast<std::size_t>(std::max(width, 0));
      ++offset;
    }
  }

  std::string prompt_;
  std::string status_line_;
  std::size_t leading_newlines_{0};
  std::size_t rendered_rows_{0};
  bool first_draw_{true};
};

// Remove the last UTF-8 character from buf (backs over continuation bytes).
void pop_utf8(std::string &buf) {
  if (buf.empty())
    return;
  buf.pop_back();
  while (!buf.empty() &&
         (static_cast<unsigned char>(buf.back()) & 0xC0U) == 0x80U)
    buf.pop_back();
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
    renderer.redraw(buf);
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
    renderer.redraw(buf);
    return;
  }

  // Already at common prefix: print candidates below, then redraw prompt.
  // Clear the synthetic input cursor before moving down so it does not remain
  // painted at the old insertion point.
  renderer.redraw(buf, false);
  std::cout << "\r\n";
  for (const auto &c : completions)
    std::cout << "  " << c << '\n';
  renderer.invalidate();
  renderer.redraw(buf);
}

std::string read_escape_sequence() {
  std::string seq;
  unsigned char c = 0;
  if (::read(STDIN_FILENO, &c, 1) <= 0)
    return seq;
  seq += static_cast<char>(c);

  if (c != '[' && c != 'O')
    return seq;

  while (seq.size() < 8) {
    if (::read(STDIN_FILENO, &c, 1) <= 0)
      break;
    seq += static_cast<char>(c);
    if ((c >= '@' && c <= '~'))
      break;
  }
  return seq;
}

bool handle_escape_sequence(std::string_view seq, const ControlFn &control_fn) {
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

std::optional<std::string> readline(std::string_view prompt,
                                    const CompleteFn &complete_fn,
                                    const ControlFn &control_fn,
                                    std::string_view status_line) {
  // Non-TTY fallback: just use getline (pipes, scripts, tests)
  if (isatty(STDIN_FILENO) == 0) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line))
      return std::nullopt;
    return line;
  }

  RawMode raw;
  if (!raw.enter(STDIN_FILENO)) {
    // Couldn't enter raw mode — fall back
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line))
      return std::nullopt;
    return line;
  }

  std::string buf;
  InputRenderer renderer(prompt, status_line);
  renderer.redraw(buf);

  while (true) {
    unsigned char c = 0;
    auto n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) {
      // EOF or error
      renderer.redraw(buf, false);
      raw.leave();
      std::cout << "\r\n" << std::flush;
      return buf.empty() ? std::nullopt : std::optional<std::string>(buf);
    }

    if (c == '\r' || c == '\n') {
      renderer.redraw(buf, false);
      raw.leave();
      std::cout << "\r\n" << std::flush;
      return buf;
    }

    if (c == '\x04') { // Ctrl+D — EOF
      renderer.redraw(buf, false);
      raw.leave();
      std::cout << "\r\n" << std::flush;
      return std::nullopt;
    }

    if (c == '\x1b') {
      const auto seq = read_escape_sequence();
      if (handle_escape_sequence(seq, control_fn))
        renderer.redraw(buf);
      continue;
    }

    if (c == '\t') { // Tab — complete
      if (complete_fn) {
        auto candidates = complete_fn(buf);
        apply_completions(renderer, buf, std::move(candidates));
      }
      continue;
    }

    if (c == '\x7f' || c == '\x08') { // Backspace / DEL
      if (!buf.empty()) {
        pop_utf8(buf);
        renderer.redraw(buf);
      }
      continue;
    }

    if (c >= 0x20 || (c & 0x80U) != 0U) { // printable or UTF-8 continuation
      buf += static_cast<char>(c);
      renderer.redraw(buf);
      continue;
    }

    // Other control chars: ignore
  }
}

} // namespace pi::cli
