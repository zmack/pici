#include "core/stream_renderer.h"

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "core/markdown.h"
#include "core/terminal.h"

namespace pi::core {
namespace {

static int term_width(int fd) {
  struct winsize ws{};
  if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0)
    return static_cast<int>(ws.ws_col);
  return 80;
}

static int term_height(int fd) {
  struct winsize ws{};
  if (::ioctl(fd, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0)
    return static_cast<int>(ws.ws_row);
  return 24;
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

static constexpr std::string_view kViewportCursor = "\033[7m \033[0m";

static void append_viewport_cursor(std::string &rendered) {
  rendered.append(kViewportCursor);
}

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

    // Effective live-region start. Normally this is committed_bytes_ (the
    // committed prefix is intact and on screen verbatim). If the new render
    // diverges before committed_bytes_ (shared < committed_bytes_), the on-
    // screen committed content is stale — we must redraw further back.
    //
    // "Further back" = the last \n\n boundary before the divergence point in
    // the new render. Writing from there is always safe because \n\n is a
    // block boundary, never the interior of an ANSI escape sequence.
    // Without this, committed_bytes_ can point one byte past a \033 into a
    // [1;96m... sequence, emitting the [ literally instead of as ESC+[.
    std::size_t live_start = committed_bytes_;
    int         live_rows  = committed_rows_;

    if (shared < committed_bytes_) {
      live_start = 0;
      for (std::size_t s = 0; s + 1 < shared; ++s) {
        if (rv[s] == '\n' && rv[s + 1] == '\n')
          live_start = s + 2;
      }
      live_rows = cursor_rows_for_rendered(rv.substr(0, live_start), w);
    }

    std::size_t new_commit = live_start;
    {
      auto candidate = find_commit_boundary(rv, live_start);
      if (candidate > new_commit) new_commit = candidate;
    }

    const int rows_up = std::max(0, prev_cursor_rows_ - std::max(live_rows, 1));

    std::string frame;
    frame.reserve(64 + rendered.size() - live_start);

    frame += '\r';
    if (rows_up > 0) {
      frame += "\033[";
      frame += std::to_string(rows_up);
      frame += 'A';
    }
    frame += "\033[J";
    frame.append(rendered.data() + live_start, rendered.size() - live_start);

    ::write(fd_, frame.data(), frame.size());

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

// Full-screen compositor using the alternate screen buffer.
//
// Layout (h = terminal height):
//   rows 1..h-2  — content (DECSTBM scroll region)
//   row h-1      — status bar (token count, tool activity)
//   row h        — readline prompt; never written by this renderer
//
// Reserving row h for readline means the prompt's leading \n lands there
// without overlapping the status bar.

class ViewportRenderer final : public Renderer {
public:
  explicit ViewportRenderer(int fd) : fd_(fd) { enter(); }

  ~ViewportRenderer() {
    leave();
    if (current_ == this) current_ = nullptr;
  }

  void on_turn_start() override {
    raw_buffer_.clear();
    thinking_buffer_.clear();
    in_thinking_ = false;
    total_tokens_ = 0;
    status_text_.clear();
    active_tools_.clear();
    scroll_offset_rows_ = 0;
    max_scroll_rows_ = 0;
    scanner_   = {};
    fin_cache_ = {};
    set_scroll_region();
    write_seq("\033[H\033[J"); // home + erase content region
    paint_status();
  }

  void on_text_delta(std::string_view delta) override {
    raw_buffer_ += delta;
    repaint();
  }

  void on_scroll(RendererScrollCommand command) override {
    const int content_rows = std::max(1, term_height(fd_) - 2);
    const int page_rows = std::max(1, content_rows - 1);

    switch (command) {
    case RendererScrollCommand::line_up:
      scroll_offset_rows_ += 1;
      break;
    case RendererScrollCommand::line_down:
      scroll_offset_rows_ = std::max(0, scroll_offset_rows_ - 1);
      break;
    case RendererScrollCommand::page_up:
      scroll_offset_rows_ += page_rows;
      break;
    case RendererScrollCommand::page_down:
      scroll_offset_rows_ = std::max(0, scroll_offset_rows_ - page_rows);
      break;
    case RendererScrollCommand::top:
      scroll_offset_rows_ = std::numeric_limits<int>::max();
      break;
    case RendererScrollCommand::bottom:
      scroll_offset_rows_ = 0;
      break;
    }

    repaint();
  }

  void on_thinking_start() override {
    in_thinking_ = true;
    thinking_buffer_.clear();
  }

  void on_thinking_delta(std::string_view delta) override {
    thinking_buffer_ += delta;
    status_text_ = "[thinking\xe2\x80\xa6]";
    paint_status();
  }

  void on_thinking_end() override {
    in_thinking_ = false;
    status_text_.clear();
    // The thinking prefix length just became fixed.  Any scan_pos that was
    // advanced while raw_buffer_ was empty (or while thinking was growing)
    // may point into a stale content layout — reset both so the next repaint
    // scans cleanly from the beginning.
    scanner_   = {};
    fin_cache_ = {};
    repaint();
  }

  void on_tool_start(std::string_view call_id, std::string_view name,
                     std::string_view) override {
    active_tools_[std::string(call_id)] = std::string(name);
    paint_status();
  }

  void on_tool_end(std::string_view call_id, std::string_view,
                   const ToolResult &, bool) override {
    active_tools_.erase(std::string(call_id));
    paint_status();
  }

  void on_message_end(const TokenUsage &u) override {
    total_tokens_ += u.output;
    paint_status();
  }

  void on_turn_end() override {
    status_text_ = "tokens: " + std::to_string(total_tokens_) + "  done";
    paint_status();
  }

  void on_error(RendererErrorKind, std::string_view msg) override {
    status_text_ = "error: ";
    status_text_.append(msg.substr(0, 60));
    paint_status();
  }

private:
  void enter() {
    if (in_alt_) return;
    in_alt_ = true;
    current_ = this;
    if (!atexit_registered_) {
      std::atexit(atexit_fn);
      atexit_registered_ = true;
    }
    struct sigaction sa{};
    sa.sa_handler = sig_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT,  &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP,  &sa, nullptr);
    write_seq("\033[?1049h" // enter alternate screen
              "\033[?25l"   // hide cursor
              "\033[H\033[2J"); // home + clear
    set_scroll_region();
  }

  // Restore the terminal to the main screen — async-signal-safe.
  // Only writes escape codes; does not call malloc or cmark.
  void restore_terminal() {
    if (!in_alt_) return;
    in_alt_ = false;
    static constexpr char kRestore[] =
        "\033[r"       // reset scroll region
        "\033[?25h"   // show cursor (must precede ?1049l)
        "\033[?1049l"; // exit alternate screen
    ::write(fd_, kRestore, sizeof(kRestore) - 1);
  }

  void leave() {
    if (!in_alt_) return;
    restore_terminal(); // sets in_alt_ = false, writes escapes
    // Print last response to main-screen scrollback (may allocate — not
    // signal-safe, but leave() is only called from atexit or destructor).
    if (!raw_buffer_.empty()) {
      auto r = render_visible_markdown(raw_buffer_);
      ::write(fd_, r.data(), r.size());
      if (r.empty() || r.back() != '\n') ::write(fd_, "\n", 1);
    }
  }

  // Scroll region covers rows 1..h-2 (content only).
  // Row h-1 is the status bar; row h is left for readline's prompt.
  void set_scroll_region() {
    const int h = term_height(fd_);
    if (h < 3) return; // degenerate terminal: no room for content+status+input
    std::string s = "\033[1;";
    s += std::to_string(h - 2);
    s += "r";
    write_seq(s.c_str());
  }

  void repaint() {
    const int w            = term_width(fd_);
    const int h            = term_height(fd_);
    const int content_rows = h - 2;
    if (content_rows <= 0) return;

    // Build raw content: thinking block (if present) then the live response.
    std::string content;
    if (!thinking_buffer_.empty()) {
      content += "[thinking]\n";
      content += thinking_buffer_;
      content += "\n\n";
    }
    content += raw_buffer_;

    // ── Advance scanner (O(new bytes) only) ──────────────────────────────────
    scanner_.advance(content);
    const std::size_t boundary = scanner_.last_stable;

    // ── Invalidate finalized cache on terminal width change ───────────────────
    if (fin_cache_.width != w) {
      fin_cache_ = {};
    }

    std::string tail_rendered;

    if (boundary > fin_cache_.raw_end) {
      // ── Cache miss: boundary advanced (or first call after reset).
      // Render the full content once and extract the finalized prefix from the
      // actual output — this guarantees byte-exact correctness at the join point.
      // render(prefix_raw) ≠ full_render[0..boundary] in general due to cmark's
      // trailing-newline normalisation; using the real prefix avoids that.
      const std::string full_rendered = render_visible_markdown(content);

      // Find the last \n\n in the full rendered output.
      std::size_t rendered_boundary = 0;
      for (std::size_t i = 0; i + 1 < full_rendered.size(); ++i) {
        if (full_rendered[i] == '\n' && full_rendered[i + 1] == '\n')
          rendered_boundary = i + 2;
      }

      fin_cache_.rendered   = full_rendered.substr(0, rendered_boundary);
      fin_cache_.raw_end    = boundary;
      fin_cache_.width      = w;
      // O(fin_rendered.size()) row count — paid once per boundary advance.
      fin_cache_.row_count  = cursor_rows_for_rendered(fin_cache_.rendered, w);

      // The tail is the rest of the same render — no second parse needed.
      tail_rendered = full_rendered.substr(rendered_boundary);

    } else {
      // ── Cache hit: hot path — render only the suffix (O(tail.size())) ────────
      const std::string_view tail_raw(content.data() + fin_cache_.raw_end,
                                      content.size()  - fin_cache_.raw_end);
      tail_rendered = render_visible_markdown(tail_raw);
    }

    append_viewport_cursor(tail_rendered);

    // ── Build viewport from finalized cache + tail ────────────────────────────
    // Count tail rows without allocating line strings (O(tail.size())).
    const int tail_rows = cursor_rows_for_rendered(tail_rendered, w);

    std::string frame;
    frame += "\033[H\033[J"; // home + erase content region

    if (scroll_offset_rows_ == 0 && tail_rows >= content_rows) {
      // Fast path: all visible content is within the tail — finalized prefix is
      // completely off-screen.  Skip the split_lines walk over fin_cache_.rendered.
      const auto tail_lines_vec = split_lines(tail_rendered, w);
      const int  total          = static_cast<int>(tail_lines_vec.size());
      const int  first          = total - content_rows; // non-negative: tail_rows >= content_rows
      frame.reserve(tail_rendered.size() + static_cast<std::size_t>(content_rows) * 8);
      for (int i = first; i < total; ++i) {
        frame += tail_lines_vec[static_cast<std::size_t>(i)];
        if (i + 1 < total) frame += "\r\n";
      }
    } else {
      // Mixed / short / scrolled path: need finalized rows + all tail rows.
      // fin_cache_.rendered ends at \n\n so concatenation is join-clean.
      // When fin_cache_.rendered is empty (no boundary yet), combined equals
      // tail_rendered == render_visible_markdown(content) — identical to before G2.
      std::string combined;
      combined.reserve(fin_cache_.rendered.size() + tail_rendered.size());
      combined += fin_cache_.rendered;
      combined += tail_rendered;

      const auto lines         = split_lines(combined, w);
      const int  total         = static_cast<int>(lines.size());
      max_scroll_rows_         = std::max(0, total - content_rows);
      scroll_offset_rows_      = std::clamp(scroll_offset_rows_, 0, max_scroll_rows_);
      const int  first         = std::max(0, total - content_rows - scroll_offset_rows_);
      const int  last_plus_one = std::min(total, first + content_rows);
      frame.reserve(combined.size() + static_cast<std::size_t>(content_rows) * 8);
      for (int i = first; i < last_plus_one; ++i) {
        frame += lines[static_cast<std::size_t>(i)];
        if (i + 1 < last_plus_one) frame += "\r\n";
      }
    }

    ::write(fd_, frame.data(), frame.size());
    paint_status();
  }

  void paint_status() {
    const int w = term_width(fd_);
    const int h = term_height(fd_);

    std::string text;
    if (!active_tools_.empty()) {
      text = "[";
      bool first = true;
      for (const auto &[id, name] : active_tools_) {
        if (!first) text += ", ";
        text += name;
        first = false;
      }
      text += "]";
    } else {
      text = status_text_;
    }
    if (scroll_offset_rows_ > 0) {
      if (!text.empty()) text += "  ";
      text += "scroll ";
      text += std::to_string(scroll_offset_rows_);
      text += "/";
      text += std::to_string(max_scroll_rows_);
    }
    if (static_cast<int>(text.size()) > w)
      text.resize(static_cast<std::size_t>(w));

    // CUP addresses any row regardless of DECSTBM, so row h-1 is reachable
    // even though it's outside the scroll region.
    std::string bar;
    bar += "\033[";
    bar += std::to_string(h - 1);
    bar += ";1H\033[2K\033[2m";
    bar += text;
    bar += "\033[0m";
    ::write(fd_, bar.data(), bar.size());
  }

  // Split rendered ANSI string into wrapped physical rows of `width` columns.
  // ANSI/VT escapes (CSI, OSC, etc.) pass through without counting toward width.
  // Wide characters (CJK, emoji) count as 2 columns.
  static std::vector<std::string> split_lines(std::string_view s, int width) {
    std::vector<std::string> out;
    std::string cur;
    int col = 0;
    for (std::size_t i = 0; i < s.size();) {
      if (s[i] == '\n') {
        out.push_back(std::move(cur)); cur.clear(); col = 0; ++i; continue;
      }
      if (s[i] == '\033') {
        const auto nxt = skip_ansi_sequence(s, i);
        if (nxt > i) { cur.append(s.data() + i, nxt - i); i = nxt; continue; }
      }
      const int cw  = codepoint_width(s, i);
      const auto nxt = advance_utf8(s, i);
      if (col + cw > width && col > 0) { out.push_back(std::move(cur)); cur.clear(); col = 0; }
      cur.append(s.data() + i, nxt - i);
      col += cw;
      if (col >= width) { out.push_back(std::move(cur)); cur.clear(); col = 0; }
      i = nxt;
    }
    if (!cur.empty()) out.push_back(std::move(cur));
    return out;
  }

  void write_seq(const char *s) { ::write(fd_, s, std::strlen(s)); }

  // atexit: full leave() — safe to allocate here.
  static void atexit_fn() { if (current_) current_->leave(); }

  // Signal handler: restore_terminal() only — async-signal-safe (no malloc).
  // Calling cmark/render_visible_markdown from a signal handler is UB because
  // cmark calls malloc, which is not async-signal-safe and can deadlock.
  static void sig_handler(int sig) {
    if (current_) current_->restore_terminal();
    struct sigaction sa{};
    sa.sa_handler = SIG_DFL;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(sig, &sa, nullptr);
    raise(sig);
  }

  // Cached rendered ANSI for the finalized (complete-block) prefix of content.
  // Populated when BlockBoundaryScanner finds a new stable boundary.  On the
  // hot path (streaming mid-block), only the tail is re-rendered.
  struct FinCache {
    std::size_t  raw_end{0};    // scanner.last_stable when this cache was built
    std::string  rendered;      // full_rendered[0..rendered_boundary] from that render
    int          width{0};      // terminal width when rendered was computed
    int          row_count{0};  // visual rows of rendered at width (for G3 fast path)
  };

  int fd_;
  bool in_alt_{false};
  static bool atexit_registered_;
  std::string raw_buffer_;
  std::string thinking_buffer_;
  bool in_thinking_{false};
  std::map<std::string, std::string> active_tools_;
  std::string status_text_;
  std::uint64_t total_tokens_{0};
  int scroll_offset_rows_{0};
  int max_scroll_rows_{0};
  BlockBoundaryScanner scanner_;
  FinCache             fin_cache_;

  static ViewportRenderer *current_;
};

ViewportRenderer *ViewportRenderer::current_           = nullptr;
bool              ViewportRenderer::atexit_registered_ = false;

} // namespace

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

std::unique_ptr<Renderer> make_viewport_renderer(int fd) {
  return std::make_unique<ViewportRenderer>(fd);
}

std::unique_ptr<Renderer> make_auto_renderer(int fd) {
  if (::isatty(fd)) {
    return make_diff_renderer(fd);
  }
  return make_raw_renderer(fd);
}

StreamRendererRegistry::StreamRendererRegistry() {
  factories_["raw"]      = [](int fd) { return make_raw_renderer(fd); };
  factories_["markdown"] = [](int fd) { return make_diff_renderer(fd); };
  factories_["viewport"] = [](int fd) { return make_viewport_renderer(fd); };
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
