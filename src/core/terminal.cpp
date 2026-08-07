#include "core/terminal.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <mutex>
#include <ranges> // NOLINT(misc-include-cleaner)
#include <string>
#include <string_view>
#include <sys/ioctl.h>
#include <thread>
#include <unistd.h>

namespace pi::core {

int term_width(int fd) {
  struct winsize ws {}; // NOLINT(misc-include-cleaner)
  // NOLINTNEXTLINE(misc-include-cleaner)
  if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return static_cast<int>(ws.ws_col);
  return 80;
}

int term_height(int fd) {
  struct winsize ws {}; // NOLINT(misc-include-cleaner)
  // NOLINTNEXTLINE(misc-include-cleaner)
  if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
    return static_cast<int>(ws.ws_row);
  return 24;
}

namespace {

char32_t decode_utf8(std::string_view s, std::size_t i) {
  const auto c = static_cast<unsigned char>(s[i]);
  if ((c & 0x80) == 0x00)
    return c;
  if ((c & 0xE0) == 0xC0) {
    char32_t cp = c & 0x1F;
    if (i + 1 < s.size())
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    return cp;
  }
  if ((c & 0xF0) == 0xE0) {
    char32_t cp = c & 0x0F;
    if (i + 1 < s.size())
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    if (i + 2 < s.size())
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    return cp;
  }
  if ((c & 0xF8) == 0xF0) {
    char32_t cp = c & 0x07;
    if (i + 1 < s.size())
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    if (i + 2 < s.size())
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    if (i + 3 < s.size())
      cp = (cp << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    return cp;
  }
  return 0xFFFD;
}

// Ranges from Unicode Standard Annex #11 (Wide + Fullwidth categories).
// Covers the characters most commonly produced by LLM responses.

bool is_wide(char32_t cp) {
  struct Range {
    char32_t lo, hi;
  };
  static constexpr std::array<Range, 24> kWide = {{
      {.lo = 0x1100, .hi = 0x115F}, // Hangul Jamo
      {.lo = 0x2329, .hi = 0x232A}, // Angle brackets (wide)
      {.lo = 0x2E80,
       .hi = 0x303E}, // CJK Radicals, Kangxi, Bopomofo, Hiragana preamble
      {.lo = 0x3041,
       .hi = 0x33FF}, // Hiragana, Katakana, Bopomofo, Hangul compat, CJK compat
      {.lo = 0x3400, .hi = 0x4DBF}, // CJK Extension A
      {.lo = 0x4E00, .hi = 0x9FFF}, // CJK Unified Ideographs (the main block)
      {.lo = 0xA000, .hi = 0xA4CF}, // Yi Syllables + Radicals
      {.lo = 0xA960, .hi = 0xA97F}, // Hangul Jamo Extended-A
      {.lo = 0xAC00, .hi = 0xD7AF}, // Hangul Syllables
      {.lo = 0xF900, .hi = 0xFAFF}, // CJK Compatibility Ideographs
      {.lo = 0xFE10, .hi = 0xFE1F}, // Vertical Forms
      {.lo = 0xFE30, .hi = 0xFE6F}, // CJK Compatibility Forms + Small Forms
      {.lo = 0xFF01,
       .hi = 0xFF60}, // Fullwidth ASCII + Halfwidth Katakana (fullwidth half)
      {.lo = 0xFFE0, .hi = 0xFFE6},   // Fullwidth currency signs
      {.lo = 0x1B000, .hi = 0x1B0FF}, // Kana Supplement
      {.lo = 0x1F200, .hi = 0x1F2FF}, // Enclosed Ideographic Supplement
      {.lo = 0x1F300, .hi = 0x1F64F}, // Misc Symbols, Dingbats, Emoticons
      {.lo = 0x1F900, .hi = 0x1F9FF}, // Supplemental Symbols and Pictographs
      {.lo = 0x1FA00, .hi = 0x1FAFF}, // Chess / Extended Pictographs
      {.lo = 0x20000, .hi = 0x2A6DF}, // CJK Extension B
      {.lo = 0x2A700, .hi = 0x2CEAF}, // CJK Extension C, D, E
      {.lo = 0x2CEB0, .hi = 0x2EBEF}, // CJK Extension F
      {.lo = 0x2F800, .hi = 0x2FA1F}, // CJK Compatibility Ideographs Supplement
      {.lo = 0x30000, .hi = 0x3134F}, // CJK Extension G
  }};
  return std::ranges::any_of(kWide, [cp](const auto &range) {
    return cp >= range.lo && cp <= range.hi;
  });
}

// Combining/zero-width characters — contribute 0 display columns.
bool is_combining(char32_t cp) {
  struct Range {
    char32_t lo, hi;
  };
  static constexpr std::array<Range, 8> kCombining = {{
      {.lo = 0x0300, .hi = 0x036F}, // Combining Diacritical Marks
      {.lo = 0x0483, .hi = 0x0489},
      {.lo = 0x0591, .hi = 0x05BD},   // Hebrew cantillation
      {.lo = 0x064B, .hi = 0x065F},   // Arabic combining
      {.lo = 0x1DC0, .hi = 0x1DFF},   // Combining Diacritical Marks Supplement
      {.lo = 0x20D0, .hi = 0x20FF},   // Combining Diacritical Marks for Symbols
      {.lo = 0xFE20, .hi = 0xFE2F},   // Combining Half Marks
      {.lo = 0xE0100, .hi = 0xE01EF}, // Variation Selectors Supplement
  }};
  return std::ranges::any_of(kCombining, [cp](const auto &range) {
    return cp >= range.lo && cp <= range.hi;
  });
}

} // namespace

std::size_t skip_ansi_sequence(std::string_view s, std::size_t i) {
  if (i >= s.size() || s[i] != '\033')
    return i;
  if (i + 1 >= s.size())
    return i;

  const char next = s[i + 1];

  // CSI: \033[ param* inter* final
  if (next == '[') {
    i += 2;
    while (i < s.size()) {
      const auto c = static_cast<unsigned char>(s[i]);
      if (c >= 0x40 && c <= 0x7E)
        return i + 1; // final byte
      ++i;
    }
    return i; // truncated — consumed what we can
  }

  // OSC: \033] ... BEL  or  \033] ... ST (\033\\)
  if (next == ']') {
    i += 2;
    while (i < s.size()) {
      if (s[i] == '\007')
        return i + 1;
      if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '\\')
        return i + 2;
      ++i;
    }
    return i;
  }

  // DCS / PM / APC: \033[P^_] ... ST
  if (next == 'P' || next == '^' || next == '_') {
    i += 2;
    while (i < s.size()) {
      if (s[i] == '\033' && i + 1 < s.size() && s[i + 1] == '\\')
        return i + 2;
      ++i;
    }
    return i;
  }

  // Two-byte Fe sequences: \033 + single byte in 0x40-0x5F
  // (excludes [, ], P, ^, _ which are multi-char introducers handled above)
  const auto nb = static_cast<unsigned char>(next);
  if (nb >= 0x40 && nb <= 0x5F)
    return i + 2;

  return i; // unrecognised — don't skip
}

std::size_t advance_utf8(std::string_view s, std::size_t i) {
  if (i >= s.size())
    return i;
  const auto c = static_cast<unsigned char>(s[i]);
  if ((c & 0x80) == 0x00)
    return i + 1;
  if ((c & 0xE0) == 0xC0)
    return std::min(i + 2, s.size());
  if ((c & 0xF0) == 0xE0)
    return std::min(i + 3, s.size());
  if ((c & 0xF8) == 0xF0)
    return std::min(i + 4, s.size());
  return i + 1; // invalid lead byte — step one byte
}

int codepoint_width(std::string_view s, std::size_t i) {
  if (i >= s.size())
    return 0;
  const char32_t cp = decode_utf8(s, i);
  // Non-printing C0/C1 controls and DEL
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
    return 0;
  if (is_combining(cp))
    return 0;
  if (is_wide(cp))
    return 2;
  return 1;
}

int display_columns(std::string_view line) {
  int columns = 0;
  for (std::size_t i = 0; i < line.size();) {
    if (line[i] == '\n' || line[i] == '\r')
      break;
    if (line[i] == '\033') {
      const auto next = skip_ansi_sequence(line, i);
      if (next > i) {
        i = next;
        continue;
      }
    }
    columns += codepoint_width(line, i);
    i = advance_utf8(line, i);
  }
  return columns;
}

std::string truncate_ansi_line(std::string_view line, int width) {
  if (width <= 0)
    return {};

  std::string result;
  int columns = 0;
  bool truncated = false;
  bool saw_escape = false;
  for (std::size_t i = 0; i < line.size();) {
    if (line[i] == '\n' || line[i] == '\r') {
      truncated = true;
      break;
    }
    if (line[i] == '\033') {
      const auto next = skip_ansi_sequence(line, i);
      if (next > i) {
        result.append(line.substr(i, next - i));
        saw_escape = true;
        i = next;
        continue;
      }
    }

    const auto next = advance_utf8(line, i);
    const int columns_for_char = codepoint_width(line, i);
    if (columns_for_char > 0 && columns + columns_for_char > width) {
      truncated = true;
      break;
    }
    result.append(line.substr(i, next - i));
    columns += columns_for_char;
    i = next;
  }

  if (truncated && saw_escape)
    result += "\033[0m";
  return result;
}

const std::array<std::string_view, 10> kTerminalTitleSpinnerFrames = {
    "\u280B", "\u2819", "\u2839", "\u2838", "\u283C",
    "\u2834", "\u2826", "\u2827", "\u2807", "\u280F"};

namespace {

bool is_terminal_title_whitespace(char32_t cp) {
  return cp == 0x20 || cp == 0x09 || cp == 0x0A || cp == 0x0B || cp == 0x0C ||
         cp == 0x0D || cp == 0x85 || cp == 0xA0 || cp == 0x1680 ||
         (cp >= 0x2000 && cp <= 0x200A) || cp == 0x2028 || cp == 0x2029 ||
         cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

bool is_disallowed_terminal_title_char(char32_t cp) {
  if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
    return true;
  switch (cp) {
  case 0x00AD:
  case 0x034F:
  case 0x061C:
  case 0x180E:
  case 0xFEFF:
    return true;
  default:
    break;
  }
  if (cp >= 0x200B && cp <= 0x200F)
    return true;
  if (cp >= 0x202A && cp <= 0x202E)
    return true;
  if (cp >= 0x2060 && cp <= 0x206F)
    return true;
  if (cp >= 0xFE00 && cp <= 0xFE0F)
    return true;
  if (cp >= 0xFFF9 && cp <= 0xFFFB)
    return true;
  if (cp >= 0x1BCA0 && cp <= 0x1BCA3)
    return true;
  if (cp >= 0xE0100 && cp <= 0xE01EF)
    return true;
  return false;
}

} // namespace

std::string sanitize_terminal_title(std::string_view title) {
  std::string sanitized;
  sanitized.reserve(title.size());
  std::size_t chars_written = 0;
  bool pending_space = false;
  for (std::size_t i = 0; i < title.size();) {
    const auto lead = static_cast<unsigned char>(title[i]);
    const std::size_t next = advance_utf8(title, i);
    // advance_utf8 steps a single byte both for plain ASCII (lead < 0x80)
    // and for an orphan/invalid lead byte with no recognized multi-byte
    // pattern (lead >= 0x80). decode_utf8 maps the latter to the U+FFFD
    // sentinel, but U+FFFD isn't itself disallowed, so without this check
    // the raw invalid byte gets copied straight into the "sanitized"
    // output below instead of being dropped.
    if (next == i + 1 && lead >= 0x80) {
      i = next;
      continue;
    }
    const char32_t cp = decode_utf8(title, i);
    if (is_terminal_title_whitespace(cp)) {
      pending_space = !sanitized.empty();
      i = next;
      continue;
    }
    if (is_disallowed_terminal_title_char(cp)) {
      // C0/C1 control characters act as word separators, like whitespace
      // (e.g. ESC/BEL between two words should collapse to one space, not
      // glue the words together). Invisible Unicode format characters
      // (zero-width joiners, bidi overrides, BOM, ...) are the opposite:
      // they occur mid-word and must be removed without introducing a
      // space.
      if (cp < 0x20 || (cp >= 0x7F && cp < 0xA0))
        pending_space = !sanitized.empty();
      i = next;
      continue;
    }
    if (pending_space) {
      const std::size_t remaining = kMaxTerminalTitleChars > chars_written
                                        ? kMaxTerminalTitleChars - chars_written
                                        : 0;
      if (remaining > 1) {
        sanitized.push_back(' ');
        ++chars_written;
        pending_space = false;
      }
    }
    if (chars_written >= kMaxTerminalTitleChars)
      break;
    sanitized.append(title.substr(i, next - i));
    ++chars_written;
    i = next;
  }
  return sanitized;
}

std::string format_active_terminal_title(std::string_view base_title,
                                         std::string_view frame) {
  if (base_title.empty())
    return std::string(frame);
  std::string out;
  out.reserve(frame.size() + 1 + base_title.size());
  out.append(frame);
  out.push_back(' ');
  out.append(base_title);
  return out;
}

std::string terminal_title_sequence(std::string_view sanitized_title) {
  std::string seq;
  seq.reserve(4 + sanitized_title.size() + 1);
  seq.append("\033]0;");
  seq.append(sanitized_title);
  seq.push_back('\007');
  return seq;
}

std::string terminal_project_label(const std::filesystem::path &cwd) {
  std::error_code ec;
  std::filesystem::path cur = cwd;
  if (cur.empty())
    return "pici";
  std::filesystem::path search = cur;
  while (true) {
    const auto git_path = search / ".git";
    if (std::filesystem::exists(git_path, ec)) {
      auto name = search.filename().string();
      if (!name.empty())
        return name;
      return "pici";
    }
    auto parent = search.parent_path();
    if (parent.empty() || parent == search)
      break;
    search = parent;
  }
  auto name = cur.filename().string();
  if (!name.empty())
    return name;
  if (!cur.empty()) {
    auto parent = cur.parent_path();
    if (!parent.empty()) {
      auto fallback = parent.filename().string();
      if (!fallback.empty())
        return fallback;
    }
  }
  return "pici";
}

TerminalTitleResult set_terminal_title(int fd, std::string_view title) {
  if (isatty(fd) == 0)
    return TerminalTitleResult::Skipped;
  const std::string sanitized = sanitize_terminal_title(title);
  const std::string seq = terminal_title_sequence(sanitized);
  const ssize_t n = ::write(fd, seq.data(), seq.size());
  if (n < 0 || static_cast<std::size_t>(n) != seq.size())
    return TerminalTitleResult::Skipped;
  return TerminalTitleResult::Applied;
}

TerminalTitleResult clear_terminal_title(int fd) {
  if (isatty(fd) == 0)
    return TerminalTitleResult::Skipped;
  const std::string seq = terminal_title_sequence("");
  const ssize_t n = ::write(fd, seq.data(), seq.size());
  if (n < 0 || static_cast<std::size_t>(n) != seq.size())
    return TerminalTitleResult::Skipped;
  return TerminalTitleResult::Applied;
}

TerminalTitleController::TerminalTitleController(int fd,
                                                 std::string initial_base_title)
    : fd_(fd), interval_(kTerminalTitleSpinnerInterval) {
  is_tty_ = (isatty(fd_) != 0);
  if (!is_tty_) {
    base_title_ = sanitize_terminal_title(initial_base_title);
    return;
  }
  writer_ = [this](std::string_view sanitized) -> TerminalTitleResult {
    const std::string seq = terminal_title_sequence(sanitized);
    const ssize_t n = ::write(fd_, seq.data(), seq.size());
    if (n < 0 || static_cast<std::size_t>(n) != seq.size())
      return TerminalTitleResult::Skipped;
    return TerminalTitleResult::Applied;
  };
  base_title_ = sanitize_terminal_title(initial_base_title);
  emit_sanitized(base_title_);
}

TerminalTitleController::TerminalTitleController(
    int fd, std::string initial_base_title, Writer writer,
    std::chrono::milliseconds interval)
    : fd_(fd), writer_(std::move(writer)), interval_(interval) {
  if (writer_)
    is_tty_ = true;
  else
    is_tty_ = (isatty(fd_) != 0);
  base_title_ = sanitize_terminal_title(initial_base_title);
  if (is_tty_)
    emit_sanitized(base_title_);
}

TerminalTitleController::TerminalTitleController(
    int fd, std::string initial_base_title, Writer writer,
    std::chrono::milliseconds interval, bool is_tty)
    : fd_(fd), writer_(std::move(writer)), interval_(interval),
      is_tty_(is_tty) {
  base_title_ = sanitize_terminal_title(initial_base_title);
  if (is_tty_)
    emit_sanitized(base_title_);
}

TerminalTitleController::~TerminalTitleController() noexcept {
  try {
    stop_activity();
    if (has_applied_ && is_tty_) {
      const std::string seq = terminal_title_sequence("");
      if (writer_) {
        writer_("");
      } else if (isatty(fd_) != 0) {
        ::write(fd_, seq.data(), seq.size());
      }
      has_applied_ = false;
      last_emitted_.clear();
    }
  } catch (...) {
  }
}

void TerminalTitleController::set_base_title(std::string title) {
  const std::string sanitized = sanitize_terminal_title(title);
  bool should_emit = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sanitized == base_title_ && !active_) {
      return;
    }
    base_title_ = sanitized;
    should_emit = !active_;
  }
  if (should_emit)
    emit_sanitized(sanitized);
}

void TerminalTitleController::start_activity() {
  std::string base_copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ || !is_tty_)
      return;
    active_ = true;
    frame_index_ = 0;
    base_copy = base_title_;
  }
  const std::string active_title =
      format_active_terminal_title(base_copy, kTerminalTitleSpinnerFrames[0]);
  const std::string sanitized = sanitize_terminal_title(active_title);
  emit_sanitized(sanitized);
  worker_ = std::jthread([this](std::stop_token st) { worker_loop(st); });
}

void TerminalTitleController::stop_activity() {
  bool was_active = false;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    was_active = active_;
    if (!was_active)
      return;
  }
  if (worker_.joinable()) {
    worker_.request_stop();
    cv_.notify_all();
    worker_.join();
  }
  std::string base_copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_ = false;
    base_copy = base_title_;
  }
  emit_sanitized(base_copy);
}

TerminalTitleResult
TerminalTitleController::write_title(std::string_view sanitized) {
  if (!is_tty_)
    return TerminalTitleResult::Skipped;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (sanitized == last_emitted_ && has_applied_)
      return TerminalTitleResult::Applied;
  }
  TerminalTitleResult res = TerminalTitleResult::Skipped;
  if (writer_) {
    res = writer_(sanitized);
  } else {
    if (isatty(fd_) == 0)
      return TerminalTitleResult::Skipped;
    const std::string seq = terminal_title_sequence(sanitized);
    const ssize_t n = ::write(fd_, seq.data(), seq.size());
    res = (n >= 0 && static_cast<std::size_t>(n) == seq.size())
              ? TerminalTitleResult::Applied
              : TerminalTitleResult::Skipped;
  }
  if (res == TerminalTitleResult::Applied) {
    std::lock_guard<std::mutex> lock(mutex_);
    last_emitted_ = std::string(sanitized);
    has_applied_ = true;
  }
  return res;
}

void TerminalTitleController::emit_sanitized(std::string_view sanitized) {
  (void)write_title(sanitized);
}

void TerminalTitleController::worker_loop(std::stop_token st) {
  while (!st.stop_requested()) {
    {
      std::unique_lock<std::mutex> lk(cv_mutex_);
      cv_.wait_for(lk, st, interval_, [&] { return st.stop_requested(); });
      if (st.stop_requested())
        break;
    }
    std::string base_copy;
    std::size_t idx = 0;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!active_)
        break;
      frame_index_ = (frame_index_ + 1) % kTerminalTitleSpinnerFrames.size();
      idx = frame_index_;
      base_copy = base_title_;
    }
    const std::string active_title = format_active_terminal_title(
        base_copy, kTerminalTitleSpinnerFrames[idx]);
    const std::string sanitized = sanitize_terminal_title(active_title);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (!active_ || st.stop_requested())
        break;
    }
    emit_sanitized(sanitized);
  }
}

TerminalTitleActivityGuard::TerminalTitleActivityGuard(
    TerminalTitleController &controller)
    : controller_(controller) {
  controller_.start_activity();
}

TerminalTitleActivityGuard::~TerminalTitleActivityGuard() noexcept {
  try {
    controller_.stop_activity();
  } catch (...) {
  }
}

int rows_for_line(std::string_view line, int width) {
  if (width <= 0)
    return 1;
  int col = 0;
  int rows = 1;
  for (std::size_t i = 0; i < line.size();) {
    if (line[i] == '\033') {
      const auto next = skip_ansi_sequence(line, i);
      if (next > i) {
        i = next;
        continue;
      }
    }
    const int cw = codepoint_width(line, i);
    i = advance_utf8(line, i);
    col += cw;
    if (col >= width) {
      ++rows;
      col = 0;
    }
  }
  return rows;
}

int cursor_rows_for_rendered(std::string_view rendered, int width) {
  if (width <= 0)
    return 1;
  int rows = 1;
  int col = 0;
  for (std::size_t i = 0; i < rendered.size();) {
    if (rendered[i] == '\033') {
      const auto next = skip_ansi_sequence(rendered, i);
      if (next > i) {
        i = next;
        continue;
      }
    }
    if (rendered[i] == '\n') {
      ++rows;
      col = 0;
      ++i;
      continue;
    }
    const int cw = codepoint_width(rendered, i);
    i = advance_utf8(rendered, i);
    col += cw;
    if (col >= width) {
      ++rows;
      col = 0;
    }
  }
  return rows;
}

void BlockBoundaryScanner::advance(std::string_view s) {
  for (std::size_t i = scan_pos; i < s.size(); ++i) {
    const char c = s[i];

    if (c == '\n') {
      const bool blank = !saw_nonspace;

      // Two consecutive newlines ending a blank line = \n\n boundary candidate.
      // No i+1 < s.size() guard: the boundary IS stable even if nothing follows
      // yet; G2's cache will render an empty suffix
      // (render_visible_markdown("") returns "") harmlessly. The guard would
      // silently drop boundaries across incremental advance() calls.
      if (blank && last_char_nl && !inside_fence && last_nonblank_col0)
        last_stable = i + 1;

      if (!blank) {
        last_nonblank_col0 = (line_indent == 0);

        // Fence open/close detection.
        // Opener: line_indent ≤ 3, run ≥ 3 backticks/tildes.
        //   Info string (e.g. "py" in ```py) is OK — line_only_fence may be
        //   false.
        // Closer: same char, run ≥ opening length, and line_only_fence (no
        //   non-fence, non-space content — trailing spaces are allowed).
        if (line_indent <= 3 && fence_run >= 3) {
          if (!inside_fence) {
            inside_fence = true;
            fence_char = fence_run_char;
            fence_len = fence_run;
          } else if (fence_run_char == fence_char && fence_run >= fence_len &&
                     line_only_fence) {
            inside_fence = false;
            fence_char = 0;
            fence_len = 0;
          }
        }
      }

      last_char_nl = true;
      line_indent = 0;
      saw_nonspace = false;
      fence_run = 0;
      fence_run_char = 0;
      line_only_fence = true;

    } else {
      last_char_nl = false;

      if (!saw_nonspace) {
        if (c == ' ' || c == '\t') {
          ++line_indent;
        } else {
          saw_nonspace = true;
          // Start fence-run tracking only if indent is small enough.
          // Tab is counted as 1 indent unit (conservative; CommonMark uses 4
          // columns for tab, but LLM output rarely has tab-indented fences).
          if (line_indent <= 3 && (c == '`' || c == '~')) {
            fence_run = 1;
            fence_run_char = c;
            line_only_fence = true;
          } else {
            fence_run = 0;
            line_only_fence = false;
          }
        }
      } else if (line_only_fence) {
        if (c == fence_run_char) {
          ++fence_run;
        } else if (c != ' ') {
          // Non-fence, non-space char: this line is not a pure fence line.
          // Spaces after the run are legal on closing fence lines (and harmless
          // on openers), so we keep line_only_fence=true for spaces.
          line_only_fence = false;
        }
      }
    }
  }
  scan_pos = s.size();
}

} // namespace pi::core
