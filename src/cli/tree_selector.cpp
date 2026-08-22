#include "cli/tree_selector.h"

#include "core/session/session_tree.h"
#include "core/terminal.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/select.h> // NOLINT(misc-include-cleaner)
#include <termios.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pi::cli {

namespace {

// RAII raw mode — identical to the one in readline.cpp.
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

enum class Key { Up, Down, Enter, Esc, Other };

// Read one logical key from stdin (handles arrow escape sequences).
// A bare \033 with nothing following within 50 ms is treated as Esc.
Key read_key() {
  unsigned char c = 0;
  if (::read(STDIN_FILENO, &c, 1) <= 0)
    return Key::Esc;

  if (c == 'k')
    return Key::Up;
  if (c == 'j')
    return Key::Down;
  if (c == 'q')
    return Key::Esc;
  if (c == '\r' || c == '\n')
    return Key::Enter;

  if (c != '\033')
    return Key::Other;

  // Check whether the escape is the start of a CSI sequence.
  fd_set fds;                 // NOLINT(misc-include-cleaner)
  FD_ZERO(&fds);              // NOLINT(misc-include-cleaner)
  FD_SET(STDIN_FILENO, &fds); // NOLINT(misc-include-cleaner)
  struct timeval tv {};       // NOLINT(misc-include-cleaner)
  tv.tv_usec = 50L * 1000L;   // 50 ms
  // NOLINTNEXTLINE(misc-include-cleaner)
  if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0)
    return Key::Esc; // bare Esc

  unsigned char next = 0;
  if (::read(STDIN_FILENO, &next, 1) <= 0)
    return Key::Esc;
  if (next != '[')
    return Key::Esc;

  unsigned char arrow = 0;
  if (::read(STDIN_FILENO, &arrow, 1) <= 0)
    return Key::Esc;
  if (arrow == 'A')
    return Key::Up;
  if (arrow == 'B')
    return Key::Down;
  return Key::Other;
}

void write_str(const char *s) {
  core::write_best_effort(STDOUT_FILENO, s, std::strlen(s));
}
void write_str(const std::string &s) {
  core::write_best_effort(STDOUT_FILENO, s.data(), s.size());
}

// Render the visible window of lines into the alternate screen.
// header (title) + footer always shown; middle is the scrolling window.
void render(const std::vector<core::SessionTreeLine> &lines, std::size_t cursor,
            std::size_t view_top, int visible_rows) {
  // Move to top of screen and clear
  write_str("\033[H\033[J");

  write_str("  Session Tree\n\n");

  for (int row = 0; row < visible_rows; ++row) {
    std::size_t idx = view_top + static_cast<std::size_t>(row);
    if (idx >= lines.size())
      break;

    const bool is_cursor = (idx == cursor);
    if (is_cursor)
      write_str("\033[7m"); // reverse-video

    write_str("  ");
    write_str(lines[idx].text);

    if (is_cursor)
      write_str("\033[0m");

    write_str("\033[K\n"); // erase to EOL then newline
  }

  write_str("\n  \033[2m↑↓ navigate   Enter switch   Esc cancel\033[0m\n");
}

} // namespace

TreeSelectorResult
run_tree_selector(const std::vector<core::SessionTreeLine> &lines,
                  const std::string & /*current_session_id*/,
                  std::size_t initial_cursor, bool caller_owns_alt_screen) {
  if (lines.empty())
    return {.cancelled = true};

  const int h = core::term_height(STDOUT_FILENO);
  // header (2 lines: title + blank) + footer (2 lines: blank + hint)
  const int visible_rows = std::max(1, h - 4);

  RawMode raw;
  const bool tty = raw.enter(STDIN_FILENO);

  // A caller that already owns a persistent alt-screen session (the region
  // renderer) must not have it nested: entering `1049h` while already
  // inside the alternate screen clears it without saving, and the matching
  // `1049l` on exit drops back to the primary screen buffer the owning
  // renderer never painted, corrupting the whole display. Draw straight
  // into the existing screen instead; the caller repaints afterward.
  if (!caller_owns_alt_screen)
    write_str("\033[?1049h");

  std::size_t cursor = initial_cursor;
  // Scroll the window so the cursor is visible.
  std::size_t view_top = 0;
  if (std::cmp_greater_equal(cursor, visible_rows))
    view_top = cursor - (static_cast<std::size_t>(visible_rows) / 2);

  render(lines, cursor, view_top, visible_rows);

  TreeSelectorResult result{.cancelled = true};

  while (true) {
    Key k = read_key();

    if (k == Key::Esc) {
      result.cancelled = true;
      break;
    }
    if (k == Key::Enter) {
      result.cancelled = false;
      result.selected_session_id = lines[cursor].session_id;
      break;
    }
    if (k == Key::Up && cursor > 0) {
      --cursor;
      view_top = std::min(view_top, cursor);
    }
    if (k == Key::Down && cursor + 1 < lines.size()) {
      ++cursor;
      const auto vr = static_cast<std::size_t>(visible_rows);
      if (cursor >= view_top + vr)
        view_top = cursor - vr + 1;
    }

    render(lines, cursor, view_top, visible_rows);
  }

  // Exit alternate screen (restores the previous terminal content)
  if (!caller_owns_alt_screen)
    write_str("\033[?1049l");

  if (!tty)
    raw.leave(); // harmless no-op; leave() checks active flag

  return result;
}

} // namespace pi::cli
