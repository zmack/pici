#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <termios.h>

namespace pi::cli {

// RAII guard: puts fd into raw mode (no ECHO/ICANON; ISIG stays enabled so
// Ctrl+C still delivers SIGINT) for as long as it's active.
//
// readline() manages its own instance by default, entered/left around each
// call. A caller that owns a persistent full-screen compositor (e.g.
// RegionRenderer's alt-screen) should instead construct one of these before
// its interactive loop starts and pass it via readline()'s
// external_raw_mode parameter -- otherwise the terminal reverts to
// cooked/echo mode for the whole span of a turn (no readline() call in
// flight), and stray keystrokes get echoed by the tty driver straight into
// the compositor's fixed layout instead of being captured, then silently
// dropped when the next readline() call re-enters raw mode (TCSAFLUSH
// discards unread input on a termios switch).
class TerminalRawMode {
public:
  TerminalRawMode() = default;
  ~TerminalRawMode();
  TerminalRawMode(const TerminalRawMode &) = delete;
  TerminalRawMode &operator=(const TerminalRawMode &) = delete;
  TerminalRawMode(TerminalRawMode &&) = delete;
  TerminalRawMode &operator=(TerminalRawMode &&) = delete;

  bool enter(int fd);
  void leave();
  bool active() const { return active_; }

private:
  int fd_{-1};
  struct termios saved_ {};
  bool active_{false};
  bool kitty_pushed_{false};
};

// Called with the current buffer when Tab is pressed.
// Returns candidate completion strings.
using CompleteFn = std::function<std::vector<std::string>(std::string_view)>;

enum class ControlAction {
  scroll_line_up,
  scroll_line_down,
  scroll_page_up,
  scroll_page_down,
  scroll_top,
  scroll_bottom,
};

using ControlFn = std::function<void(ControlAction)>;

enum class ReadlineExit {
  submitted,
  eof,
  mailbox_wake,
};

struct ReadlineResult {
  ReadlineExit reason{ReadlineExit::eof};
  std::string text;
  // Byte offset in the UTF-8 buffer, always kept on a codepoint boundary.
  std::size_t cursor{0};
};

// A process-local, nonblocking wake channel. Writes coalesce while the
// channel is readable; both descriptors are close-on-exec and owned by this
// object for its entire lifetime.
class ReadlineWake {
public:
  ReadlineWake();
  ~ReadlineWake();
  ReadlineWake(const ReadlineWake &) = delete;
  ReadlineWake &operator=(const ReadlineWake &) = delete;
  ReadlineWake(ReadlineWake &&other) noexcept;
  ReadlineWake &operator=(ReadlineWake &&other) noexcept;

  int read_fd() const noexcept { return read_fd_; }
  bool notify() const noexcept;
  void drain() const noexcept;

private:
  int read_fd_{-1};
  int write_fd_{-1};
};

// Read one line from stdin with optional tab completion.
//
// - When stdin is a TTY, enters raw mode so Tab is intercepted before the
//   terminal driver sees it.  Single match: replace buffer + trailing space.
//   Multiple matches: complete to common prefix; if already there, print list.
//   No matches: bell.
// - When stdin is not a TTY (pipe/script), falls back to std::getline so
//   automated input works normally.
//
// In TTY mode, an optional wake fd interrupts editing and returns the draft
// without submitting it. The caller can pass the returned text and cursor to
// the next invocation. Non-TTY input retains blocking getline behavior.
//
// clear_on_submit: when true, a submitted line is erased from the terminal
// instead of being left in place with a trailing newline. Full-screen
// renderers that echo the request in their own scrolling history (so the
// text would otherwise appear twice, and linger for the whole turn) should
// pass true; renderers with no separate transcript view should pass false so
// the typed line remains in normal terminal scrollback.
//
// on_resize: invoked when a terminal resize is observed while editing, after
// this function has redrawn its own line but before continuing to poll. Use
// it to let a full-screen renderer repaint its own fixed layout.
//
// vim_mode: when true, a VimEngine (cli/vim_mode.h) intercepts Normal-mode
// key input ahead of the plain key-dispatch chain below -- see
// composer-textarea-rewrite.md's M5 section. Config-driven
// ([input] vim_mode in config.toml), off by default; every other call site
// keeps working unchanged since this defaults to false.
//
// external_raw_mode: nullptr (default) means this call enters and leaves its
// own raw mode, exactly scoped to this one call, as above. Pass an already-
// entered TerminalRawMode owned by the caller instead to skip that per-call
// enter/leave -- see TerminalRawMode's comment for why a full-screen caller
// wants to do this.
ReadlineResult
readline(std::string_view prompt, const CompleteFn &complete_fn = {},
         const ControlFn &control_fn = {}, std::string_view status_line = {},
         std::string_view initial_draft = {},
         std::size_t initial_cursor = std::string_view::npos, int wake_fd = -1,
         bool clear_on_submit = false,
         const std::function<void()> &on_resize = {}, bool vim_mode = false,
         TerminalRawMode *external_raw_mode = nullptr);

// --- Shared editing primitives -------------------------------------------
//
// Buffer-navigation primitives defined in readline.cpp, also used by
// cli/vim_mode.cpp's VimEngine (see composer-textarea-rewrite.md, M3 design
// decision 9: M5's vim mode consumes these directly rather than re-deriving
// word/line-boundary logic). Operate on a '\n'-delimited UTF-8 buffer and a
// byte cursor, always kept on a codepoint boundary.
std::size_t previous_utf8_offset(std::string_view buf, std::size_t cursor);
std::size_t next_utf8_offset(std::string_view buf, std::size_t cursor);
std::size_t line_start(std::string_view buf, std::size_t cursor);
std::size_t line_end(std::string_view buf, std::size_t cursor);
std::size_t previous_word_boundary(std::string_view buf, std::size_t cursor);
std::size_t next_word_boundary(std::string_view buf, std::size_t cursor);
std::size_t previous_line_offset(std::string_view buf, std::size_t cursor);
std::size_t next_line_offset(std::string_view buf, std::size_t cursor);

} // namespace pi::cli
