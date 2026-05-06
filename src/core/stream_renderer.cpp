#include "core/stream_renderer.h"

#include <sys/ioctl.h>
#include <unistd.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

// Visual rows a single line (no embedded \n) occupies, accounting for
// ANSI escape sequences and terminal wrapping.
static int rows_for_line(std::string_view line, int width) {
  int col = 0;
  int rows = 1;
  for (std::size_t i = 0; i < line.size(); ++i) {
    if (line[i] == '\033' && i + 1 < line.size() && line[i + 1] == '[') {
      i += 2;
      while (i < line.size() && line[i] != 'm') {
        ++i;
      }
      continue;
    }
    if (++col >= width) {
      ++rows;
      col = 0;
    }
  }
  return rows;
}

// Split on '\n', discarding the trailing empty element from a final newline.
static std::vector<std::string> split_lines(const std::string &s) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\n') {
      lines.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  if (start < s.size()) {
    lines.push_back(s.substr(start));
  }
  return lines;
}

static int total_rows(const std::vector<std::string> &lines, int width) {
  int n = 0;
  for (const auto &l : lines) {
    n += rows_for_line(l, width);
  }
  return n;
}

static std::string strip_trailing_newline(std::string s) {
  if (!s.empty() && s.back() == '\n') {
    s.pop_back();
  }
  return s;
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

    auto rendered = strip_trailing_newline(render_markdown_ansi(text_buffer_));
    auto new_lines = split_lines(rendered);
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
      prev_total_rows_ = total_rows(new_lines, w);
      prev_lines_ = std::move(new_lines);
      return;
    }

    if (prev_rendered_.empty()) {
      if (!rendered.empty()) {
        ::write(fd_, rendered.data(), rendered.size());
      }
      prev_rendered_ = std::move(rendered);
      prev_total_rows_ = total_rows(new_lines, w);
      prev_lines_ = std::move(new_lines);
      return;
    }

    // Find the first line that differs from the previous frame.
    std::size_t diff_at = 0;
    while (diff_at < prev_lines_.size() && diff_at < new_lines.size() &&
           prev_lines_[diff_at] == new_lines[diff_at]) {
      ++diff_at;
    }

    // How many visual rows the unchanged prefix above diff_at occupies.
    int unchanged_rows = 0;
    for (std::size_t i = 0; i < diff_at; ++i) {
      unchanged_rows += rows_for_line(prev_lines_[i], w);
    }

    // The cursor stays at the end of the rendered content, so move back to the
    // first row of the changed suffix, clear it, and repaint only that tail.
    const int rows_to_repaint = prev_total_rows_ - unchanged_rows;
    const bool has_new = diff_at < new_lines.size();

    if (rows_to_repaint > 0 || has_new) {
      std::string frame;
      frame.reserve(64 + rendered.size());
      frame += '\r';
      if (rows_to_repaint > 1) {
        frame += "\033[";
        frame += std::to_string(rows_to_repaint - 1);
        frame += 'A';
      }
      frame += "\033[J";
      if (has_new) {
        for (std::size_t i = diff_at; i < new_lines.size(); ++i) {
          if (i > diff_at) {
            frame += '\n';
          }
          frame += new_lines[i];
        }
      }
      ::write(fd_, frame.data(), frame.size());
    }

    prev_rendered_ = std::move(rendered);
    prev_total_rows_ = total_rows(new_lines, w);
    prev_lines_ = std::move(new_lines);
  }

  void finish() override {
    const char nl = '\n';
    ::write(fd_, &nl, 1);
    reset();
  }

  void reset() override {
    text_buffer_.clear();
    prev_rendered_.clear();
    prev_lines_.clear();
    prev_total_rows_ = 0;
  }

private:
  int fd_;
  std::string text_buffer_;
  std::string prev_rendered_;
  std::vector<std::string> prev_lines_;
  int prev_total_rows_{0};
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
