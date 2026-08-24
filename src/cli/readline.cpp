#include "cli/readline.h"

#include "cli/vim_mode.h"
#include "cli/wrap.h"
#include "core/terminal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/fcntl.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/types.h>
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

// Process-wide leftover bytes from the Kitty-keyboard capability probe (see
// kitty_keyboard_enabled below): whatever the probe's bounded read window
// swallowed that wasn't part of either reply it was looking for -- a fast
// typist's keystrokes, or stray bytes from a terminal/multiplexer that
// doesn't understand the query. Drained by read_stdin_byte/poll_stdin_retry
// before any real read of stdin, so it's never lost and never reordered
// relative to bytes actually typed afterward. The probe only ever runs
// once per process (see kitty_keyboard_enabled), so this is populated at
// most once, during the very first RawMode::enter() call.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::deque<char> g_pending_stdin_bytes;

// Every raw-byte read of stdin in this file (the main dispatch loop,
// read_escape_sequence, read_bracketed_paste) goes through this instead of
// calling ::read() directly, so bytes queued in g_pending_stdin_bytes are
// replayed before any new byte is actually read from the fd. Return value
// matches ::read(fd, &out, 1): 1 on success, 0 on EOF, -1 on error (errno
// set).
ssize_t read_stdin_byte(unsigned char &out) {
  if (!g_pending_stdin_bytes.empty()) {
    out = static_cast<unsigned char>(g_pending_stdin_bytes.front());
    g_pending_stdin_bytes.pop_front();
    return 1;
  }
  return ::read(STDIN_FILENO, &out, 1);
}

// Companion to read_stdin_byte: reports stdin as immediately readable
// without actually calling poll() whenever bytes are already queued, so
// replaying them never waits out the timeout that governed the original
// blocking read. Falls back to a real poll_retry() otherwise.
int poll_stdin_retry(struct pollfd *descriptors, nfds_t count, int timeout_ms) {
  if (!g_pending_stdin_bytes.empty()) {
    descriptors->revents = static_cast<short>(POLLIN);
    // count is always 1 or 2 (stdin, optionally the wake fd) at every call
    // site in this file, and descriptors always points at an array with at
    // least `count` elements -- the +1 stays in bounds.
    if (count > 1)
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      (descriptors + 1)->revents = 0;
    return 1;
  }
  return poll_retry(descriptors, count, timeout_ms);
}

// How long the Kitty-keyboard-protocol probe below will wait for a DA1
// reply before giving up and assuming the protocol isn't supported. DA1
// replies are near-instant on a real terminal; this only matters for a
// terminal that answers neither query, where it bounds startup latency
// instead of hanging indefinitely.
constexpr std::chrono::milliseconds kKittyProbeTimeout{300};

// Probes for Kitty keyboard protocol support (the "disambiguate escape
// codes" progressive-enhancement flag -- see
// https://sw.kovidgoyal.net/kitty/keyboard-protocol/) once per process: the
// result is a function-local static, computed on the first call and simply
// read back on every later one, so raw mode being entered on every
// readline() call and every mailbox wake never repeats the query/DA1 round
// trip -- see composer-textarea-rewrite.md's M4 section for why that
// matters. Must only be called once raw mode (no ECHO/ICANON, ICRNL
// cleared) is already active on stdin, so the reply can be read back
// cleanly.
bool kitty_keyboard_enabled() {
  static const bool supported = [] {
    // Explicit escape hatch: tmux only forwards Kitty-protocol key reports
    // with `set -g extended-keys on` (tmux >= 3.4); older tmux, screen, and
    // other multiplexer/terminfo shims may swallow the query outright (the
    // probe below already concludes "unsupported" for that case, which is
    // fine) or answer on the real terminal's behalf in a way that looks
    // like support without key reports actually being forwarded end-to-end.
    // This override skips the probe entirely and forces the permanent
    // plain-Enter-submits/Alt+Enter-inserts fallback regardless of what it
    // would otherwise conclude.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const char *disable = std::getenv("PICI_DISABLE_KITTY_KEYBOARD");
        disable != nullptr && *disable != '\0' &&
        std::string_view(disable) != "0")
      return false;

    // Query support ("CSI ?u", asking the terminal to report its current
    // progressive-enhancement flags) followed immediately by a DA1 query
    // ("CSI c") as a sentinel: virtually every terminal answers DA1 even if
    // it has no idea what "CSI ?u" means, so DA1's reply is the signal to
    // stop waiting rather than always paying a fixed timeout. Written
    // directly to the fd rather than through std::cout, so it can't be left
    // sitting in an iostream buffer while probe_terminal_capability blocks
    // waiting for the reply.
    static constexpr std::string_view kQuery = "\033[?u\033[c";
    if (::write(STDOUT_FILENO, kQuery.data(), kQuery.size()) !=
        static_cast<ssize_t>(kQuery.size()))
      return false;

    auto result = core::probe_terminal_capability(
        STDIN_FILENO,
        [](std::string_view buf, std::size_t i) {
          return core::match_dec_private_reply(buf, i, 'u');
        },
        kKittyProbeTimeout);

    // Whatever wasn't consumed by either reply must not be lost -- queue it
    // for read_stdin_byte to hand back to the normal input path.
    g_pending_stdin_bytes.insert(g_pending_stdin_bytes.end(),
                                 result.leftover.begin(),
                                 result.leftover.end());
    return result.supported;
  }();
  return supported;
}

// How long the OSC 11 background-color probe below will wait for a DA1
// reply before giving up -- same rationale as kKittyProbeTimeout above,
// just for a different query.
constexpr std::chrono::milliseconds kOsc11ProbeTimeout{300};

// The input box's background tint before any terminal-background query --
// used outright on a terminal without confirmed truecolor support, and as
// the fallback if the query times out or the reply can't be parsed. SGR 100
// (bright-black background) renders as a distinguishable mid-gray under
// most terminal color themes, dark and light alike.
constexpr std::string_view kFallbackInputAreaBackground = "\033[100m";

// Extracts one 0-255 color component from an OSC 11 reply's "rrrr"/"gggg"/
// "bbbb" hex token (xterm's format allows 1-4 hex digits per component;
// only 1-2 are seen in practice). Takes the first (up to) two hex digits --
// i.e. the high byte of whatever width arrives -- which is the usual
// normalization for this reply and is precise for the common 2-digit case.
bool parse_osc_color_component(std::string_view token, int &out) {
  if (token.empty() || token.size() > 4)
    return false;
  const auto hex_value = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };
  const int d0 = hex_value(token[0]);
  if (d0 < 0)
    return false;
  if (token.size() == 1) {
    out = d0 * 17; // nibble replication for a bare single hex digit
    return true;
  }
  const int d1 = hex_value(token[1]);
  if (d1 < 0)
    return false;
  out = d0 * 16 + d1;
  return true;
}

// Parses R/G/B out of one complete OSC 11 reply already matched by
// core::match_osc_color_reply, e.g. "\033]11;rgb:1e1e/1e1e/2222\033\\" or
// "\033]11;rgb:1e/1e/22\007". Rejects anything that isn't three '/'-
// separated hex tokens after the first ':' -- including the "rgba:" form
// some terminals use, which this milestone doesn't need to special-case
// since falling back to the fixed tint is always safe.
bool parse_osc_color_reply(std::string_view reply, int &r, int &g, int &b) {
  const auto colon = reply.find(':');
  if (colon == std::string_view::npos)
    return false;
  auto body = reply.substr(colon + 1);
  if (!body.empty() && body.back() == '\007')
    body.remove_suffix(1);
  else if (body.size() >= 2 && body.substr(body.size() - 2) == "\033\\")
    body.remove_suffix(2);

  // Pulls off the next '/'-delimited token from `body`, advancing past it
  // (and the separator) -- three calls below, one per component, instead
  // of an indexed loop.
  const auto next_token =
      [&body](bool expect_more) -> std::optional<std::string_view> {
    const auto slash = body.find('/');
    if ((slash == std::string_view::npos) == expect_more)
      return std::nullopt; // wrong number of '/'-separated tokens
    if (slash == std::string_view::npos) {
      const auto token = body;
      body = {};
      return token;
    }
    const auto token = body.substr(0, slash);
    body.remove_prefix(slash + 1);
    return token;
  };

  const auto r_token = next_token(true);
  if (!r_token || !parse_osc_color_component(*r_token, r))
    return false;
  const auto g_token = next_token(true);
  if (!g_token || !parse_osc_color_component(*g_token, g))
    return false;
  const auto b_token = next_token(false);
  return b_token.has_value() && parse_osc_color_component(*b_token, b);
}

// Nudges the terminal's own queried background lightness by a small, fixed
// amount instead of blending toward a second reference color -- keeps the
// tint readably distinct in both light and dark themes without needing one.
// ~12% is deliberately modest: "barely there but perceptible" is the goal,
// matching what the fixed \033[100m fallback above aims for by different
// means.
std::string blend_input_area_tint(int r, int g, int b) {
  constexpr double kBlendAmount = 0.12;
  const double luminance = 0.2126 * r + 0.7152 * g + 0.0722 * b;
  const bool dark = luminance < 128.0;
  const auto blend_channel = [&](int channel) {
    const double target = dark ? 255.0 : 0.0;
    const double blended = channel + (target - channel) * kBlendAmount;
    return std::clamp(static_cast<int>(std::lround(blended)), 0, 255);
  };
  return "\033[48;2;" + std::to_string(blend_channel(r)) + ';' +
         std::to_string(blend_channel(g)) + ';' +
         std::to_string(blend_channel(b)) + 'm';
}

// Computes the input box's background tint once per process (see
// input_area_background() below). Truecolor output (needed to blend at
// all) has no reliable terminal-query protocol, so this uses the standard
// pragmatic check most CLI tools use instead: COLORTERM set to "truecolor"
// or "24bit". Without it, the OSC 11 query below is skipped entirely --
// this function returns the fixed fallback immediately, without touching
// the terminal -- since blending without confirmed truecolor support would
// just produce a broken SGR sequence.
std::string compute_input_area_background() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe)
  const char *colorterm = std::getenv("COLORTERM");
  const bool truecolor =
      colorterm != nullptr && (std::string_view(colorterm) == "truecolor" ||
                               std::string_view(colorterm) == "24bit");
  if (!truecolor)
    return std::string(kFallbackInputAreaBackground);

  // OSC 11 background-color query, BEL-terminated -- the more broadly
  // compatible choice, since some terminals only answer a BEL-terminated
  // query even though they'd accept either terminator on the way in; the
  // reply itself is accepted with either terminator regardless (see
  // core::match_osc_color_reply). Followed immediately by the same DA1
  // sentinel the Kitty probe above uses, in one write so it can't be left
  // sitting in an iostream buffer while probe_terminal_capability blocks
  // waiting for the reply.
  static constexpr std::string_view kQuery = "\033]11;?\007\033[c";
  if (::write(STDOUT_FILENO, kQuery.data(), kQuery.size()) !=
      static_cast<ssize_t>(kQuery.size()))
    return std::string(kFallbackInputAreaBackground);

  // probe_terminal_capability only reports whether a reply matched, not the
  // reply's own content -- capture it as a side effect of the matcher
  // closure it invokes, since core::match_osc_color_reply itself stays a
  // pure length-only matcher (parallel to match_dec_private_reply, reusable
  // on its own).
  std::string captured_reply;
  auto result = core::probe_terminal_capability(
      STDIN_FILENO,
      [&](std::string_view buf, std::size_t i) {
        const auto len = core::match_osc_color_reply(buf, i);
        if (len > 0)
          captured_reply = std::string(buf.substr(i, len));
        return len;
      },
      kOsc11ProbeTimeout);

  // Same replay contract as the Kitty probe: whatever wasn't consumed by
  // either reply must not be lost.
  g_pending_stdin_bytes.insert(g_pending_stdin_bytes.end(),
                               result.leftover.begin(), result.leftover.end());

  int r = 0;
  int g = 0;
  int b = 0;
  if (!result.supported || !parse_osc_color_reply(captured_reply, r, g, b))
    return std::string(kFallbackInputAreaBackground);
  return blend_input_area_tint(r, g, b);
}

// Cached process-wide, same pattern as kitty_keyboard_enabled() above --
// computed once on the first call and simply read back on every later one,
// so raw mode being entered on every readline() call never repeats the
// query/DA1 round trip. Must only be called once raw mode is already
// active on stdin, same precondition as the Kitty probe.
std::string_view input_area_background() {
  static const std::string background = compute_input_area_background();
  return background;
}

// Out-of-line: TerminalRawMode is declared in readline.h (pi::cli, external
// linkage) since main.cpp needs to own one across its whole interactive
// session; everything it calls here (kitty_keyboard_enabled,
// input_area_background) stays in this file's anonymous namespace.

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
// glyphs actually written, not the rest of the row. input_area_background()
// (above) returns either a truecolor tint blended against the terminal's
// actual queried background, or the fixed SGR 100 (bright-black background)
// fallback -- either way it renders as a distinguishable mid-gray-ish box
// under most terminal color themes, dark and light alike.

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
    std::cout << input_area_background();
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

    // Footer hint row: mirrors status_line_'s existing row-above pattern,
    // but after the composer instead of before it. Only painted when the
    // composer isn't already using its full core::kMaxComposerRows budget --
    // both so it never grows the box past that cap, and so it never needs
    // region_renderer.cpp's fixed bottom-row reservation (sized to exactly
    // kMaxComposerRows) to grow to match; a composer already at the cap
    // simply doesn't get a footer this redraw.
    const bool show_footer = composer_rows_shown < core::kMaxComposerRows;
    if (show_footer) {
      // Physical cursor is currently parked at the composer's own bottom
      // row. Return it there after painting one extra row below via
      // explicit relative motion ("\033[1A" + '\r' + a column-restoring
      // CUF), NOT DECSC/DECRC ("\0337"/"\0338") -- unlike relative motion,
      // DECSC/DECRC save/restore an ABSOLUTE (row, column) screen
      // coordinate. In plain (non-region) mode there's nothing constraining
      // where the composer's bottom row ends up: if it lands on the
      // terminal's own last row, this row's "\r\n" has nowhere to go and
      // scrolls the *entire screen* up by one line -- DECRC would then
      // restore to a coordinate that, after the scroll, shows different
      // (shifted) content than before: the footer's own row, not the
      // composer's. The cursor-motion math just below (which measures
      // purely from the composer's own bottom row, oblivious to any scroll)
      // would then desync from the real cursor, and so would the next
      // redraw's clear_previous() erase, which trusts that math --
      // producing exactly the "doubled footer" symptom this was reported
      // as. Relative motion has no such failure mode: moving up exactly one
      // row from wherever painting the footer left the cursor always lands
      // back on the row that -- scroll or no scroll -- now holds what was
      // on the starting row, because a scroll (if one happened) shifted
      // everything uniformly, "one row up from here" included. The column
      // restore mirrors what DECRC used to do for callers that redraw with
      // show_cursor=false, where nothing further repositions the cursor
      // after this block. The explicit re-application of the tint doesn't
      // rely on DECRC having restored SGR state (not every terminal does
      // that anyway) -- it's reapplied unconditionally instead.
      static constexpr std::string_view kFooterHint =
          "Alt+Enter: newline · Ctrl+C: cancel";
      std::cout << "\r\n\033[0m\033[2m"
                << core::truncate_ansi_line(kFooterHint,
                                            static_cast<int>(columns))
                << "\033[0m\033[K";
      std::cout << "\033[1A\r";
      // Guard against ever emitting CUF with n >= columns, same as the
      // cursor-in-row guard just below for cursor_position.column: CSI n C
      // clamps at the terminal's rightmost cell regardless, but more
      // importantly `column` itself can equal `columns` here (the paint
      // loop's own bookkeeping convention for "this row is exactly full")
      // in the show_cursor=false path, which skips the normalization a few
      // lines up that would otherwise reset it to 0 on a new row. With
      // DECAWM off (the "\033[?7l" active for this whole redraw), the real
      // terminal cursor can never actually reach column `columns` -- it's
      // already clamped at `columns - 1` from painting the row's last cell,
      // so there's nothing to restore in that case.
      if (column > 0 && column < columns)
        std::cout << "\033[" << column << 'C';
      std::cout << input_area_background();
    }

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
    // The footer, when shown, is one more row on screen than rows_relative
    // accounts for -- fold it into rendered_rows_ here (not into
    // rows_relative itself) so clear_previous()'s erase loop covers it too,
    // without disturbing the cursor-motion math above, which intentionally
    // still measures from the composer's own bottom row.
    rendered_rows_ = rows_relative + (show_footer ? 1 : 0);
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

} // namespace

// --- Shared editing primitives -------------------------------------------
//
// Line-boundary, word-boundary, and logical-row primitives used by the M3
// key bindings below (Home/End/Ctrl+A/Ctrl+E, word-left/right, Ctrl+W/U/K/Y,
// Up/Down) and by M5's vim_mode.cpp (declared in readline.h, real linkage
// rather than the anonymous-namespace internal linkage used by the rest of
// this file's helpers, specifically so VimEngine can reuse them directly
// instead of re-deriving the same logic).

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

namespace {

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

  auto poll_result = poll_stdin_retry(descriptors.data(), descriptor_count, 25);
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
  if (read_stdin_byte(c) <= 0)
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
    poll_result = poll_stdin_retry(descriptors.data(), descriptor_count, 25);
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
    if (read_stdin_byte(c) <= 0)
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
    if (poll_stdin_retry(descriptors.data(), descriptor_count, -1) <= 0)
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
    const auto n = read_stdin_byte(c);
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      return result; // EOF/error mid-paste: stop with whatever was read.
    }
    result.content += static_cast<char>(c);
  }
}

// Parses a Kitty "CSI u" key-report body -- what read_escape_sequence
// returns after the leading ESC once the "disambiguate escape codes" flag
// is active, e.g. "[13u" or "[13;2u" for Enter/Shift+Enter -- into its
// codepoint and modifier fields. Only the shape this milestone needs is
// recognized: "[<codepoint>[;<modifiers>]u"; a missing modifier section
// defaults to 1 (no modifiers held), matching the protocol allowing it to
// be omitted entirely when no modifiers are held. Returns false for
// anything that doesn't have exactly this shape -- a different escape
// sequence altogether, or a CSI u report with fields (an event-type suffix,
// alternate-key encoding, associated text) this milestone doesn't decode.
bool parse_csi_u_key(std::string_view seq, std::uint32_t &codepoint,
                     int &modifiers) {
  if (seq.size() < 3 || seq.front() != '[' || seq.back() != 'u')
    return false;
  std::size_t pos = 1;
  const auto read_number = [&](long &out) {
    const auto start = pos;
    long value = 0;
    while (pos < seq.size() && seq[pos] >= '0' && seq[pos] <= '9') {
      value = (value * 10) + (seq[pos] - '0');
      ++pos;
    }
    if (pos == start)
      return false;
    out = value;
    return true;
  };
  long cp = 0;
  if (!read_number(cp))
    return false;
  long mod = 1;
  if (pos < seq.size() && seq[pos] == ';') {
    ++pos;
    if (!read_number(mod))
      return false;
  }
  if (pos + 1 != seq.size()) // trailing content before 'u' this parse skips
    return false;
  codepoint = static_cast<std::uint32_t>(cp);
  modifiers = static_cast<int>(mod);
  return true;
}

enum class CsiUEnterKind { none, plain, shift };

// Classifies a CSI-u escape body (see parse_csi_u_key above) as an Enter
// key report, if it is one -- the only key this milestone needs to decode
// from the new encoding.
CsiUEnterKind classify_csi_u_enter(std::string_view seq) {
  std::uint32_t codepoint = 0;
  int modifiers = 1;
  constexpr std::uint32_t kEnterCodepoint = 13;
  if (!parse_csi_u_key(seq, codepoint, modifiers) ||
      codepoint != kEnterCodepoint)
    return CsiUEnterKind::none;
  // Kitty's modifier encoding is 1 + a bitmask (Shift is bit 0, value 1);
  // the field is 1-biased so "no modifiers held" is representable as a
  // plain 1 rather than 0. Shift held -- alone or combined with any other
  // modifier -- is therefore any value >= 2 with that bit set once the bias
  // is removed.
  const bool shift = modifiers >= 2 && (((modifiers - 1) & 0x1) != 0);
  return shift ? CsiUEnterKind::shift : CsiUEnterKind::plain;
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

TerminalRawMode::~TerminalRawMode() { leave(); }

bool TerminalRawMode::enter(int fdesc) {
  if (isatty(fdesc) == 0)
    return false;
  if (tcgetattr(fdesc, &saved_) != 0)
    return false;
  struct termios raw = saved_;
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
  fd_ = fdesc;
  active_ = true;
  // Bracketed paste: the terminal wraps pasted content in ESC[200~ /
  // ESC[201~ markers so it can be read as one block instead of being
  // indistinguishable from typed keystrokes.
  std::cout << "\033[?2004h" << std::flush;
  // Kitty keyboard protocol: push just the "disambiguate escape codes"
  // flag (value 1) so Enter and Shift+Enter arrive as distinguishable
  // "CSI u" key reports instead of the same '\r' byte -- see
  // kitty_keyboard_enabled() above for the once-per-process probe this is
  // gated on, and classify_csi_u_enter() below for the decode side.
  // Popped in leave() below, which runs on every exit path (a normal
  // return and ~TerminalRawMode() alike) -- matched to exactly the same
  // signal-safety level as the rest of this class's state: ISIG is left
  // enabled (see this class's comment in readline.h), so an abnormal
  // Ctrl+C exit leaves the termios/bracketed-paste/kitty-flag state
  // exactly as unrestored as each other, no better and no worse.
  if (kitty_keyboard_enabled()) {
    std::cout << "\033[>1u" << std::flush;
    kitty_pushed_ = true;
  }
  // Background tint: query the terminal's actual background via OSC 11
  // (once per process -- see input_area_background() above) so redraw()
  // can blend a tint against it instead of always using the fixed
  // fallback. Same precondition as the Kitty probe above: must run once
  // raw mode is confirmed active, so the reply can be read back cleanly.
  (void)input_area_background();
  return true;
}

void TerminalRawMode::leave() {
  if (active_) {
    if (kitty_pushed_) {
      std::cout << "\033[<u" << std::flush;
      kitty_pushed_ = false;
    }
    std::cout << "\033[?2004l" << std::flush;
    tcsetattr(fd_, TCSAFLUSH, &saved_);
    active_ = false;
  }
}

ReadlineResult readline(std::string_view prompt, const CompleteFn &complete_fn,
                        const ControlFn &control_fn,
                        std::string_view status_line,
                        std::string_view initial_draft,
                        std::size_t initial_cursor, int wake_fd,
                        bool clear_on_submit,
                        const std::function<void()> &on_resize, bool vim_mode,
                        TerminalRawMode *external_raw_mode) {
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

  // external_raw_mode's caller owns entering/leaving it across a whole
  // interactive session (see TerminalRawMode's comment in readline.h); this
  // call must not tear it down on the way out. Otherwise, scope a local
  // instance to exactly this call, as before.
  std::optional<TerminalRawMode> local_raw;
  TerminalRawMode *raw = external_raw_mode;
  if (raw == nullptr) {
    local_raw.emplace();
    raw = &*local_raw;
  }
  if (!raw->active() && !raw->enter(STDIN_FILENO)) {
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

  // Single most-recent-kill slot for Ctrl+W/Ctrl+U/Ctrl+K/Ctrl+Y -- also
  // reused as vim mode's implicit unnamed register for d/c below, so a
  // vim-mode deletion can be yanked back with Ctrl+Y and vice versa. Scoped
  // to this call, matching M1's decision not to introduce a ReadlineState
  // that persists across readline() calls -- nothing needs the kill buffer
  // to outlive one prompt. Each kill overwrites it (last-kill-wins); this is
  // not a ring of multiple kills.
  std::string kill_buffer;

  // Vim mode's Normal/Insert state machine (see cli/vim_mode.h). Always
  // constructed -- cheap and inert when vim_mode is false or its mode stays
  // Insert -- so the dispatch loop below has one object to check rather
  // than conditionally allocating it. Starts in Insert mode, matching plain
  // (non-vim) editing exactly until the user deliberately presses Escape.
  VimEngine vim_engine;

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
      if (external_raw_mode == nullptr)
        raw->leave();
      std::cout << std::flush;
    } else {
      renderer.redraw(buf, cursor, false);
      if (external_raw_mode == nullptr)
        raw->leave();
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
    if (!g_pending_stdin_bytes.empty()) {
      // Replay bytes queued by the Kitty-keyboard-protocol startup probe
      // (see kitty_keyboard_enabled) ahead of any real poll/read: report
      // stdin as already readable instead of blocking, so queued type-ahead
      // is processed immediately rather than waiting on a real event.
      poll_result = 1;
      descriptors[0].revents = POLLIN;
      if (descriptor_count > 1)
        descriptors[1].revents = 0;
    } else {
      while (true) {
        poll_result = ::poll(descriptors.data(), descriptor_count, -1);
        if (poll_result >= 0)
          break;
        if (errno != EINTR)
          break;
        // A signal (SIGWINCH among others) interrupted the blocking poll.
        // Notice a genuine resize here rather than silently retrying, since
        // nothing else observes it while this thread sits idle in
        // readline().
        const auto generation = core::resize_generation();
        if (generation != last_resize_generation) {
          last_resize_generation = generation;
          if (on_resize)
            on_resize();
          renderer.reanchor();
          renderer.redraw(buf, cursor);
        }
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
      const auto n = read_stdin_byte(c);
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

      if (c == '\r')
        return finish(ReadlineExit::submitted);

      if (c == '\n') {
        // Bare LF (Ctrl+J): many terminals translate Shift+Enter into a
        // raw '\n' byte at the terminal level, entirely independent of the
        // Kitty keyboard protocol or tmux's extended-keys forwarding --
        // Codex's own keymap treats Ctrl+J the same way, as an
        // always-available newline-insert binding regardless of whether
        // protocol negotiation succeeded. ICRNL is already cleared
        // (RawMode::enter), so '\r' (Enter) and '\n' (Ctrl+J / a
        // terminal-translated Shift+Enter) reliably arrive as distinct
        // bytes here.
        buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(cursor), '\n');
        ++cursor;
        renderer.redraw(buf, cursor);
        continue;
      }

      if (c == '\x04') // Ctrl+D — EOF
        return finish(ReadlineExit::eof);

      // Vim mode's Normal-mode key layer intercepts single, non-escape
      // bytes ahead of the plain-editor dispatch chain below -- see
      // cli/vim_mode.h. Submit ('\r'/'\n', above) and EOF (Ctrl+D, above)
      // stay live in every mode, since they're readline()-level controls
      // rather than buffer edits (a Normal mode with no way to submit would
      // trap the user into switching back to Insert for every message).
      // Escape sequences (arrows, Ctrl+arrows, mouse wheel, Alt+B/F,
      // bracketed paste, Alt+Enter, the Kitty protocol) are handled below,
      // in the '\x1b' branch, exactly the same in both modes -- vim mode's
      // covered keys are all single ASCII bytes, so there's no overlap to
      // arbitrate, and scroll/paste/newline-insert have no vim equivalent
      // in this milestone's cut-down scope. Any byte the Normal-mode state
      // machine doesn't recognize (Backspace, Tab, Ctrl+A/E/W/U/K/Y, digits,
      // punctuation, ...) is a harmless no-op -- see VimEngine::
      // handle_normal_key's default case.
      if (vim_mode && vim_engine.mode() == VimMode::Normal && c != '\x1b') {
        const auto vim_result =
            vim_engine.handle_normal_key(c, buf, cursor, kill_buffer);
        if (vim_result.changed)
          renderer.redraw(buf, cursor);
        continue;
      }

      if (c == '\x1b') {
        const auto escape = read_escape_sequence(wake_fd);
        if (escape.wake)
          return finish(ReadlineExit::mailbox_wake);
        if (vim_mode) {
          // Escape always cancels an in-progress d/c operator, matching
          // real Vim -- regardless of whether this turns out to be a bare
          // Escape or the start of some other escape sequence (a fast
          // typist landing on an arrow key right after 'd', say).
          vim_engine.cancel_pending();
          if (escape.sequence.empty()) {
            // Bare Escape (read_escape_sequence's short poll window timed
            // out with nothing following): enter Normal mode. Idempotent
            // if already there. No buffer/cursor change, so no redraw.
            vim_engine.set_mode(VimMode::Normal);
            continue;
          }
        }
        if (escape.sequence == "\r") {
          // Alt+Enter (legacy ESC + \r, distinguishable now that ICRNL is
          // cleared): insert a newline instead of submitting.
          buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(cursor), '\n');
          ++cursor;
          renderer.redraw(buf, cursor);
        } else if (const auto enter_kind =
                       classify_csi_u_enter(escape.sequence);
                   enter_kind != CsiUEnterKind::none) {
          // Enter via the Kitty keyboard protocol's "CSI u" key-report
          // encoding (active once RawMode::enter has confirmed support and
          // pushed the "disambiguate escape codes" flag): codepoint 13 with
          // the Shift modifier bit set means Shift+Enter, handled exactly
          // like Alt+Enter above; without it -- or with no modifier section
          // at all, which the protocol allows omitting when nothing is
          // held -- it's plain Enter and submits exactly like a raw '\r'
          // does.
          if (enter_kind == CsiUEnterKind::shift) {
            buf.insert(buf.begin() + static_cast<std::ptrdiff_t>(cursor), '\n');
            ++cursor;
            renderer.redraw(buf, cursor);
          } else {
            return finish(ReadlineExit::submitted);
          }
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
