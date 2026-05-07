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

class RawStreamRenderer final : public Renderer {
public:
  explicit RawStreamRenderer(int fd) : fd_(fd) {}

  void on_text_delta(std::string_view delta) override {
    ::write(fd_, delta.data(), delta.size());
  }

  void on_message_end(const TokenUsage &) override {
    const char nl = '\n';
    ::write(fd_, &nl, 1);
  }

private:
  int fd_;
};

// ─── DiffMarkdownRenderer ─────────────────────────────────────────────────────
//
// Scrollback-safe streaming renderer.
//
// The key insight: cursor-up + \033[J overwrites the terminal viewport but
// the earlier partial render is already in the scrollback buffer.  Doing this
// on every structural change (e.g. when a code fence closes and syntax
// highlighting fires) leaves a trail of intermediate renders in scrollback.
//
// Fix: split prev_rendered_ into two regions:
//   committed  — written once, lives in scrollback, never redrawn
//   live       — the current incomplete block, redrawn in place as needed
//
// A paragraph break (\n\n) or the beginning of a new markdown block after a
// newline is used as the commit boundary.  When a new render comes in:
//   1. If it extends the previous render without changing committed content,
//      just append the new suffix (fast path).
//   2. Otherwise, advance the commit point as far as possible, output any
//      newly-committed text as a plain append, then redraw only the live tail.

class DiffMarkdownRenderer final : public Renderer {
public:
  explicit DiffMarkdownRenderer(int fd) : fd_(fd) {}

  void on_text_delta(std::string_view delta) override {
    text_buffer_ += delta;

    auto rendered = render_visible_markdown(text_buffer_);
    const int w   = term_width(fd_);

    // ── Fast path: new render is a pure extension of what we already output ──
    if (!prev_rendered_.empty() &&
        rendered.size() >= prev_rendered_.size() &&
        rendered.compare(0, prev_rendered_.size(), prev_rendered_) == 0) {
      const auto suffix = rendered.substr(prev_rendered_.size());
      if (!suffix.empty()) {
        ::write(fd_, suffix.data(), suffix.size());
        advance_commit(rendered, w);
      }
      prev_rendered_     = std::move(rendered);
      prev_cursor_rows_  = cursor_rows_for_rendered(prev_rendered_, w);
      return;
    }

    // ── First write ───────────────────────────────────────────────────────────
    if (prev_rendered_.empty()) {
      if (!rendered.empty()) {
        ::write(fd_, rendered.data(), rendered.size());
        advance_commit(rendered, w);
      }
      prev_rendered_    = std::move(rendered);
      prev_cursor_rows_ = cursor_rows_for_rendered(prev_rendered_, w);
      return;
    }

    // ── Structural change: advance commit, then redraw only the live tail ─────
    //
    // 1. Find how much of `rendered` shares a prefix with the committed
    //    portion of prev_rendered_.  If the committed prefix is no longer
    //    a prefix of the new render, we can't do better than a full redraw
    //    of the live region (rare; only if committed content was wrong).
    //
    // 2. Any new text that extends past the old commit point and ends on a
    //    commit boundary is appended directly (committed).
    //
    // 3. The remaining live tail is redrawn in place.

    std::string_view rv(rendered);
    std::string_view old_committed(prev_rendered_.data(), committed_bytes_);

    // How much of the new render matches the old committed prefix?
    std::size_t shared = 0;
    while (shared < old_committed.size() && shared < rv.size() &&
           old_committed[shared] == rv[shared])
      ++shared;

    // If committed prefix is intact, we only need to redraw the live part.
    // Find what the new committed prefix will be (advance further if possible).
    std::size_t new_commit = committed_bytes_;
    {
      // Scan forward from the current commit point for new commit boundaries
      auto candidate = find_commit_boundary(rv, committed_bytes_);
      if (candidate > new_commit) new_commit = candidate;
    }

    // Bytes newly committed since last render
    const std::size_t extra_committed =
        new_commit > committed_bytes_ ? new_commit - committed_bytes_ : 0;

    // New live tail (everything after the new commit point)
    const std::string_view new_live = rv.substr(new_commit);

    // rows_up: how far to move the cursor back from the end of the previous
    // render to the start of the live region.
    //
    // committed_rows_ is the row the cursor sits on after writing the
    // committed portion (1 = start of content, N = start of row N).
    // When nothing is committed yet, committed_rows_ = 0 so we treat it as 1
    // (the cursor starts on row 1).
    //
    // To go from row prev_cursor_rows_ to row max(committed_rows_,1):
    //   rows_up = prev_cursor_rows_ - max(committed_rows_, 1)
    const int rows_up = prev_cursor_rows_ - std::max(committed_rows_, 1);

    std::string frame;
    frame.reserve(64 + rendered.size() - committed_bytes_);

    frame += '\r';
    if (rows_up > 0) {
      frame += "\033[";
      frame += std::to_string(rows_up);
      frame += 'A';
    }
    frame += "\033[J"; // erase from the start of live region to end of screen

    // Write committed tail (everything after the old commit point)
    frame.append(rendered.data() + committed_bytes_,
                 rendered.size() - committed_bytes_);

    ::write(fd_, frame.data(), frame.size());

    // Update state
    committed_bytes_  = new_commit;
    committed_rows_   = cursor_rows_for_rendered(
        std::string_view(rendered).substr(0, committed_bytes_), w);
    prev_rendered_    = std::move(rendered);
    prev_cursor_rows_ = cursor_rows_for_rendered(prev_rendered_, w);
  }

  void on_message_end(const TokenUsage &) override {
    const char nl = '\n';
    ::write(fd_, &nl, 1);
    clear_state();
  }

  void on_turn_start() override { clear_state(); }

  void clear_state() {
    text_buffer_.clear();
    prev_rendered_.clear();
    committed_bytes_ = 0;
    committed_rows_  = 0;
    prev_cursor_rows_ = 0;
  }

  int fd_;
  std::string text_buffer_;
  std::string prev_rendered_;
  std::size_t committed_bytes_{0}; // bytes in prev_rendered_ that are committed
  int committed_rows_{0};          // terminal rows for committed portion
  int prev_cursor_rows_{0};        // total terminal rows

  // Find the furthest byte offset in `rendered` that is a safe commit
  // boundary — i.e., a point where the rendered content is structurally
  // complete and unlikely to change on the next delta.
  //
  // Commit boundaries: any double-newline (\n\n) past the current commit.
  // A single trailing newline is NOT a commit boundary on its own because
  // the next token might start a new markdown construct in the same block.
  static std::size_t find_commit_boundary(std::string_view rendered,
                                           std::size_t from) {
    std::size_t best = from;
    for (std::size_t i = from; i + 1 < rendered.size(); ++i) {
      if (rendered[i] == '\n' && rendered[i + 1] == '\n') {
        best = i + 2; // commit through the blank line
      }
    }
    return best;
  }

  // After a plain-append update, try to advance the commit point.
  void advance_commit(const std::string &rendered, int w) {
    auto candidate = find_commit_boundary(rendered, committed_bytes_);
    if (candidate > committed_bytes_) {
      committed_bytes_ = candidate;
      committed_rows_  = cursor_rows_for_rendered(
          std::string_view(rendered).substr(0, committed_bytes_), w);
    }
  }
};

} // namespace

// ─── dispatch_event ───────────────────────────────────────────────────────────

void dispatch_event(const AgentEvent &ev, Renderer &r) {
  std::visit(
      [&r](const auto &e) {
        using T = std::decay_t<decltype(e)>;

        // ── Turn boundaries ──────────────────────────────────────────────────
        if constexpr (std::is_same_v<T, TurnStartEvent>) {
          r.on_turn_start();

        } else if constexpr (std::is_same_v<T, TurnEndEvent>) {
          r.on_turn_end();

        // ── Streaming assistant message ──────────────────────────────────────
        } else if constexpr (std::is_same_v<T, MessageUpdateEvent>) {
          std::visit(
              [&r](const auto &ae) {
                using AE = std::decay_t<decltype(ae)>;

                if constexpr (std::is_same_v<AE, AssistantMessageTextDeltaEvent>) {
                  r.on_text_delta(ae.delta);

                } else if constexpr (std::is_same_v<AE, AssistantMessageThinkingStartEvent>) {
                  r.on_thinking_start();

                } else if constexpr (std::is_same_v<AE, AssistantMessageThinkingDeltaEvent>) {
                  r.on_thinking_delta(ae.delta);

                } else if constexpr (std::is_same_v<AE, AssistantMessageThinkingEndEvent>) {
                  r.on_thinking_end();

                } else if constexpr (std::is_same_v<AE, AssistantMessageErrorEvent>) {
                  auto msg = ae.error.error_message.value_or("LLM error");
                  r.on_error(RendererErrorKind::llm, msg);
                }
              },
              e.assistant_message_event);

        // ── Message complete ─────────────────────────────────────────────────
        } else if constexpr (std::is_same_v<T, MessageEndEvent>) {
          if (const auto *am = std::get_if<AssistantMessage>(&e.message)) {
            if (am->error_message)
              r.on_error(RendererErrorKind::llm, *am->error_message);
            r.on_message_end(am->usage);
          }

        // ── Tool execution ───────────────────────────────────────────────────
        } else if constexpr (std::is_same_v<T, ToolExecutionStartEvent>) {
          r.on_tool_start(e.tool_call_id, e.tool_name, e.args);

        } else if constexpr (std::is_same_v<T, ToolExecutionEndEvent>) {
          if (e.result)
            r.on_tool_end(e.tool_call_id, e.tool_name, *e.result, e.is_error);

        // ── Agent abort ──────────────────────────────────────────────────────
        } else if constexpr (std::is_same_v<T, AgentEndEvent>) {
          // check for aborted stop reason in any final assistant message
          for (const auto &msg : e.messages) {
            if (const auto *am = std::get_if<AssistantMessage>(&msg)) {
              if (am->stop_reason == StopReason::aborted)
                r.on_error(RendererErrorKind::abort, "agent aborted");
            }
          }
        }
      },
      ev);
}

std::unique_ptr<Renderer> make_raw_renderer(int fd) {
  return std::make_unique<RawStreamRenderer>(fd);
}

std::unique_ptr<Renderer> make_diff_renderer(int fd) {
  return std::make_unique<DiffMarkdownRenderer>(fd);
}

std::unique_ptr<Renderer> make_auto_renderer(int fd) {
  if (::isatty(fd)) {
    return make_diff_renderer(fd);
  }
  return make_raw_renderer(fd);
}

// ─── StreamRendererRegistry ───────────────────────────────────────────────────

StreamRendererRegistry::StreamRendererRegistry() {
  factories_["raw"]      = [](int fd) { return make_raw_renderer(fd); };
  factories_["markdown"] = [](int fd) { return make_diff_renderer(fd); };
  factories_["auto"]     = [](int fd) { return make_auto_renderer(fd); };
}

StreamRendererRegistry &StreamRendererRegistry::instance() {
  static StreamRendererRegistry reg;
  return reg;
}

void StreamRendererRegistry::register_renderer(std::string name, Factory factory) {
  std::lock_guard<std::mutex> lk(mutex_);
  factories_[std::move(name)] = std::move(factory);
}

std::unique_ptr<Renderer>
StreamRendererRegistry::make(const std::string &name, int fd) const {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = factories_.find(name);
  if (it != factories_.end()) return it->second(fd);
  return nullptr;
}

bool StreamRendererRegistry::has(const std::string &name) const {
  std::lock_guard<std::mutex> lk(mutex_);
  return factories_.count(name) > 0;
}

} // namespace pi::core
