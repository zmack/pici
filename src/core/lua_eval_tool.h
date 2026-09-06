#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/lua_tool.h"
#include "core/message_types.h"

namespace pi::core {

// Live, swappable view of the pici.* binding surface handed to the `lua`
// tool (plans/velvety-skipping-marble.md). configure_hooks() rebuilds
// LuaHooks::AgentInfo whenever the model, session, or add-on set changes;
// store() republishes it here so the next `lua` call sees current bindings
// instead of a stale capture taken once at tool-construction time.
//
// Archetype: value holder owned by the session that publishes into it; it
// owns no lifetime of its own and is never a state authority.
//
// This object outlives individual LuaEvalTool::execute() calls (it is
// shared across the whole session), which is also why pici.scratch --
// intentionally cross-call, session-length state -- lives here rather than
// on the throwaway per-call lua_State.
//
// Stage note (plans/velvety-skipping-marble.md, Stage 2): as of this stage
// nothing calls store() yet -- configure_hooks() wiring is Stage 4 -- so
// snapshot() always returns a default-constructed (configured=false)
// Snapshot in practice. The class is still built to its full §4.3 shape now
// because pici.scratch (§4.4) needs a session-length home regardless of
// whether the agents/session/mailbox bindings exist yet.
class LuaEvalBindings {
public:
  struct Snapshot {
    LuaHooks::AgentBindings agents;
    LuaHooks::SessionBindings session;
    LuaHooks::MailboxBindings mailbox;
    std::string model_id;
    std::string model_provider;
    std::string model_api;
    std::string cwd;
    std::vector<std::string> tool_names;
    bool configured{false};
  };

  // Republishes the current binding set. Called every time
  // configure_hooks() runs (initial setup, model switch, session
  // fork/activation, /reload-addons) -- see the plan's §4.3 for the
  // configure_hooks() early-return bug this depends on being fixed
  // (Stage 4).
  void store(const LuaHooks::AgentInfo &info);

  // Copies the current binding set under lock. A snapshot copies roughly
  // 21 std::functions plus a handful of strings -- irrelevant cost next to
  // actually spawning a subagent.
  Snapshot snapshot() const;

  // Defense-in-depth for session teardown (plan §3): drops the stored
  // bindings back to a default (configured=false) Snapshot so a `lua` call
  // that somehow outlives session teardown can't reach a half-destroyed
  // object through a stale std::function closure. The sequential,
  // per-call-state design means there should be no such call in practice;
  // this is one line of defense regardless.
  void clear();

  // pici.scratch -- cross-call persistence, scoped to this tool only (not
  // pici.storage, which is the add-on author's own on-disk file; see the
  // plan's §4.4). Bound by a total-size cap, a per-key size cap, and a
  // key-count cap, all separate from one another: a single pathological
  // key must not be able to consume the whole total-size budget on its
  // own, and a flood of tiny keys must not be able to grow unboundedly
  // just because each one individually stays under the per-key cap.
  //
  // Returns the stored value for `key`, or nullopt if absent.
  std::optional<nlohmann::json> scratch_get(const std::string &key) const;

  // Attempts to store `value` under `key`. `value` must already have been
  // produced through the depth-capped/cycle-guarded lua_to_json (not some
  // other marshaling path) -- see lua_tool.cpp's eval_scratch_set, which is
  // the only caller. Returns an error message on rejection (any cap
  // exceeded), nullopt on success.
  std::optional<std::string> scratch_set(const std::string &key,
                                         const nlohmann::json &value);

private:
  mutable std::mutex mutex_;
  Snapshot current_;

  // A scratch entry stores its own serialized byte size, computed once at
  // set() time, rather than re-deriving it via a fresh dump() on every
  // subsequent size check -- get() already pays a full marshal on every
  // read; a set() shouldn't also pay a redundant dump() just to check a
  // cap.
  struct ScratchEntry {
    nlohmann::json value;
    std::size_t cached_size{0};
  };

  mutable std::mutex scratch_mutex_;
  std::unordered_map<std::string, ScratchEntry> scratch_;
  std::size_t scratch_total_bytes_{0};
};

// Creates the `lua` built-in tool (plans/velvety-skipping-marble.md).
// Callers opt in explicitly (Args::lua_eval, Stage 4); make_lua_eval_tool is
// never called from create_all_tools()'s default set.
//
// `bindings` may be a freshly constructed, never-store()'d holder -- every
// pici.* binding then reports its own "not available" error cleanly rather
// than crashing (see push_agent_binding/push_mailbox_binding). Stage 2 does
// not wire any pici.agents/session/mailbox trampolines yet (that is
// Stage 3), so only pici.scratch, pici.log, and pici.truncate_tool_result
// are live regardless of what `bindings` holds.
std::shared_ptr<const ToolDefinition>
make_lua_eval_tool(std::shared_ptr<LuaEvalBindings> bindings);

} // namespace pi::core
