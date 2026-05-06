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

  // Runtime info injected once after the parent agent is constructed.
  // Enables pici.model(), pici.tools(), pici.cwd(), pici.storage, and
  // pici.run_agent() in Lua.
  struct AgentInfo {
    std::string model_id;
    std::string model_provider;
    std::string model_api;
    std::vector<std::string> tool_names;
    std::string cwd;
    // Where to persist pici.storage data. Empty = in-memory only.
    std::filesystem::path storage_path;
    RunAgentFn run_agent;
  };

  // Call once after the parent agent and tools are fully configured.
  std::function<void(const AgentInfo &)> configure;

  // Slash commands declared by this add-on.
  // pici uses these to complete command names automatically when the user
  // types /... with no space yet — no Lua needed for that case.
  struct Command {
    std::string name;        // e.g. "rewind"
    std::string description; // shown in help / completion list
    std::string args_hint;   // e.g. "<turn>", optional
  };
  std::vector<Command> commands;

  // Argument completion hook — called only when partial already contains a
  // space (command name is settled).  Return candidate strings for the args.
  // compose_hooks unions all add-ons' results.
  //
  // Lua signature:
  //   complete(partial, transcript) → nil | {string, ...}
  std::function<std::vector<std::string>(std::string_view partial,
                                         const std::vector<Message> &transcript)>
      complete;

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

// ─── Test runner ─────────────────────────────────────────────────────────────
//
// A test file calls pici.test.run(name, fn) to register tests. Inside fn:
//   pici.test.eq(a, b [,msg])  — assert equality
//   pici.test.ok(val [,msg])   — assert truthy
//   pici.test.fail([msg])      — unconditional failure
//
// Use pici.mock_run_agent(fn) to stub pici.run_agent before calling the
// add-on under test.  Use dofile("addon.lua") to load the add-on table.
//
// Example:
//   local addon = dofile("rewind.lua")
//   pici.test.run("rewind truncates", function()
//     local r = addon.on_command("rewind", "1", {...})
//     pici.test.eq(r.handled, true)
//     pici.test.eq(r.truncate_to, 2)
//   end)

struct TestResult {
  int passed{0};
  int failed{0};
  int total{0};
};

// Load and run a Lua test file. Prints PASS/FAIL for each pici.test.run()
// call. Throws std::runtime_error on load or syntax errors.
TestResult run_lua_test_file(const std::filesystem::path &path);

// Load hooks from a Lua file. Only functions present in the returned table are
// wired up; missing hooks are left as null std::functions.
// Throws std::runtime_error on load/syntax errors.
std::shared_ptr<LuaHooks>
load_lua_hooks(const std::filesystem::path &path);

// Load all .lua files from a directory as independent add-ons, then compose them.
// Files that fail to load are skipped with a warning to stderr.
// Returns nullptr if no files loaded successfully.
std::shared_ptr<LuaHooks>
load_lua_hooks_dir(const std::filesystem::path &directory);

// Merge multiple LuaHooks into one with these composition rules:
//   before_tool_call    — run all; first {block=true} short-circuits
//   after_tool_call     — run all; first non-null return wins
//   should_stop_after_turn — OR: stop if any returns true
//   on_command          — first {handled=true} wins
//   set_run_agent       — forwarded to all add-ons
// Null entries in the list are ignored.
std::shared_ptr<LuaHooks>
compose_hooks(std::vector<std::shared_ptr<LuaHooks>> hooks_list);

} // namespace pi::core
