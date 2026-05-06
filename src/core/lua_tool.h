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
};

// Load hooks from a Lua file. Only functions present in the returned table are
// wired up; missing hooks are left as null std::functions.
// Throws std::runtime_error on load/syntax errors.
std::shared_ptr<LuaHooks>
load_lua_hooks(const std::filesystem::path &path);

} // namespace pi::core
