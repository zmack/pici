#include "core/terminal.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <ranges>   // NOLINT(misc-include-cleaner)
#include <signal.h> // NOLINT(modernize-deprecated-headers)
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/ioctl.h>
// NOLINTNEXTLINE(misc-include-cleaner): poll() declarations vary by platform.
#include <sys/poll.h>
#include <sys/types.h>
#include <system_error>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pi::core {

namespace {

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_sigint_pending = 0;

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
volatile std::sig_atomic_t g_resize_generation = 0;

void resize_sig_handler(int /*sig*/) {
  g_resize_generation = static_cast<std::sig_atomic_t>(g_resize_generation + 1);
}

} // namespace

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

AltScreenSession *AltScreenSession::current_ = nullptr;
bool AltScreenSession::atexit_registered_ = false;

AltScreenSession::AltScreenSession(int fd) : fd_(fd) { enter(); }

AltScreenSession::~AltScreenSession() noexcept { leave(); }

void AltScreenSession::enter() {
  if (in_alt_)
    return;

  in_alt_ = true;
  current_ = this;
  if (!atexit_registered_) {
    std::atexit(atexit_fn);
    atexit_registered_ = true;
  }

  struct sigaction sa {};      // NOLINT(misc-include-cleaner)
  sa.sa_handler = sig_handler; // NOLINT(misc-include-cleaner)
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGINT, &sa, previous_actions_.data());
  sigaction(SIGTERM, &sa, previous_actions_.data() + 1);
  sigaction(SIGHUP, &sa,
            previous_actions_.data() + 2); // NOLINT(misc-include-cleaner)
  signals_installed_ = true;

  static constexpr std::string_view kEnter =
      "\033[?1049h"   // enter alternate screen
      "\033[H\033[2J" // home + clear
      "\033[?1000h"   // report mouse button/wheel events
      "\033[?1006h";  // ...using SGR extended coordinate encoding
  ::write(fd_, kEnter.data(), kEnter.size());
}

void AltScreenSession::restore_terminal() noexcept {
  if (!in_alt_)
    return;

  in_alt_ = false;
  static constexpr std::string_view kRestore =
      "\033[?1006l"  // stop SGR mouse coordinate encoding
      "\033[?1000l"  // stop mouse reporting — restores native selection
      "\033[r"       // reset scroll region
      "\033[?25h"    // show cursor (must precede ?1049l)
      "\033[?1049l"; // exit alternate screen
  ::write(fd_, kRestore.data(), kRestore.size());
}

void AltScreenSession::restore_signal_handlers() noexcept {
  if (!signals_installed_)
    return;

  sigaction(SIGINT, previous_actions_.data(), nullptr);
  sigaction(SIGTERM, previous_actions_.data() + 1, nullptr);
  sigaction(SIGHUP, previous_actions_.data() + 2, nullptr);
  signals_installed_ = false;
}

void AltScreenSession::leave() noexcept {
  restore_terminal();
  restore_signal_handlers();
  if (current_ == this)
    current_ = nullptr;
}

void AltScreenSession::atexit_fn() {
  if (current_ != nullptr)
    current_->leave();
}

void AltScreenSession::sig_handler(int sig) {
  if (sig == SIGINT && g_sigint_pending == 0) {
    notify_sigint();
    return;
  }

  if (current_ != nullptr)
    current_->restore_terminal();
  struct sigaction sa {};  // NOLINT(misc-include-cleaner)
  sa.sa_handler = SIG_DFL; // NOLINT(misc-include-cleaner)
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(sig, &sa, nullptr);
  raise(sig);
}

void notify_sigint() noexcept { g_sigint_pending = 1; }

bool consume_sigint() {
  if (g_sigint_pending == 0)
    return false;
  g_sigint_pending = 0;
  return true;
}

void install_resize_handler() {
  static bool installed = false; // Only ever set from the main thread.
  if (installed)
    return;
  installed = true;

  struct sigaction sa {};             // NOLINT(misc-include-cleaner)
  sa.sa_handler = resize_sig_handler; // NOLINT(misc-include-cleaner)
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; // No SA_RESTART: a blocked poll()/select() must observe
                   // EINTR so callers can notice the resize promptly.
  sigaction(SIGWINCH, &sa, nullptr); // NOLINT(misc-include-cleaner)
}

int resize_generation() noexcept {
  return static_cast<int>(g_resize_generation);
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

std::size_t match_dec_private_reply(std::string_view s, std::size_t i,
                                    char final_byte) {
  if (i + 2 >= s.size())
    return 0;
  if (s[i] != '\033' || s[i + 1] != '[' || s[i + 2] != '?')
    return 0;
  std::size_t j = i + 3;
  while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';'))
    ++j;
  if (j >= s.size())
    return 0; // sequence hasn't finished arriving yet
  return s[j] == final_byte ? j - i + 1 : 0;
}

TerminalProbeResult
probe_terminal_capability(int read_fd, const TerminalProbeMatcher &is_reply,
                          std::chrono::milliseconds timeout) {
  TerminalProbeResult result;
  std::string raw;
  const auto deadline = std::chrono::steady_clock::now() + timeout;

  const auto has_complete_da1 = [&] {
    for (std::size_t i = 0; i < raw.size(); ++i) {
      if (match_dec_private_reply(raw, i, 'c') > 0)
        return true;
    }
    return false;
  };

  // Read raw bytes until a complete DA1 reply has been seen (the common
  // case on any real terminal) or the overall deadline passes -- whichever
  // comes first, so a terminal that answers neither query never hangs this
  // call.
  while (!has_complete_da1()) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline)
      break;
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    struct pollfd descriptor {
      .fd = read_fd, .events = POLLIN, .revents = 0
    };
    const auto poll_result =
        ::poll(&descriptor, 1,
               static_cast<int>(
                   std::max<decltype(remaining)::rep>(remaining.count(), 0)));
    if (poll_result < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (poll_result == 0)
      break; // overall timeout
    if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0)
      break;
    std::array<char, 256> chunk{};
    const auto n = ::read(read_fd, chunk.data(), chunk.size());
    if (n <= 0) {
      if (n < 0 && (errno == EINTR || errno == EAGAIN))
        continue;
      break;
    }
    raw.append(chunk.data(), static_cast<std::size_t>(n));
  }

  // Classify every byte read via a single left-to-right scan: the first
  // complete DA1 reply and, independently, the first reply recognized by
  // `is_reply`. Matched spans can't overlap since the scan only advances
  // past a match once one is found at the current position.
  std::size_t da1_start = std::string::npos;
  std::size_t da1_len = 0;
  std::size_t reply_start = std::string::npos;
  std::size_t reply_len = 0;
  for (std::size_t i = 0; i < raw.size();) {
    if (da1_start == std::string::npos) {
      if (const auto len = match_dec_private_reply(raw, i, 'c'); len > 0) {
        da1_start = i;
        da1_len = len;
        i += len;
        continue;
      }
    }
    if (reply_start == std::string::npos && is_reply) {
      if (const auto len = is_reply(raw, i); len > 0) {
        reply_start = i;
        reply_len = len;
        i += len;
        continue;
      }
    }
    ++i;
  }

  result.supported =
      reply_start != std::string::npos &&
      (da1_start == std::string::npos || reply_start < da1_start);

  result.leftover.reserve(raw.size());
  for (std::size_t i = 0; i < raw.size();) {
    if (i == da1_start) {
      i += da1_len;
      continue;
    }
    if (result.supported && i == reply_start) {
      i += reply_len;
      continue;
    }
    result.leftover.push_back(raw[i]);
    ++i;
  }
  return result;
}

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
  const std::filesystem::path &cur = cwd;
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

TerminalTitleController::TerminalTitleController(
    int fd, const std::string &initial_base_title)
    : fd_(fd), interval_(kTerminalTitleSpinnerInterval),
      is_tty_(isatty(fd_) != 0) {

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
    int fd, const std::string &initial_base_title, Writer writer,
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
    int fd, const std::string &initial_base_title, Writer writer,
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
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // A noexcept destructor must not let an exception escape; there is
    // nothing more to do during teardown than swallow it.
  }
}

void TerminalTitleController::set_base_title(const std::string &title) {
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
  worker_ =
      std::jthread([this](std::stop_token st) { worker_loop(std::move(st)); });
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
        base_copy, kTerminalTitleSpinnerFrames.at(idx));
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
  } catch (...) { // NOLINT(bugprone-empty-catch)
    // A noexcept destructor must not let an exception escape; there is
    // nothing more to do during teardown than swallow it.
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

std::vector<std::string> split_lines(std::string_view s, int width) {
  std::vector<std::string> out;
  std::string cur;
  int col = 0;
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\n') {
      out.push_back(std::move(cur));
      cur.clear();
      col = 0;
      ++i;
      continue;
    }
    if (s[i] == '\033') {
      const auto next = skip_ansi_sequence(s, i);
      if (next > i) {
        cur.append(s.substr(i, next - i));
        i = next;
        continue;
      }
    }
    const int cw = codepoint_width(s, i);
    const auto next = advance_utf8(s, i);
    if (col + cw > width && col > 0) {
      out.push_back(std::move(cur));
      cur.clear();
      col = 0;
    }
    cur.append(s.substr(i, next - i));
    col += cw;
    if (col >= width) {
      out.push_back(std::move(cur));
      cur.clear();
      col = 0;
    }
    i = next;
  }
  if (!cur.empty())
    out.push_back(std::move(cur));
  return out;
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

std::string truncate_tool_result(std::string_view content) {
  while (!content.empty() && (content.back() == '\n' || content.back() == '\r'))
    content.remove_suffix(1);

  std::vector<std::string_view> lines;
  std::size_t pos = 0;
  while (pos <= content.size()) {
    const std::size_t next = content.find('\n', pos);
    if (next == std::string_view::npos) {
      lines.emplace_back(content.substr(pos));
      break;
    }
    lines.emplace_back(content.substr(pos, next - pos));
    pos = next + 1;
  }
  if (lines.empty())
    lines.emplace_back();

  std::vector<std::string_view> visible;
  std::string omitted;
  if (lines.size() > 5) {
    visible.emplace_back(lines[0]);
    visible.emplace_back(lines[1]);
    omitted =
        "\xe2\x80\xa6 +" + std::to_string(lines.size() - 4) + " lines omitted";
    visible.push_back(omitted);
    visible.push_back(lines[lines.size() - 2]);
    visible.push_back(lines[lines.size() - 1]);
  } else {
    visible = std::move(lines);
  }

  std::string out;
  for (std::size_t i = 0; i < visible.size(); ++i) {
    out += (i == 0) ? " -> " : "    ";
    out += visible[i];
    if (i + 1 < visible.size())
      out += '\n';
  }
  return out;
}

std::string sanitize_tool_output(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    if (text[i] == '\033') {
      const std::size_t next = skip_ansi_sequence(text, i);
      if (next > i) {
        bool is_sgr = false;
        if (i + 1 < text.size() && text[i + 1] == '[' && next > 0) {
          const auto fin = static_cast<unsigned char>(text[next - 1]);
          if (fin == 'm')
            is_sgr = true;
        }
        if (is_sgr)
          out.append(text.substr(i, next - i));
        i = next;
        continue;
      }
      // Unrecognised ESC, drop it
      ++i;
      continue;
    }
    const auto lead = static_cast<unsigned char>(text[i]);
    const std::size_t next = advance_utf8(text, i);
    if (next == i + 1 && lead >= 0x80) {
      // Invalid UTF-8 byte, strip
      i = next;
      continue;
    }
    out.append(text.substr(i, next - i));
    i = next;
  }
  return out;
}

} // namespace pi::core
