#pragma once

#include "core/event_types.h"
#include "core/message_types.h"

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace pi::core {

// ─── Error kind ──────────────────────────────────────────────────────────────

enum class RendererErrorKind {
  llm,        // LLM returned an error response
  transport,  // network / HTTP error
  tool,       // tool execution error (also surfaces via on_tool_end)
  abort,      // agent was cancelled via stop_token
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

// ─── Renderer ────────────────────────────────────────────────────────────────
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
  virtual void on_tool_end(std::string_view call_id,
                            std::string_view tool_name,
                            const ToolResult &result,
                            bool is_error) {}

  // One assistant message is fully received (there may be several per turn
  // when tool calls are involved).  usage is the token count for this message.
  virtual void on_message_end(const TokenUsage &usage) {}

  // The entire agent turn is complete (all messages + tool results).
  virtual void on_turn_end() {}

  // An error occurred.  kind distinguishes LLM / transport / abort errors.
  virtual void on_error(RendererErrorKind kind, std::string_view message) {}

  // Optional viewport/navigation input. Renderers without an addressable
  // viewport can ignore it.
  virtual void on_scroll(RendererScrollCommand command) {}
};

// ─── dispatch_event ──────────────────────────────────────────────────────────
//
// Translates one AgentEvent into the appropriate Renderer call(s).
// Call this inside any EventStream iteration loop to wire a renderer without
// coupling it to the agent internals.
//
//   for (const auto &ev : agent.prompt(text))
//     dispatch_event(ev, *renderer);
//
void dispatch_event(const AgentEvent &ev, Renderer &renderer);

// ─── Built-in renderer factories ─────────────────────────────────────────────

// Writes raw text deltas as they arrive (suitable for non-TTY / pipes).
std::unique_ptr<Renderer> make_raw_renderer(int fd = 1);

// Re-renders accumulated markdown on each delta, committed-region aware to
// avoid duplicating content in the terminal scrollback buffer.
std::unique_ptr<Renderer> make_diff_renderer(int fd = 1);

// Clears and repaints the full live viewport on every delta (Textual-style
// immediate-mode full-screen compositor).
std::unique_ptr<Renderer> make_viewport_renderer(int fd = 1);

// Selects make_diff_renderer on a TTY, make_raw_renderer otherwise.
std::unique_ptr<Renderer> make_auto_renderer(int fd = 1);

// Legacy alias — kept for code that still refers to StreamRenderer.
using StreamRenderer = Renderer;

// ─── Registry ────────────────────────────────────────────────────────────────

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
