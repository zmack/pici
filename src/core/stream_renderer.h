#pragma once

#include "core/event_types.h"
#include "core/message_types.h"
#include "core/request_presentation.h"
#include "core/terminal.h"

#include <cstddef>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace pi::core {

struct RendererRequest {
  RequestPresentation presentation;
  std::string text;
  std::size_t non_text_attachments{0};
};

enum class RendererErrorKind {
  llm,       // LLM returned an error response
  transport, // network / HTTP error
  tool,      // tool execution error (also surfaces via on_tool_end)
  abort,     // agent was cancelled via stop_token
  unknown,
};

enum class RendererScrollCommand {
  line_up,
  line_down,
  page_up,
  page_down,
  top,
  bottom,
};

//
// Receives presentation-level events derived from the agent's AgentEvent
// stream.  All methods except on_text_delta have default no-op
// implementations, so implementations only override what they care about.
//
// Threading: dispatch_event() may be called from the thread that drives the
// EventStream iterator (usually the agent worker thread).  Implementations
// that touch UI state must marshal to the appropriate thread themselves.

class Renderer {
public:
  virtual ~Renderer() = default;

  // A new user → assistant turn is beginning.
  virtual void on_turn_start() {}

  // The typed request that initiated this turn. Renderers may ignore it.
  virtual void on_request(const RendererRequest &) {}

  // Streaming assistant answer text.  The only pure-virtual method.
  virtual void on_text_delta(std::string_view delta) = 0;

  // Streaming model reasoning / thinking (emitted by some models separately
  // from the answer).
  virtual void on_thinking_start() {}
  virtual void on_thinking_delta(std::string_view delta) {}
  virtual void on_thinking_end() {}

  // Tool call lifecycle.  call_id correlates start with end — important for
  // parallel tool execution where multiple calls may interleave.
  virtual void on_tool_start(std::string_view call_id,
                             std::string_view tool_name,
                             std::string_view args_json) {}
  virtual void on_tool_update(std::string_view call_id,
                              std::string_view tool_name,
                              std::string_view partial_result) {}
  virtual void on_tool_end(std::string_view call_id, std::string_view tool_name,
                           const ToolResult &result, bool is_error) {}

  // One assistant message is fully received (there may be several per turn
  // when tool calls are involved).  usage is the token count for this message.
  virtual void on_message_end(const TokenUsage &usage) {}

  // Presentation-level message completion, including the immutable stop
  // reason needed by semantic renderers. Legacy renderers receive usage via
  // the existing callback above.
  virtual void on_message_end_presentation(const MessageEndPresentation &end) {
    on_message_end(end.usage);
  }

  // A domain-level tool presentation notice, delivered on the event stream.
  virtual void on_mailbox_reply_queued(std::string_view,
                                       const MailboxReplyQueuedNotice &) {}

  // The entire agent turn is complete (all messages + tool results).
  virtual void on_turn_end() {}

  // User-facing output produced by a command or add-on. This is separate
  // from model text so it does not become part of the assistant response.
  virtual void on_command_output(std::string_view text) {}

  // An error occurred.  kind distinguishes LLM / transport / abort errors.
  virtual void on_error(RendererErrorKind kind, std::string_view message) {}

  // Compaction lifecycle. Renderers must not attempt to display the
  // replacement transcript's opaque server payload; these hooks intentionally
  // carry only counts/usage, never message content.
  virtual void on_compaction_start() {}
  virtual void on_compaction_complete(std::size_t retained_message_count,
                                      const TokenUsage &usage_before,
                                      const TokenUsage &usage_after) {}
  virtual void on_compaction_error(std::string_view message, bool cancelled) {}

  // Optional viewport/navigation input. Renderers without an addressable
  // viewport can ignore it.
  virtual void on_scroll(RendererScrollCommand command) {}

  // Optional status line shown above the readline prompt. Full-screen
  // renderers may own this row and paint it inside their compositor.
  virtual bool owns_status_line() const { return false; }
  virtual void set_status_line(const std::optional<std::string> &text) {}

  // The controlling terminal's dimensions changed. Renderers that paint a
  // fixed layout (status lines, alt-screen content) should recompute and
  // repaint here. Called synchronously, between turns, on the same thread
  // that drives the interactive prompt loop; implementations must not block.
  virtual void on_resize() {}

  // True if this renderer draws its own tool call/result presentation.
  virtual bool owns_tool_output() const { return false; }
  virtual void on_tool_output_text(std::string_view call_id,
                                   std::string_view text) {}

  // Force a full, non-diffed repaint of currently-known state, discarding
  // any diff cache. A renderer that owns a persistent alternate-screen
  // compositor (owns_status_line()) must implement this so a transient
  // full-screen command UI (e.g. /tree, /model) that drew directly over its
  // content — without its own nested alternate-screen enter/exit, since
  // nesting an alt-screen toggle inside one already owned by this renderer
  // corrupts the terminal — can hand back a clean screen afterward. No-op
  // for renderers without a persistent compositor to invalidate.
  virtual void force_full_repaint() {}

  // The interactive loop is about to hand control to readline() to accept
  // the next prompt. A renderer that owns a persistent alternate-screen
  // compositor and reserves fixed rows for the composer (owns_status_line())
  // must re-anchor the terminal cursor on that reserved area here — turn
  // boundaries are not a reliable signal for this, since a single
  // user-visible exchange can contain several on_turn_start()/on_turn_end()
  // pairs when the agent calls tools, and readline() only runs after the
  // last one. No-op for renderers without a persistent compositor.
  virtual void prepare_for_prompt() {}
};

//
// Translates one AgentEvent into the appropriate Renderer call(s).
// Call this inside any EventStream iteration loop to wire a renderer without
// coupling it to the agent internals.
//
//   for (const auto &ev : agent.prompt(text))
//     dispatch_event(ev, *renderer);
//
void dispatch_event(const AgentEvent &ev, Renderer &renderer);

// Render markdown while preserving the visible trailing newlines from input.
std::string render_visible_markdown(std::string_view input);

// Writes raw text deltas as they arrive (suitable for non-TTY / pipes).
std::unique_ptr<Renderer> make_raw_renderer(int fd = 1);

// Re-renders accumulated markdown on each delta, committed-region aware to
// avoid duplicating content in the terminal scrollback buffer.
std::unique_ptr<Renderer> make_diff_renderer(int fd = 1);

// Clears and repaints the full live viewport on every delta (Textual-style
// immediate-mode full-screen compositor).
std::unique_ptr<Renderer> make_viewport_renderer(int fd = 1);

// Throttled alternate-screen compositor with diffed transcript rows.
std::unique_ptr<Renderer> make_region_renderer(int fd = 1);

// Selects make_diff_renderer on a TTY, make_raw_renderer otherwise.
std::unique_ptr<Renderer> make_auto_renderer(int fd = 1);

// Legacy alias — kept for code that still refers to StreamRenderer.
using StreamRenderer = Renderer;

class StreamRendererRegistry {
public:
  using Factory = std::function<std::unique_ptr<Renderer>(int fd)>;

  void register_renderer(std::string name, Factory factory);
  std::unique_ptr<Renderer> make(const std::string &name, int fd) const;
  bool has(const std::string &name) const;

  static StreamRendererRegistry &instance();

private:
  StreamRendererRegistry();
  std::map<std::string, Factory> factories_;
  mutable std::mutex mutex_;
};

} // namespace pi::core
