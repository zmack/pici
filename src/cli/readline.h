#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace pi::cli {

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
ReadlineResult readline(std::string_view prompt,
                        const CompleteFn &complete_fn = {},
                        const ControlFn &control_fn = {},
                        std::string_view status_line = {},
                        std::string_view initial_draft = {},
                        std::size_t initial_cursor = std::string_view::npos,
                        int wake_fd = -1, bool clear_on_submit = false,
                        const std::function<void()> &on_resize = {});

} // namespace pi::cli
