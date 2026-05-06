#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

#include "core/agent_loop.h"
#include "core/message_types.h"

namespace pi::core {

// Load a single Lua tool from a .lua file.
// Throws std::runtime_error if the file cannot be loaded or is invalid.
std::shared_ptr<const ToolDefinition>
load_lua_tool(const std::filesystem::path &path);

// Load all .lua files from a directory as tools.
// Files that fail to load are silently skipped.
std::vector<std::shared_ptr<const ToolDefinition>>
load_lua_tools(const std::filesystem::path &directory);

// ─── Lua hooks ───────────────────────────────────────────────────────────────
//
// A hooks file is a .lua script that returns a table with any of the following
// optional functions:
//
//   before_tool_call(ctx) → nil | {block=true, reason="..."}
//     ctx: {tool_name, call_id, args}
//
//   after_tool_call(ctx) → nil | {content="...", is_error=false}
//                               | {terminate=true}
//     ctx: {tool_name, call_id, args, content, is_error}
//
//   should_stop_after_turn(ctx) → bool
//     ctx: {message, tool_results=[{tool_name,content,is_error},...]}

struct LuaHooks {
  std::function<std::optional<BeforeToolCallResult>(
      const BeforeToolCallContext &, std::stop_token)>
      before_tool_call;

  std::function<std::optional<AfterToolCallResult>(
      const AfterToolCallContext &, std::stop_token)>
      after_tool_call;

  std::function<bool(const Message &,
                     const std::vector<ToolResultMessage> &,
                     const AgentContext &)>
      should_stop_after_turn;

  // Result returned by on_command.
  struct CommandResult {
    bool handled{false};
    // If set: truncate the transcript to this many messages (1-based, matches
    // the index field in the transcript table passed to on_command).
    std::optional<std::size_t> truncate_to;
    // If set: send this as the next prompt after applying truncate_to.
    std::optional<std::string> prompt;
  };

  // ── Sub-agent support ──────────────────────────────────────────────

  struct AgentRunConfig {
    std::string prompt;              // text to send to the sub-agent
    std::size_t fork_at{0};         // 0 = empty history; N = copy parent's first N messages
    std::optional<std::string> system_prompt; // override system prompt
    std::optional<std::string> model_id;      // override model id only
    std::vector<std::string> tools;           // empty = inherit all from parent
  };

  struct AgentRunResult {
    std::string text;                // concatenated final assistant text
    std::optional<std::string> error;
  };

  using RunAgentFn = std::function<AgentRunResult(const AgentRunConfig &)>;

  // Call this after the parent agent is constructed to enable pici.run_agent()
  // in Lua. Injects the factory into the live Lua state.
  std::function<void(RunAgentFn)> set_run_agent;

  // Called when the user types a slash command (/word ...) in the REPL.
  // transcript is the full message history as a Lua array (role, content,
  // index, turn fields).  Return nil/{handled=false} to fall through to the
  // agent; return {handled=true, ...} to consume the command.
  //
  // Lua signature:
  //   on_command(cmd, args, transcript) → nil | {handled, truncate_to, prompt}
  std::function<CommandResult(std::string_view cmd, std::string_view args,
                              const std::vector<Message> &transcript)>
      on_command;
};

// Load hooks from a Lua file. Only functions present in the returned table are
// wired up; missing hooks are left as null std::functions.
// Throws std::runtime_error on load/syntax errors.
std::shared_ptr<LuaHooks>
load_lua_hooks(const std::filesystem::path &path);

} // namespace pi::core
