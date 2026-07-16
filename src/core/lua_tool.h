#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/agent_loop.h"
#include "core/message_types.h"

namespace pi::core {

// Immutable context views passed to Lua command hooks. `raw` is captured when
// the command is dispatched. `effective` is the most recent request-ready
// context, if a model request has been prepared for this session.
struct LuaContextSnapshot {
  AgentContext raw;
  std::optional<AgentContext> effective;
};

// Runtime values passed to UI hooks before each readline prompt.
struct LuaUiContext {
  std::size_t turn{0};
  std::string model;
  std::size_t tools{0};
  TokenUsage last;
  TokenUsage session;
  std::string session_id;
  std::optional<std::string> session_name;
};

// Load a single Lua tool from a .lua file.
// Throws std::runtime_error if the file cannot be loaded or is invalid.
std::shared_ptr<const ToolDefinition>
load_lua_tool(const std::filesystem::path &path);

// Load all .lua files from a directory as tools.
// Files that fail to load are silently skipped.
std::vector<std::shared_ptr<const ToolDefinition>>
load_lua_tools(const std::filesystem::path &directory);

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
//
//   on_event(event) → nil
//     Observes the canonical event envelope. This hook is observational;
//     use the targeted hooks above when a decision is required.
//
//   prepare_context(ctx) → nil | {messages={...}}
//     Returns a request-local message list after pruning or compaction.
//     ctx includes messages, model, tools, estimated_tokens, and
//     context_window.
//
//   status_line(ctx) → string | nil
//     Render a line above the readline prompt. ANSI color sequences are
//     supported; embedded newlines are not.
//
//   tab_title(ctx) → string | nil
//     Set the terminal tab/window title. nil leaves the current title alone.

struct LuaHooks {
  // Path of the file this hooks object was loaded from (empty for composed).
  std::string source_path;

  // Tools registered via pici.add_tool() inside this add-on.
  // compose_hooks unions these from all add-ons.
  std::vector<std::shared_ptr<const core::ToolDefinition>> registered_tools;

  std::function<std::optional<BeforeToolCallResult>(
      const BeforeToolCallContext &, std::stop_token)>
      before_tool_call;

  std::function<std::optional<AfterToolCallResult>(const AfterToolCallContext &,
                                                   std::stop_token)>
      after_tool_call;

  std::function<void(const AgentEvent &)> on_event;

  std::function<std::optional<std::vector<Message>>(
      const AgentContext &, std::size_t estimated_tokens, std::stop_token)>
      prepare_context;

  std::function<bool(const Message &, const std::vector<ToolResultMessage> &,
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
    // If set: display this directly through the active renderer without
    // sending it to the model.
    std::optional<std::string> output;
  };

  // ── Sub-agent support ──────────────────────────────────────────────

  struct AgentRunConfig {
    std::string prompt; // text to send to the sub-agent
    std::size_t fork_at{
        0}; // 0 = empty history; N = copy parent's first N messages
    std::optional<std::string> system_prompt; // override system prompt
    std::optional<std::string> model_id;      // override model id only
    std::vector<std::string> tools;           // empty = inherit all from parent
  };

  struct AgentRunResult {
    std::string text; // concatenated final assistant text
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
  // Prompt line hook — called before each REPL input to produce the prompt
  // string. Return a non-empty string to replace the default "> "; return
  // nullopt for default. compose_hooks: last non-nil result wins (later-loaded
  // add-ons override earlier ones).
  //
  // Lua signature:
  //   prompt_line(ctx) → string | nil
  //   ctx: {turn, model, tools, last={input,output,cache_read,cache_write,
  //     total_tokens,cost={input,output,cache_read,cache_write,total}},
  //     session={...}}
  std::function<std::optional<std::string>(
      std::size_t turn, std::string_view model_id, std::size_t tools_count,
      const TokenUsage &last_usage, const TokenUsage &session_usage)>
      prompt_line;

  // UI hooks — last non-nil result wins when add-ons are composed.
  std::function<std::optional<std::string>(const LuaUiContext &)> status_line;
  std::function<std::optional<std::string>(const LuaUiContext &)> tab_title;

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
  std::function<std::vector<std::string>(
      std::string_view partial, const std::vector<Message> &transcript)>
      complete;

  // Called when the user types a slash command (/word ...) in the REPL.
  // transcript is the legacy flattened message history. `context` is the
  // complete raw/effective context view described below. Return
  // nil/{handled=false} to fall through to the agent; return
  // {handled=true, ...} to consume the command.
  //
  // Lua signature:
  //   on_command(cmd, args, transcript, context)
  //     → nil | {handled, truncate_to, prompt, output}
  //   context = {
  //     raw = {system_prompt, messages, model, tools},
  //     effective = {available=false} or
  //       {available=true, system_prompt, messages, model, tools,
  //        provider, api},
  //   }
  //
  // Raw messages use the canonical message JSON shape, including every
  // content block and message metadata. Tool schemas are structured JSON
  // values under input_schema. These views may contain sensitive prompts,
  // reasoning data, and large base64 image payloads. They are snapshots and
  // cannot mutate agent state. `effective` is the last prepared context, not
  // the exact provider wire payload.
  std::function<CommandResult(std::string_view cmd, std::string_view args,
                              const std::vector<Message> &transcript,
                              const LuaContextSnapshot &context)>
      on_command;
};

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
std::shared_ptr<LuaHooks> load_lua_hooks(const std::filesystem::path &path);

// Load all .lua files from a directory as independent add-ons, then compose
// them. Files that fail to load are skipped with a warning to stderr. Returns
// nullptr if no files loaded successfully.
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
