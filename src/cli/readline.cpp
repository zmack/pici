#include "cli/readline.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
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

constexpr std::string_view kInputCursor = "\033[7m \033[0m\033[D";

void draw_input_cursor() {
  std::cout << "\033[K" << kInputCursor << std::flush;
}

// Redraw the whole line in place (handles partial completion rewrites).
// Leading newlines in prompt are stripped: \r already moves to line start
// and re-emitting a \n would push the cursor down to a new line.
void redraw(std::string_view prompt, const std::string &buf,
            bool show_cursor = true) {
  while (!prompt.empty() && prompt[0] == '\n')
    prompt.remove_prefix(1);
  std::cout << '\r' << prompt << buf << "\033[K";
  if (show_cursor)
    std::cout << kInputCursor;
  std::cout << std::flush;
}

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
void apply_completions(std::string_view prompt, std::string &buf,
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
    redraw(prompt, buf);
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
    redraw(prompt, buf);
    return;
  }

  // Already at common prefix: print candidates below, then redraw prompt.
  // Clear the synthetic input cursor before moving down so it does not remain
  // painted at the old insertion point.
  redraw(prompt, buf, false);
  std::cout << '\n';
  for (const auto &c : completions)
    std::cout << "  " << c << '\n';
  redraw(prompt, buf);
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
                                    const ControlFn &control_fn) {
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
  std::cout << prompt;
  draw_input_cursor();

  while (true) {
    unsigned char c = 0;
    auto n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) {
      // EOF or error
      redraw(prompt, buf, false);
      raw.leave();
      std::cout << '\n' << std::flush;
      return buf.empty() ? std::nullopt : std::optional<std::string>(buf);
    }

    if (c == '\r' || c == '\n') {
      redraw(prompt, buf, false);
      raw.leave();
      std::cout << '\n' << std::flush;
      return buf;
    }

    if (c == '\x04') { // Ctrl+D — EOF
      redraw(prompt, buf, false);
      raw.leave();
      std::cout << '\n' << std::flush;
      return std::nullopt;
    }

    if (c == '\x1b') {
      const auto seq = read_escape_sequence();
      if (handle_escape_sequence(seq, control_fn))
        redraw(prompt, buf);
      continue;
    }

    if (c == '\t') { // Tab — complete
      if (complete_fn) {
        auto candidates = complete_fn(buf);
        apply_completions(prompt, buf, std::move(candidates));
      }
      continue;
    }

    if (c == '\x7f' || c == '\x08') { // Backspace / DEL
      if (!buf.empty()) {
        pop_utf8(buf);
        redraw(prompt, buf);
      }
      continue;
    }

    if (c >= 0x20 || (c & 0x80U) != 0U) { // printable or UTF-8 continuation
      buf += static_cast<char>(c);
      redraw(prompt, buf);
      continue;
    }

    // Other control chars: ignore
  }
}

} // namespace pi::cli
