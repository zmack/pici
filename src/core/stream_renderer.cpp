#include "core/stream_renderer.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <string_view>

#include "core/markdown.h"

namespace pi::core {
namespace {

static int term_width(int fd) {
  struct winsize ws{};
  if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
    return static_cast<int>(ws.ws_col);
  }
  return 80;
}

static std::size_t skip_ansi_sgr(std::string_view s, std::size_t i) {
  if (i + 1 < s.size() && s[i] == '\033' && s[i + 1] == '[') {
    i += 2;
    while (i < s.size()) {
      const char c = s[i];
      if ((c >= '@' && c <= '~')) {
        return i + 1;
      }
      ++i;
    }
  }
  return i;
}

static std::size_t advance_utf8(std::string_view s, std::size_t i) {
  if (i >= s.size()) {
    return i;
  }
  const unsigned char c = static_cast<unsigned char>(s[i]);
  if ((c & 0x80) == 0x00) {
    return i + 1;
  }
  if ((c & 0xE0) == 0xC0) {
    return std::min(i + 2, s.size());
  }
  if ((c & 0xF0) == 0xE0) {
    return std::min(i + 3, s.size());
  }
  if ((c & 0xF8) == 0xF0) {
    return std::min(i + 4, s.size());
  }
  return i + 1;
}

// Visual rows a single line (no embedded \n) occupies, accounting for
// ANSI escape sequences, UTF-8 glyphs, and terminal wrapping.
static int rows_for_line(std::string_view line, int width) {
  int col = 0;
  int rows = 1;
  for (std::size_t i = 0; i < line.size();) {
    if (line[i] == '\033') {
      const auto next = skip_ansi_sgr(line, i);
      if (next > i) {
        i = next;
        continue;
      }
    }
    i = advance_utf8(line, i);
    if (++col >= width) {
      ++rows;
      col = 0;
    }
  }
  return rows;
}

static int cursor_rows_for_rendered(std::string_view rendered, int width) {
  int rows = 1;
  int col = 0;
  for (std::size_t i = 0; i < rendered.size();) {
    if (rendered[i] == '\033') {
      const auto next = skip_ansi_sgr(rendered, i);
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
    i = advance_utf8(rendered, i);
    if (++col >= width) {
      ++rows;
      col = 0;
    }
  }
  return rows;
}

// Split on '\n', discarding the trailing empty element from a final newline.
static std::string strip_final_newline(std::string s) {
  if (!s.empty() && s.back() == '\n') {
    s.pop_back();
  }
  return s;
}

static int count_trailing_newlines(std::string_view s) {
  int n = 0;
  for (std::size_t i = s.size(); i > 0 && s[i - 1] == '\n'; --i) {
    ++n;
  }
  return n;
}

static std::string render_visible_markdown(std::string_view input) {
  auto rendered = strip_final_newline(render_markdown_ansi(input));
  rendered.append(static_cast<std::size_t>(count_trailing_newlines(input)),
                  '\n');
  return rendered;
}

// ─── RawStreamRenderer ────────────────────────────────────────────────────────

class RawStreamRenderer final : public StreamRenderer {
public:
  explicit RawStreamRenderer(int fd) : fd_(fd) {}

  void update(std::string_view delta) override {
    ::write(fd_, delta.data(), delta.size());
  }

  void finish() override {
    const char nl = '\n';
    ::write(fd_, &nl, 1);
  }

  void reset() override {}

private:
  int fd_;
};

// ─── DiffMarkdownRenderer ─────────────────────────────────────────────────────

class DiffMarkdownRenderer final : public StreamRenderer {
public:
  explicit DiffMarkdownRenderer(int fd) : fd_(fd) {}

  void update(std::string_view delta) override {
    text_buffer_ += delta;

    auto rendered = render_visible_markdown(text_buffer_);
    const int w = term_width(fd_);

    // Common case: streaming text extends the already-rendered output without
    // changing any earlier markdown structure, so append in place.
    if (!prev_rendered_.empty() && rendered.size() >= prev_rendered_.size() &&
        rendered.compare(0, prev_rendered_.size(), prev_rendered_) == 0) {
      const auto suffix = rendered.substr(prev_rendered_.size());
      if (!suffix.empty()) {
        ::write(fd_, suffix.data(), suffix.size());
      }
      prev_rendered_ = std::move(rendered);
      prev_cursor_rows_ = cursor_rows_for_rendered(prev_rendered_, w);
      return;
    }

    if (prev_rendered_.empty()) {
      if (!rendered.empty()) {
        ::write(fd_, rendered.data(), rendered.size());
      }
      prev_rendered_ = std::move(rendered);
      prev_cursor_rows_ = cursor_rows_for_rendered(prev_rendered_, w);
      return;
    }

    std::string frame;
    frame.reserve(32 + prev_rendered_.size() + rendered.size());
    frame += '\r';
    if (prev_cursor_rows_ > 1) {
      frame += "\033[";
      frame += std::to_string(prev_cursor_rows_ - 1);
      frame += 'A';
    }
    frame += "\033[J";
    frame += rendered;
    ::write(fd_, frame.data(), frame.size());

    prev_rendered_ = std::move(rendered);
    prev_cursor_rows_ = cursor_rows_for_rendered(prev_rendered_, w);
  }

  void finish() override {
    const char nl = '\n';
    ::write(fd_, &nl, 1);
    reset();
  }

  void reset() override {
    text_buffer_.clear();
    prev_rendered_.clear();
    prev_cursor_rows_ = 1;
  }

private:
  int fd_;
  std::string text_buffer_;
  std::string prev_rendered_;
  int prev_cursor_rows_{1};
};

} // namespace

std::unique_ptr<StreamRenderer> make_raw_renderer(int fd) {
  return std::make_unique<RawStreamRenderer>(fd);
}

std::unique_ptr<StreamRenderer> make_diff_renderer(int fd) {
  return std::make_unique<DiffMarkdownRenderer>(fd);
}

std::unique_ptr<StreamRenderer> make_auto_renderer(int fd) {
  if (::isatty(fd)) {
    return make_diff_renderer(fd);
  }
  return make_raw_renderer(fd);
}

} // namespace pi::core
