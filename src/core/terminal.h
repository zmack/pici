#pragma once

#include <array>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <mutex>
#include <signal.h> // NOLINT(modernize-deprecated-headers)
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace pi::core {

int term_width(int fd);
int term_height(int fd);

// Owns the alternate-screen session and the process-wide signal hooks used by
// full-screen renderers. There is deliberately one pending SIGINT flag for
// the whole process, regardless of which renderer owns the session.
class AltScreenSession {
public:
  explicit AltScreenSession(int fd);
  ~AltScreenSession() noexcept;

  AltScreenSession(const AltScreenSession &) = delete;
  AltScreenSession &operator=(const AltScreenSession &) = delete;
  AltScreenSession(AltScreenSession &&) = delete;
  AltScreenSession &operator=(AltScreenSession &&) = delete;

  // Leave the alternate screen and restore the signal handlers. Safe to call
  // repeatedly; the destructor performs the same cleanup as a final guard.
  void leave() noexcept;

private:
  void enter();
  void restore_terminal() noexcept;
  void restore_signal_handlers() noexcept;
  static void atexit_fn();
  static void sig_handler(int sig);

  int fd_;
  bool in_alt_{false};
  bool signals_installed_{false};
  std::array<struct sigaction, 3> previous_actions_{};

  static AltScreenSession *current_;
  static bool atexit_registered_;
};

// Records a Ctrl-C notification. Safe to call from a signal handler.
void notify_sigint() noexcept;

// Returns and clears the first Ctrl-C notification raised by the interactive
// renderer. The signal handler itself only flips a sig_atomic_t flag.
bool consume_sigint();

// Installs a process-wide SIGWINCH handler that bumps a monotonic generation
// counter (see resize_generation()) on every terminal resize. Idempotent and
// safe to call from multiple components (readline, full-screen renderers);
// the handler is installed at most once per process.
void install_resize_handler();

// Monotonically increasing count of SIGWINCH deliveries observed since
// install_resize_handler() was first called. Callers that need to notice a
// resize should snapshot this value and compare it later rather than
// consuming/resetting it, since multiple independent components poll it.
int resize_generation() noexcept;

// Return the display-column width of one terminal line, ignoring ANSI/VT
// escape sequences.
int display_columns(std::string_view line);

// Truncate one ANSI/VT line to `width` display columns without cutting a
// multibyte character or escape sequence. Embedded newlines are discarded.
std::string truncate_ansi_line(std::string_view line, int width);

constexpr std::size_t kMaxTerminalTitleChars = 240;
extern const std::array<std::string_view, 10> kTerminalTitleSpinnerFrames;
constexpr std::chrono::milliseconds kTerminalTitleSpinnerInterval{100};

std::string sanitize_terminal_title(std::string_view title);
std::string format_active_terminal_title(std::string_view base_title,
                                         std::string_view frame);
std::string terminal_title_sequence(std::string_view sanitized_title);
std::string terminal_project_label(const std::filesystem::path &cwd);

enum class TerminalTitleResult { Applied, Skipped };
TerminalTitleResult set_terminal_title(int fd, std::string_view title);
TerminalTitleResult clear_terminal_title(int fd);

class TerminalTitleController {
public:
  explicit TerminalTitleController(int fd, std::string initial_base_title);
  using Writer = std::function<TerminalTitleResult(std::string_view)>;
  TerminalTitleController(int fd, std::string initial_base_title, Writer writer,
                          std::chrono::milliseconds interval);
  TerminalTitleController(int fd, std::string initial_base_title, Writer writer,
                          std::chrono::milliseconds interval, bool is_tty);
  ~TerminalTitleController() noexcept;

  TerminalTitleController(const TerminalTitleController &) = delete;
  TerminalTitleController &operator=(const TerminalTitleController &) = delete;
  TerminalTitleController(TerminalTitleController &&) = delete;
  TerminalTitleController &operator=(TerminalTitleController &&) = delete;

  void set_base_title(std::string title);
  void start_activity();
  void stop_activity();

private:
  TerminalTitleResult write_title(std::string_view sanitized);
  void emit_sanitized(std::string_view sanitized);
  void worker_loop(std::stop_token st);

  int fd_{-1};
  Writer writer_;
  std::chrono::milliseconds interval_{kTerminalTitleSpinnerInterval};
  bool is_tty_{false};

  mutable std::mutex mutex_;
  std::string base_title_;
  std::string last_emitted_;
  bool has_applied_{false};
  bool active_{false};
  std::size_t frame_index_{0};

  std::condition_variable_any cv_;
  std::mutex cv_mutex_;
  std::jthread worker_;
};

class TerminalTitleActivityGuard {
public:
  explicit TerminalTitleActivityGuard(TerminalTitleController &controller);
  ~TerminalTitleActivityGuard() noexcept;

  TerminalTitleActivityGuard(const TerminalTitleActivityGuard &) = delete;
  TerminalTitleActivityGuard &
  operator=(const TerminalTitleActivityGuard &) = delete;
  TerminalTitleActivityGuard(TerminalTitleActivityGuard &&) = delete;
  TerminalTitleActivityGuard &operator=(TerminalTitleActivityGuard &&) = delete;

private:
  TerminalTitleController &controller_;
};

// Skip one ANSI/VT escape sequence starting at s[i].
//
// Handles:
//   CSI  \033[...X   — parameter bytes 0x30-0x3F, intermediate 0x20-0x2F,
//                       final byte 0x40-0x7E
//   OSC  \033]...ST  — terminated by ST (\033\\) or BEL (\007)
//   DCS/PM/APC       — \033[P^_] ... ST
//   Two-byte Fe      — \033 followed by a single byte in 0x40-0x5F that is
//                       not a multi-char introducer
//
// Returns the position after the sequence, or `i` if s[i] is not \033 or
// the sequence is malformed / unrecognised.
std::size_t skip_ansi_sequence(std::string_view s, std::size_t i);

// Advance past one UTF-8 codepoint at s[i].
// Returns the byte position after the codepoint, clamped to s.size().
// If the byte sequence is truncated (partial codepoint at end of string),
// returns s.size() so the caller can safely stop.
std::size_t advance_utf8(std::string_view s, std::size_t i);

// Display-column width of the UTF-8 codepoint at s[i].
//   0  — non-printing or combining (control chars, combining diacritics)
//   1  — narrow (most Latin, Cyrillic, Greek, etc.)
//   2  — wide (CJK ideographs, Hangul, fullwidth forms, most emoji)
//
// Based on Unicode Standard Annex #11 East Asian Width; covers the ranges
// most commonly produced by LLM output. Not locale-dependent.
int codepoint_width(std::string_view s, std::size_t i);

// Visual terminal rows occupied by `line` (no embedded \\n) at `width` cols.
// Accounts for ANSI/VT escapes, UTF-8, and wide characters.
int rows_for_line(std::string_view line, int width);

// Total visual rows from the start of `rendered` to the end at `width` cols.
// Counts every physical row, including those caused by terminal wrapping.
int cursor_rows_for_rendered(std::string_view rendered, int width);

// Split an ANSI-rendered string into physical terminal rows, wrapping at the
// requested display width without counting escape sequences toward that width.
std::vector<std::string> split_lines(std::string_view s, int width);

// Truncate a tool result's content to the built-in 5-line / first-2-last-2
// display format. Used by VerboseRenderer and exposed to Lua as
// pici.truncate_tool_result(content).
std::string truncate_tool_result(std::string_view content);

// Sanitize a Lua-returned tool-call or tool-result string before writing to
// the terminal. Strips invalid UTF-8 and disallows cursor-movement /
// screen-clearing escape sequences while preserving SGR (color) codes.
std::string sanitize_tool_output(std::string_view text);

//
// Incrementally scans raw markdown content (the append-only `content` string
// that ViewportRenderer builds from thinking + raw buffers) to find the last
// "stable block boundary" — a byte offset p where:
//   1. content[p-2..p-1] == "\n\n"
//   2. We are not inside an open code fence at that position.
//   3. The last completed non-blank line before the blank line had indent == 0.
//
// Call advance() with the full growing content string on each update; only
// bytes from scan_pos onward are processed (O(new bytes) per call).
// Reset with `= {}` between turns or when the prefix becomes stale.
struct BlockBoundaryScanner {
  std::size_t scan_pos{0};    // bytes consumed in prior advance() calls
  std::size_t last_stable{0}; // last confirmed stable boundary offset

  // Fence tracking (persists across advance() calls)
  bool inside_fence{false};
  char fence_char{0}; // '`' or '~' (valid when inside_fence)
  int fence_len{0};   // opening run length (≥3 when inside_fence)

  // Current-line analysis (reset at each '\n')
  int line_indent{0};         // leading spaces/tabs before first non-space
  bool saw_nonspace{false};   // have we seen a non-space char on this line
  int fence_run{0};           // consecutive fence_run_char at line start
  char fence_run_char{0};     // the char being counted
  bool line_only_fence{true}; // no non-fence, non-space chars after the run yet

  // Cross-line state
  bool last_nonblank_col0{
      true};                // last completed non-blank line had indent == 0
  bool last_char_nl{false}; // previous byte processed was '\n'

  void advance(std::string_view s);
};

} // namespace pi::core
