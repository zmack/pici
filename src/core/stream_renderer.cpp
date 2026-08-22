#include "core/stream_renderer.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ios>
#include <mutex>
#include <type_traits>
#include <unistd.h>

#include <format>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "core/event_types.h"
#include "core/markdown.h"
#include "core/message_types.h"
#include "core/request_presentation.h"
#include "core/terminal.h"

namespace pi::core {
namespace {

// Split on '\n', discarding the trailing empty element from a final newline.
std::string strip_final_newline(std::string s) {
  if (!s.empty() && s.back() == '\n') {
    s.pop_back();
  }
  return s;
}

int count_trailing_newlines(std::string_view s) {
  int n = 0;
  for (std::size_t i = s.size(); i > 0 && s[i - 1] == '\n'; --i) {
    ++n;
  }
  return n;
}

std::string format_tokens(std::uint64_t n) {
  std::ostringstream ss;
  if (n >= 1'000'000) {
    ss << std::fixed << std::setprecision(1)
       << static_cast<double>(n) / 1'000'000.0 << 'M';
  } else if (n >= 1'000) {
    ss << std::fixed << std::setprecision(1) << static_cast<double>(n) / 1'000.0
       << 'K';
  } else {
    ss << n;
  }
  return ss.str();
}

std::string format_cost(double usd) {
  std::ostringstream ss;
  ss << '$';
  if (usd < 0.01)
    ss << std::format("{:.4g}", usd);
  else if (usd < 1.00)
    ss << std::fixed << std::setprecision(4) << usd;
  else
    ss << std::fixed << std::setprecision(2) << usd;
  return ss.str();
}

class RawStreamRenderer final : public Renderer {
public:
  explicit RawStreamRenderer(int fd) : fd_(fd) {}

  void on_text_delta(std::string_view delta) override {
    write_best_effort(fd_, delta.data(), delta.size());
  }

  void on_message_end(const TokenUsage &) override {
    const char nl = '\n';
    write_best_effort(fd_, &nl, 1);
  }

  void on_command_output(std::string_view text) override {
    write_best_effort(fd_, text.data(), text.size());
    if (text.empty() || text.back() != '\n')
      write_best_effort(fd_, "\n", 1);
  }

  // Concise plain text only: never the replacement transcript's opaque
  // server payload, which these hooks never carry in the first place.
  void on_compaction_start() override {
    static constexpr std::string_view msg = "[compacting context...]\n";
    write_best_effort(fd_, msg.data(), msg.size());
  }

  void on_compaction_complete(std::size_t retained_message_count,
                              const TokenUsage &, const TokenUsage &) override {
    const std::string msg =
        "[context compacted: " + std::to_string(retained_message_count) +
        " message(s) retained]\n";
    write_best_effort(fd_, msg.data(), msg.size());
  }

  void on_compaction_error(std::string_view message, bool cancelled) override {
    const std::string msg =
        cancelled ? "[compaction cancelled]\n"
                  : "[compaction failed: " + std::string(message) + "]\n";
    write_best_effort(fd_, msg.data(), msg.size());
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
    const int w = term_width(fd_);

    if (!prev_rendered_.empty() && rendered.size() >= prev_rendered_.size() &&
        rendered.starts_with(prev_rendered_)) {
      const auto suffix = rendered.substr(prev_rendered_.size());
      if (!suffix.empty()) {
        write_best_effort(fd_, suffix.data(), suffix.size());
        advance_commit(rendered, w);
      }
      prev_rendered_ = std::move(rendered);
      prev_cursor_rows_ = cursor_rows_for_rendered(prev_rendered_, w);
      return;
    }

    if (prev_rendered_.empty()) {
      if (!rendered.empty()) {
        write_best_effort(fd_, rendered.data(), rendered.size());
        advance_commit(rendered, w);
      }
      prev_rendered_ = std::move(rendered);
      prev_cursor_rows_ = cursor_rows_for_rendered(prev_rendered_, w);
      return;
    }

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
    int live_rows = committed_rows_;

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
      new_commit = std::max(candidate, new_commit);
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
    frame.append(rendered.substr(live_start));

    write_best_effort(fd_, frame.data(), frame.size());

    committed_bytes_ = new_commit;
    committed_rows_ = cursor_rows_for_rendered(
        std::string_view(rendered).substr(0, committed_bytes_), w);
    prev_rendered_ = std::move(rendered);
    prev_cursor_rows_ = cursor_rows_for_rendered(prev_rendered_, w);
  }

  void on_message_end(const TokenUsage &) override {
    const char nl = '\n';
    write_best_effort(fd_, &nl, 1);
    clear_state();
  }

  void on_command_output(std::string_view text) override {
    write_best_effort(fd_, text.data(), text.size());
    if (text.empty() || text.back() != '\n')
      write_best_effort(fd_, "\n", 1);
  }

  // Plain text status, matching on_command_output — never the replacement
  // transcript's opaque server payload, which these hooks never carry.
  void on_compaction_start() override {
    static constexpr std::string_view msg = "[compacting context...]\n";
    write_best_effort(fd_, msg.data(), msg.size());
  }

  void on_compaction_complete(std::size_t retained_message_count,
                              const TokenUsage &, const TokenUsage &) override {
    const std::string msg =
        "[context compacted: " + std::to_string(retained_message_count) +
        " message(s) retained]\n";
    write_best_effort(fd_, msg.data(), msg.size());
  }

  void on_compaction_error(std::string_view message, bool cancelled) override {
    const std::string msg =
        cancelled ? "[compaction cancelled]\n"
                  : "[compaction failed: " + std::string(message) + "]\n";
    write_best_effort(fd_, msg.data(), msg.size());
  }

  void on_turn_start() override { clear_state(); }

  void clear_state() {
    text_buffer_.clear();
    prev_rendered_.clear();
    committed_bytes_ = 0;
    committed_rows_ = 0;
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
      committed_rows_ = cursor_rows_for_rendered(
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
  explicit ViewportRenderer(int fd) : fd_(fd), alt_screen_(fd) {
    set_scroll_region();
  }
  ViewportRenderer(const ViewportRenderer &) = delete;
  ViewportRenderer &operator=(const ViewportRenderer &) = delete;
  ViewportRenderer(ViewportRenderer &&) = delete;
  ViewportRenderer &operator=(ViewportRenderer &&) = delete;

  ~ViewportRenderer() noexcept override {
    try {
      leave();
    } catch (...) { // NOLINT(bugprone-empty-catch)
    }
  }

  void on_turn_start() override {
    raw_buffer_.clear();
    thinking_buffer_.clear();
    in_thinking_ = false;
    total_tokens_ = 0;
    last_usage_ = TokenUsage{};
    status_text_.clear();
    active_tools_.clear();
    scroll_offset_rows_ = 0;
    max_scroll_rows_ = 0;
    scanner_ = {};
    fin_cache_ = {};
    set_scroll_region();
    write_seq("\033[?25l"      // hide cursor while the viewport is streaming
              "\033[H\033[J"); // home + erase content region
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

  bool owns_status_line() const override { return true; }

  void set_status_line(const std::optional<std::string> &text) override {
    custom_status_line_ = text;
    paint_status();
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
    scanner_ = {};
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
    last_usage_ = u;
    paint_status();
  }

  void on_turn_end() override {
    status_text_ = "tokens: " + std::to_string(total_tokens_) + "  done";
    paint_status();
    write_seq("\033[?25h");
  }

  void on_command_output(std::string_view text) override {
    if (!raw_buffer_.empty() && raw_buffer_.back() != '\n')
      raw_buffer_ += '\n';
    raw_buffer_ += '\n';
    raw_buffer_.append(text.data(), text.size());
    scanner_ = {};
    fin_cache_ = {};
    scroll_offset_rows_ = 0;
    max_scroll_rows_ = 0;
    repaint();
  }

  void on_error(RendererErrorKind, std::string_view msg) override {
    status_text_ = "error: ";
    status_text_.append(msg.substr(0, 60));
    paint_status();
    write_seq("\033[?25h");
  }

  // Status-line only: never render the replacement transcript's opaque
  // server payload, which these hooks never carry in the first place.
  void on_compaction_start() override {
    status_text_ = "compacting context\xe2\x80\xa6";
    paint_status();
  }

  void on_compaction_complete(std::size_t retained_message_count,
                              const TokenUsage &, const TokenUsage &) override {
    status_text_ = "context compacted (" +
                   std::to_string(retained_message_count) + " retained)";
    paint_status();
  }

  void on_compaction_error(std::string_view message, bool cancelled) override {
    if (cancelled) {
      status_text_ = "compaction cancelled";
    } else {
      status_text_ = "compaction failed: ";
      status_text_.append(message.substr(0, 60));
    }
    paint_status();
  }

private:
  void leave() {
    if (left_)
      return;
    left_ = true;
    alt_screen_.leave();
    // Print last response to main-screen scrollback (may allocate — not
    // signal-safe, but leave() is only called from atexit or destructor).
    if (!raw_buffer_.empty()) {
      auto r = render_visible_markdown(raw_buffer_);
      write_best_effort(fd_, r.data(), r.size());
      if (r.empty() || r.back() != '\n')
        write_best_effort(fd_, "\n", 1);
    }
  }

  // Scroll region covers rows 1..h-2 (content only).
  // Row h-1 is the status bar; row h is left for readline's prompt.
  void set_scroll_region() {
    const int h = term_height(fd_);
    if (h < 3)
      return; // degenerate terminal: no room for content+status+input
    std::string s = "\033[1;";
    s += std::to_string(h - 2);
    s += 'r';
    write_seq(s.c_str());
  }

  void repaint() {
    const int w = term_width(fd_);
    const int h = term_height(fd_);
    const int content_rows = h - 2;
    if (content_rows <= 0)
      return;

    // Build raw content: thinking block (if present) then the live response.
    std::string content;
    if (!thinking_buffer_.empty()) {
      content += "[thinking]\n";
      content += thinking_buffer_;
      content += "\n\n";
    }
    content += raw_buffer_;

    scanner_.advance(content);
    const std::size_t boundary = scanner_.last_stable;

    if (fin_cache_.width != w) {
      fin_cache_ = {};
    }

    std::string tail_rendered;

    if (boundary > fin_cache_.raw_end) {

      // Render the full content once and extract the finalized prefix from the
      // actual output — this guarantees byte-exact correctness at the join
      // point. render(prefix_raw) ≠ full_render[0..boundary] in general due to
      // cmark's trailing-newline normalisation; using the real prefix avoids
      // that.
      const std::string full_rendered = render_visible_markdown(content);

      // Find the last \n\n in the full rendered output.
      std::size_t rendered_boundary = 0;
      for (std::size_t i = 0; i + 1 < full_rendered.size(); ++i) {
        if (full_rendered[i] == '\n' && full_rendered[i + 1] == '\n')
          rendered_boundary = i + 2;
      }

      fin_cache_.rendered = full_rendered.substr(0, rendered_boundary);
      fin_cache_.raw_end = boundary;
      fin_cache_.width = w;
      // O(fin_rendered.size()) row count — paid once per boundary advance.
      fin_cache_.row_count = cursor_rows_for_rendered(fin_cache_.rendered, w);

      // The tail is the rest of the same render — no second parse needed.
      tail_rendered = full_rendered.substr(rendered_boundary);

    } else {

      const std::string_view tail_raw =
          std::string_view(content).substr(fin_cache_.raw_end);
      tail_rendered = render_visible_markdown(tail_raw);
    }

    // strings (O(tail.size())).
    const int tail_rows = cursor_rows_for_rendered(tail_rendered, w);

    std::string frame;
    frame += "\033[H\033[J"; // home + erase content region

    if (scroll_offset_rows_ == 0 && tail_rows >= content_rows) {
      // Fast path: all visible content is within the tail — finalized prefix is
      // completely off-screen.  Skip the split_lines walk over
      // fin_cache_.rendered.
      const auto tail_lines_vec = split_lines(tail_rendered, w);
      const int total = static_cast<int>(tail_lines_vec.size());
      const int first =
          total - content_rows; // non-negative: tail_rows >= content_rows
      frame.reserve(tail_rendered.size() +
                    (static_cast<std::size_t>(content_rows) * 8));
      for (int i = first; i < total; ++i) {
        frame += tail_lines_vec[static_cast<std::size_t>(i)];
        if (i + 1 < total)
          frame += "\r\n";
      }
    } else {
      // Mixed / short / scrolled path: need finalized rows + all tail rows.
      // fin_cache_.rendered ends at \n\n so concatenation is join-clean.
      // When fin_cache_.rendered is empty (no boundary yet), combined equals
      // tail_rendered == render_visible_markdown(content) — identical to before
      // G2.
      std::string combined;
      combined.reserve(fin_cache_.rendered.size() + tail_rendered.size());
      combined += fin_cache_.rendered;
      combined += tail_rendered;

      const auto lines = split_lines(combined, w);
      const int total = static_cast<int>(lines.size());
      max_scroll_rows_ = std::max(0, total - content_rows);
      scroll_offset_rows_ =
          std::clamp(scroll_offset_rows_, 0, max_scroll_rows_);
      const int first = std::max(0, total - content_rows - scroll_offset_rows_);
      const int last_plus_one = std::min(total, first + content_rows);
      frame.reserve(combined.size() +
                    (static_cast<std::size_t>(content_rows) * 8));
      for (int i = first; i < last_plus_one; ++i) {
        frame += lines[static_cast<std::size_t>(i)];
        if (i + 1 < last_plus_one)
          frame += "\r\n";
      }
    }

    write_best_effort(fd_, frame.data(), frame.size());
    paint_status();
  }

  void paint_status() {
    const int w = term_width(fd_);
    const int h = term_height(fd_);

    std::string text;
    if (custom_status_line_) {
      text = *custom_status_line_;
    } else if (!active_tools_.empty()) {
      text = "[";
      bool first = true;
      for (const auto &[id, name] : active_tools_) {
        if (!first)
          text += ", ";
        text += name;
        first = false;
      }
      text += ']';
    } else {
      text = status_text_;
    }
    if (scroll_offset_rows_ > 0) {
      if (!text.empty())
        text += "  ";
      text += "scroll ";
      text += std::to_string(scroll_offset_rows_);
      text += '/';
      text += std::to_string(max_scroll_rows_);
    }
    text = truncate_ansi_line(text, w);

    std::string usage_text;
    if (last_usage_.input != 0 || last_usage_.output != 0) {
      usage_text = "in:" + format_tokens(last_usage_.input) +
                   " out:" + format_tokens(last_usage_.output);
      bool has_pricing =
          last_usage_.cost.total != 0 || last_usage_.cost.input != 0;
      if (has_pricing) {
        usage_text += ' ';
        usage_text += format_cost(last_usage_.cost.total);
      }
    }
    int gap = w - display_columns(text) - static_cast<int>(usage_text.size());
    if (usage_text.empty() || gap < 1)
      usage_text.clear();

    // CUP addresses any row regardless of DECSTBM, so row h-1 is reachable
    // even though it's outside the scroll region.
    std::string bar;
    bar += "\033[";
    bar += std::to_string(h - 1);
    bar += ";1H\033[2K\033[2m";
    bar += text;
    if (!usage_text.empty()) {
      bar.append(static_cast<std::size_t>(gap), ' ');
      bar += usage_text;
    }
    bar += "\033[0m";
    write_best_effort(fd_, bar.data(), bar.size());
  }

  void write_seq(const char *s) const { write_best_effort(fd_, s, std::strlen(s)); }

  // Cached rendered ANSI for the finalized (complete-block) prefix of content.
  // Populated when BlockBoundaryScanner finds a new stable boundary.  On the
  // hot path (streaming mid-block), only the tail is re-rendered.
  struct FinCache {
    std::size_t raw_end{0}; // scanner.last_stable when this cache was built
    std::string
        rendered;     // full_rendered[0..rendered_boundary] from that render
    int width{0};     // terminal width when rendered was computed
    int row_count{0}; // visual rows of rendered at width (for G3 fast path)
  };

  int fd_;
  AltScreenSession alt_screen_;
  bool left_{false};
  std::string raw_buffer_;
  std::string thinking_buffer_;
  bool in_thinking_{false};
  std::map<std::string, std::string> active_tools_;
  std::optional<std::string> custom_status_line_;
  std::string status_text_;
  std::uint64_t total_tokens_{0};
  TokenUsage last_usage_;
  int scroll_offset_rows_{0};
  int max_scroll_rows_{0};
  BlockBoundaryScanner scanner_;
  FinCache fin_cache_;
};

} // namespace

std::string render_visible_markdown(std::string_view input) {
  auto rendered = strip_final_newline(render_markdown_ansi(input));
  rendered.append(static_cast<std::size_t>(count_trailing_newlines(input)),
                  '\n');
  return rendered;
}

void dispatch_event(const AgentEvent &ev, Renderer &r) {
  std::visit(
      [&r](const auto &e) {
        using T = std::decay_t<decltype(e)>;

        if constexpr (std::is_same_v<T, TurnStartEvent>) {
          r.on_turn_start();

        } else if constexpr (std::is_same_v<T, TurnEndEvent>) {
          r.on_turn_end();

        } else if constexpr (std::is_same_v<T, MessageStartEvent>) {
          if (const auto *user = std::get_if<UserMessage>(&e.message)) {
            RendererRequest request{
                .presentation = e.request.value_or(RequestPresentation{})};
            for (const auto &content : user->content) {
              std::visit(
                  [&request](const auto &block) {
                    using Block = std::decay_t<decltype(block)>;
                    if constexpr (std::is_same_v<Block, TextContent>)
                      request.text += block.text;
                    else
                      ++request.non_text_attachments;
                  },
                  content);
            }
            r.on_request(request);
          }
        } else if constexpr (std::is_same_v<T, MessageUpdateEvent>) {
          if (const auto *am = std::get_if<AssistantMessage>(&e.message))
            r.on_usage_update(am->usage);
          std::visit(
              [&r](const auto &ae) {
                using AE = std::decay_t<decltype(ae)>;

                if constexpr (std::is_same_v<AE,
                                             AssistantMessageTextDeltaEvent>) {
                  r.on_text_delta(ae.delta);

                } else if constexpr (std::is_same_v<
                                         AE,
                                         AssistantMessageToolCallStartEvent> ||
                                     std::is_same_v<
                                         AE,
                                         AssistantMessageToolCallDeltaEvent>) {
                  if (ae.content_index < ae.partial.content.size()) {
                    if (const auto *tc = std::get_if<ToolCall>(
                            &ae.partial.content[ae.content_index])) {
                      r.on_tool_call_streaming(ae.content_index, tc->id,
                                               tc->name, tc->partial_json);
                    }
                  }

                } else if constexpr (std::is_same_v<
                                         AE,
                                         AssistantMessageThinkingStartEvent>) {
                  r.on_thinking_start();

                } else if constexpr (std::is_same_v<
                                         AE,
                                         AssistantMessageThinkingDeltaEvent>) {
                  r.on_thinking_delta(ae.delta);

                } else if constexpr (std::is_same_v<
                                         AE,
                                         AssistantMessageThinkingEndEvent>) {
                  r.on_thinking_end();

                } else if constexpr (std::is_same_v<
                                         AE, AssistantMessageErrorEvent>) {
                  auto msg = ae.error.error_message.value_or("LLM error");
                  r.on_error(RendererErrorKind::llm, msg);
                }
              },
              e.assistant_message_event);

        } else if constexpr (std::is_same_v<T, MessageEndEvent>) {
          if (const auto *am = std::get_if<AssistantMessage>(&e.message)) {
            if (am->error_message)
              r.on_error(RendererErrorKind::llm, *am->error_message);
            r.on_message_end_presentation(e.presentation);
          }

        } else if constexpr (std::is_same_v<T, ToolExecutionStartEvent>) {
          r.on_tool_start(e.tool_call_id, e.tool_name, e.args);

        } else if constexpr (std::is_same_v<T, ToolExecutionUpdateEvent>) {
          r.on_tool_update(e.tool_call_id, e.tool_name, e.partial_result);

        } else if constexpr (std::is_same_v<T, ToolExecutionEndEvent>) {
          if (e.result)
            r.on_tool_end(e.tool_call_id, e.tool_name, *e.result, e.is_error);

        } else if constexpr (std::is_same_v<T, ToolPresentationEvent>) {
          if (const auto *notice =
                  std::get_if<MailboxReplyQueuedNotice>(&e.notice))
            r.on_mailbox_reply_queued(e.tool_call_id, *notice);

        } else if constexpr (std::is_same_v<T, TurnAbortedEvent>) {
          r.on_error(RendererErrorKind::abort, "agent turn aborted");
        } else if constexpr (std::is_same_v<T, AgentEndEvent>) {
          // check for aborted stop reason in any final assistant message
          for (const auto &msg : e.messages) {
            if (const auto *am = std::get_if<AssistantMessage>(&msg)) {
              if (am->stop_reason == StopReason::aborted)
                r.on_error(RendererErrorKind::abort, "agent aborted");
            }
          }
        } else if constexpr (std::is_same_v<T, CompactionEvent>) {
          if (e.kind == CompactionEventKind::start) {
            r.on_compaction_start();
          } else if (e.kind == CompactionEventKind::complete) {
            r.on_compaction_complete(e.retained_message_count, e.usage_before,
                                     e.usage_after);
          } else {
            r.on_compaction_error(e.error_message.value_or("compaction failed"),
                                  e.cancelled);
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
  if (::isatty(fd) != 0) {
    return make_diff_renderer(fd);
  }
  return make_raw_renderer(fd);
}

StreamRendererRegistry::StreamRendererRegistry() {
  factories_["raw"] = [](int fd) { return make_raw_renderer(fd); };
  factories_["markdown"] = [](int fd) { return make_diff_renderer(fd); };
  factories_["viewport"] = [](int fd) { return make_viewport_renderer(fd); };
  factories_["region"] = [](int fd) { return make_region_renderer(fd); };
  factories_["auto"] = [](int fd) { return make_auto_renderer(fd); };
}

StreamRendererRegistry &StreamRendererRegistry::instance() {
  static StreamRendererRegistry reg;
  return reg;
}

void StreamRendererRegistry::register_renderer(std::string name,
                                               Factory factory) {
  std::scoped_lock lk(mutex_);
  factories_[std::move(name)] = std::move(factory);
}

std::unique_ptr<Renderer> StreamRendererRegistry::make(const std::string &name,
                                                       int fd) const {
  std::scoped_lock lk(mutex_);
  auto it = factories_.find(name);
  if (it != factories_.end())
    return it->second(fd);
  return nullptr;
}

bool StreamRendererRegistry::has(const std::string &name) const {
  std::scoped_lock lk(mutex_);
  return factories_.contains(name);
}

} // namespace pi::core
