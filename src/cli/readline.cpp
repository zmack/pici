#include "cli/readline.h"

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <termios.h>
#include <unistd.h>
#include <vector>

namespace pi::cli {

namespace {

// RAII guard: put terminal into raw mode while alive.
// ISIG is kept enabled so Ctrl+C still delivers SIGINT.
struct RawMode {
  int fd{-1};
  struct termios saved {};
  bool active{false};

  bool enter(int fdesc) {
    if (!isatty(fdesc)) return false;
    if (tcgetattr(fdesc, &saved) != 0) return false;
    struct termios raw = saved;
    raw.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(fdesc, TCSAFLUSH, &raw) != 0) return false;
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

  ~RawMode() { leave(); }
};

// Redraw the whole line in place (handles partial completion rewrites).
void redraw(std::string_view prompt, const std::string &buf) {
  // \r to line start, prompt+buf, \033[K clears the rest of the line
  std::cout << '\r' << prompt << buf << "\033[K" << std::flush;
}

// Remove the last UTF-8 character from buf (backs over continuation bytes).
void pop_utf8(std::string &buf) {
  if (buf.empty()) return;
  buf.pop_back();
  while (!buf.empty() && (static_cast<unsigned char>(buf.back()) & 0xC0u) == 0x80u)
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
  std::sort(completions.begin(), completions.end());

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

  // Already at common prefix: print candidates below, then redraw prompt
  std::cout << '\n';
  for (const auto &c : completions)
    std::cout << "  " << c << '\n';
  redraw(prompt, buf);
}

} // namespace

std::optional<std::string> readline(std::string_view prompt,
                                    CompleteFn complete_fn) {
  // Non-TTY fallback: just use getline (pipes, scripts, tests)
  if (!isatty(STDIN_FILENO)) {
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
  }

  RawMode raw;
  if (!raw.enter(STDIN_FILENO)) {
    // Couldn't enter raw mode — fall back
    std::cout << prompt << std::flush;
    std::string line;
    if (!std::getline(std::cin, line)) return std::nullopt;
    return line;
  }

  std::cout << prompt << std::flush;
  std::string buf;

  while (true) {
    unsigned char c = 0;
    auto n = ::read(STDIN_FILENO, &c, 1);
    if (n <= 0) {
      // EOF or error
      raw.leave();
      std::cout << '\n' << std::flush;
      return buf.empty() ? std::nullopt : std::optional<std::string>(buf);
    }

    if (c == '\r' || c == '\n') {
      raw.leave();
      std::cout << '\n' << std::flush;
      return buf;
    }

    if (c == '\x04') { // Ctrl+D — EOF
      raw.leave();
      std::cout << '\n' << std::flush;
      return std::nullopt;
    }

    if (c == '\x1b') { // ESC — consume ANSI escape sequence, ignore
      unsigned char seq[2] = {0, 0};
      if (::read(STDIN_FILENO, &seq[0], 1) > 0 && seq[0] == '[')
        ::read(STDIN_FILENO, &seq[1], 1);
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

    if (c >= 0x20 || (c & 0x80u) != 0u) { // printable or UTF-8 continuation
      buf += static_cast<char>(c);
      std::cout << static_cast<char>(c) << std::flush;
      continue;
    }

    // Other control chars: ignore
  }
}

} // namespace pi::cli
