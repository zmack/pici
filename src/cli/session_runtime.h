#pragma once

// Shared construction for the CLI/JSONL-RPC and ACP frontends. Extracted
// from src/main.cpp's cmd_run() and src/acp/main.cpp per
// plans/session-runtime-migration.md Phase 2, to close the gap the
// architecture lexicon calls out: cmd_run() and ACP independently resolved
// models/auth and independently assembled Agent::Options/core::SessionRuntime::
// Config, with no shared, testable seam between them.
//
// Everything here defaults to CLI's current behavior (all capabilities
// on). ACP calls the same functions with SessionRuntimeCapabilities'
// enable_* flags off, so it keeps its current feature scope (no mailbox,
// no Lua hooks, no skill catalog, no context files, no auto-compaction)
// instead of silently gaining those over HTTP as a side effect of sharing
// construction code -- see Phase 2's capability-scope decision (option (a))
// in the plan doc.
//
// core::MailboxRuntime itself relocated out of cli:: in Phase 3 (it never
// depended on cli::Args). This file stays in pi::cli rather than moving to
// core:: alongside it: SessionRuntimeConfig/AgentOptionsConfig embed
// cli::Args by value throughout (not just in passing), and Args is a
// CLI-specific config type -- moving this header into core:: would make
// core:: depend on a frontend type, inverting the intended dependency
// direction (frontends depend on core, not the reverse). That's a real
// design change (extracting an Args-independent config shape core:: could
// own), not a relocation, so it's deliberately left for a later phase
// rather than folded into Phase 3's mailbox-only scope.

#include "cli/args.h"
#include "cli/system_prompt.h"
#include "core/agent.h"
#include "core/agent_task.h"
#include "core/auth/authentication.h"
#include "core/lua_tool.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/mailbox/mailbox_types.h"
#include "core/models.h"
#include "core/sandbox.h"
#include "core/session/agent_session.h"
#include "core/session/mailbox_runtime.h"
#include "core/session/session_record.h"
#include "core/session/session_store.h"
#include "core/skills.h"
#include "core/stream_diagnostics.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pi::cli {

// Runtime-visible hook state, hot-swappable (e.g. by a REPL hook-reload
// command) without rebuilding the Agent::Options callbacks that forward
// through it. Only populated when SessionRuntimeCapabilities::enable_hooks
// is set.
struct HookRuntime {
  std::mutex mutex;
  std::shared_ptr<core::LuaHooks> hooks;
};

// Which optional subsystems a caller wants wired in. All default to CLI's
// current behavior (on); ACP constructs this with the mailbox/hooks/
// skills/context-file/auto-compaction flags off to preserve its current,
// narrower feature scope.
struct SessionRuntimeCapabilities {
  bool enable_mailbox{true};
  bool enable_hooks{true};
  bool enable_skills{true};
  bool enable_context_files{true};
  bool enable_auto_compaction{true};
};

// Maps the CLI-facing thinking-level enum onto the kernel's. Shared
// because build_agent_options() needs it and cmd_run()'s "/model" REPL
// command also calls it directly when switching models mid-session.
core::ThinkingLevel to_core_thinking(ThinkingLevel t);

// Resolves the effective Model for a run from CLI/config inputs -- what
// --model/--provider/--base-url mean. This is the logic previously
// duplicated (with real behavioral drift) between cmd_run()'s
// resolve_model() and pi-acp's main(); see
// test/test_frontend_parity.cpp for the parity test against this function
// and its header comment for the two behavior differences this
// unification resolves in ACP's favor of matching CLI's existing,
// more-established behavior.
core::ModelResolution resolve_model_selection(
    const Args &args,
    const std::shared_ptr<const core::ModelCatalog> &registry);

struct AgentOptionsConfig {
  Args args;
  core::Model model;
  std::shared_ptr<const core::ModelCatalog> model_catalog;
  std::shared_ptr<pi::auth::Authentication> authentication;
  std::shared_ptr<core::StreamDiagnostics> diagnostics;
  // Whether a mailbox coordinator is active for this run. Only consulted
  // when capabilities.enable_hooks is set: it decides whether the bundled
  // mailbox.lua add-on auto-loads (matching cmd_run()'s prior
  // `allow_auto_mailbox && mailbox` check). Capabilities.enable_mailbox
  // being off implies this should be false too.
  bool mailbox_active{false};
  SessionRuntimeCapabilities capabilities;
};

struct AgentOptionsResult {
  core::Agent::Options options;
  // Null when capabilities.enable_hooks is false.
  std::shared_ptr<HookRuntime> hook_runtime;
  std::shared_ptr<core::LuaHooks> hooks;
  std::vector<std::shared_ptr<core::LuaHooks>> hooks_list_saved;
};

// Builds core::Agent::Options: model, registry, thinking level, verbose,
// diagnostics, and the get_auth/get_api_key callbacks bound to
// config.authentication. When capabilities.enable_hooks is set, also loads
// and composes Lua hooks (per config.args.hooks_files/hooks_dir and the
// bundled mailbox add-on) and wires the five hook-forwarding opts
// callbacks through a HookRuntime. When disabled, those five opts
// callbacks are left entirely unset -- matching ACP's current behavior
// exactly (no hook callbacks installed at all), not "hooks that no-op".
AgentOptionsResult build_agent_options(const AgentOptionsConfig &config);

// The hook-composition step build_agent_options() uses internally,
// exposed so a caller (cmd_run()'s REPL hook-reload command) can
// recompute the same composed set on demand without restarting. Updates
// hooks_list_saved the same way build_agent_options() does.
std::shared_ptr<core::LuaHooks>
reload_hooks(const Args &args, bool mailbox_active,
             std::vector<std::shared_ptr<core::LuaHooks>> &hooks_list_saved);

// Assembles the core::SessionRuntime::Config literal previously duplicated
// ad hoc at cmd_run()'s `AgentSession runtime(...)` call,
// src/acp/server.cpp's task_root construction, and
// src/acp/handlers.cpp's per-run session construction. auto_compaction is
// only populated from args when capabilities.enable_auto_compaction is
// set; otherwise it's left at SessionRuntime::AutoCompactionConfig{}'s
// disabled default, matching ACP's pre-existing behavior of never setting
// it explicitly. `mailbox` constructs the runtime's owned (unconnected
// until SessionRuntime::activate()) MailboxRuntime; leave it null for any
// capability-gated caller (ACP always does, per
// SessionRuntimeCapabilities::enable_mailbox).
core::SessionRuntime::Config build_agent_session_config(
    const core::Agent::Options &agent_options,
    std::shared_ptr<const core::ModelCatalog> model_catalog,
    std::vector<std::shared_ptr<const core::ToolDefinition>> tools,
    std::shared_ptr<core::SessionStore> session_store,
    core::SandboxPolicyPtr sandbox_policy, const Args &args,
    const SessionRuntimeCapabilities &capabilities,
    std::shared_ptr<core::MailboxCoordinator> mailbox = {});

// The full CLI-shaped runtime bundle: durable session store, resolved
// sandbox policy, an optionally-loaded/resumed session record, context
// files, a skill catalog, and the constructed core::SessionRuntime itself
// (which as of Phase 6 owns the Agent, AgentTaskManager, and mailbox
// attachment that used to be three separate bundle fields). Built by
// open_session_runtime(); activate_session_runtime() finishes it once the
// caller has registered tools and finalized the system prompt on the live
// agent (see that function's comment for why this is two calls, not one).
//
// Not (yet) consumed by ACP for its per-run sessions: those never call
// activate() (ACP has never supported subagent delegation or mailbox
// delivery -- SessionRuntimeCapabilities keeps both off), so ACP calls
// build_agent_options() and build_agent_session_config() directly and
// constructs a bare core::SessionRuntime rather than going through this
// CLI-shaped bundle (which also resolves CLI-only concerns like
// --resume/--continue and context-file/skill discovery that ACP doesn't
// have).
struct SessionRuntimeConfig {
  Args args;
  core::Model model;
  std::shared_ptr<const core::ModelCatalog> model_catalog;
  std::shared_ptr<pi::auth::Authentication> authentication;
  std::shared_ptr<core::StreamDiagnostics> diagnostics;
  // CLI-only instrumentation hook (e.g. cmd_run()'s "/context" command
  // caches the last effective context here): not part of
  // AgentOptionsConfig/build_agent_options() because it typically closes
  // over caller-local state by reference, and it must be set before
  // SessionRuntime construction -- unlike system_prompt, there is no
  // post-construction setter for it on the live Agent.
  std::function<void(const core::AgentContext &)> on_effective_context;
  SessionRuntimeCapabilities capabilities;
};

struct SessionRuntimeBundle {
  // Set on a validation failure (bad --sandbox string -- including one
  // stored in a resumed session's header, an unresolvable --resume
  // prefix, --continue with no previous session, or an invalid
  // agents.write_tools value). Callers must check this before touching
  // any other field: on error, session/session_store/etc. are left at
  // their default-constructed (empty/null) state, not a usable bundle.
  // Kept in-bundle rather than reported via a duplicate, caller-side
  // SessionStore probe so validation only reads the filesystem once and
  // can see the same loaded_session the rest of construction sees (the
  // agents.write_tools-vs-disabled-sandbox warning below needs the
  // *resolved* sandbox mode, which can come from a resumed session's
  // stored header, not just the raw --sandbox flag).
  std::optional<std::string> error;
  // Set alongside a successful resolution when agents.write_tools=all
  // combines with a resolved (possibly session-inherited) disabled
  // sandbox; cmd_run() prints it as a warning, not an error.
  std::optional<std::string> warning;

  std::shared_ptr<core::SessionStore> session_store;
  core::SandboxPolicyPtr sandbox_policy;
  std::optional<core::SessionRecord> loaded_session;
  core::AgentTaskManager::ChildWriteTools child_write_tools{
      core::AgentTaskManager::ChildWriteTools::none};
  std::vector<ContextFile> context_files;
  std::shared_ptr<const core::SkillCatalog> skill_catalog;
  const core::SkillCatalog *skill_catalog_ptr{nullptr};
  // Mutable: the caller (cmd_run()) updates agent_options.system_prompt
  // after tool registration determines the final tool list, then passes
  // this bundle to activate_session_runtime(), which uses the
  // now-finalized value when calling core::SessionRuntime::activate() (so
  // child agents inherit the final system prompt). This mirrors
  // cmd_run()'s pre-extraction behavior exactly: the live root Agent's
  // initial system prompt is set explicitly via
  // agent().state().set_system_prompt(...) (unaffected by this field),
  // while this field only feeds AgentTaskManager's child-agent options.
  core::Agent::Options agent_options;
  std::shared_ptr<HookRuntime> hook_runtime;
  std::shared_ptr<core::LuaHooks> hooks;
  std::vector<std::shared_ptr<core::LuaHooks>> hooks_list_saved;

  // Owns the Agent, AgentTaskManager, and mailbox attachment (Phase 6
  // folded what used to be three separate bundle fields --
  // unique_ptr<AgentSession>, MailboxRuntime, shared_ptr<AgentTaskManager>
  // -- into core::SessionRuntime itself; see that class's own
  // declaration-order comment for the construction/destruction ordering
  // constraint, which now lives inside it rather than here). Null only
  // when `error` is set.
  std::shared_ptr<core::SessionRuntime> runtime;
};

// Phase A: resolves the session store, loaded/resumed session, sandbox
// policy, child-write-tools policy, context files and skill catalog (when
// enabled), agent options (including hooks, when enabled), mailbox launch
// (when enabled), and constructs the SessionRuntime (with its mailbox
// runtime built but not yet connected). Does not call
// SessionRuntime::activate() -- that needs the finalized system prompt,
// which the caller only knows after registering tools; see
// activate_session_runtime().
//
// On a validation failure (bad --continue/--resume/--sandbox/
// agents.write_tools input), returns a bundle with only `error` set --
// check it before touching any other field. Success may still set
// `warning` (currently: agents.write_tools=all with a resolved disabled
// sandbox).
SessionRuntimeBundle open_session_runtime(const SessionRuntimeConfig &config);

// Phase B: called after the caller has registered tools on
// bundle.runtime->agent() and updated bundle.agent_options.system_prompt
// to its final value. Calls bundle.runtime->activate(), forwarding
// extra_task_event_callback (e.g. a REPL activity/wake bridge) and
// wake_root (e.g. the REPL's readline-wake notifier) through to it.
void activate_session_runtime(
    SessionRuntimeBundle &bundle,
    core::AgentTaskEventCallback extra_task_event_callback = {},
    std::function<void()> wake_root = {});

} // namespace pi::cli
