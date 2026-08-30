// open_session_runtime()/activate_session_runtime() live in their own
// translation unit, separate from session_runtime.cpp's frontend-shared
// pieces (resolve_model_selection/build_agent_options/
// build_agent_session_config), because they're the only functions here
// that call cli::resolve_mailbox_launch_options() (the thin Args-dependent
// adapter left behind in cli/mailbox_runtime.h/.cpp after Phase 3
// relocated the Args-independent core::MailboxRuntime type itself into
// core/session/mailbox_runtime.h). Keeping them apart means pi-acp (which
// only needs the frontend-shared pieces; see cli/session_runtime.h's file
// comment on the capability-scope decision) doesn't have to link
// src/cli/mailbox_runtime.cpp just to satisfy the linker for code it
// never calls -- it still gets core::MailboxRuntime for free via pi-core.

#include "cli/session_runtime.h"

#include "cli/config.h"
#include "cli/mailbox_runtime.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <ranges>
#include <set>
#include <utility>

namespace pi::cli {

namespace {

std::vector<ContextFile> load_context_files() {
  namespace fs = std::filesystem;
  static constexpr std::array<std::string_view, 4> kCandidates = {
      "AGENTS.md", "AGENTS.MD", "CLAUDE.md", "CLAUDE.MD"};

  auto try_load = [&](const fs::path &dir) -> std::optional<ContextFile> {
    for (auto name : kCandidates) {
      fs::path p = dir / name;
      std::error_code ec;
      if (!fs::exists(p, ec))
        continue;
      std::ifstream f(p);
      if (!f)
        continue;
      std::string content((std::istreambuf_iterator<char>(f)),
                          std::istreambuf_iterator<char>());
      return ContextFile{.path = p.string(), .content = std::move(content)};
    }
    return std::nullopt;
  };

  std::vector<ContextFile> result;
  std::set<std::string> seen;

  // Walk up from cwd to root, collecting innermost-first then reversing.
  std::vector<ContextFile> ancestors;
  fs::path cur = fs::current_path();
  while (true) {
    if (auto cf = try_load(cur)) {
      if (seen.insert(cf->path).second)
        ancestors.push_back(std::move(*cf));
    }
    fs::path parent = cur.parent_path();
    if (parent == cur)
      break;
    cur = parent;
  }
  std::ranges::reverse(ancestors);
  result.insert(result.end(), ancestors.begin(), ancestors.end());
  return result;
}

} // namespace

SessionRuntimeBundle open_session_runtime(const SessionRuntimeConfig &config) {
  // Built into locals first, then assembled into one SessionRuntimeBundle
  // aggregate-initialization at the end.
  auto session_store = std::make_shared<core::SessionStore>(
      config.args.session_dir.empty()
          ? core::SessionStore::default_sessions_dir()
          : std::filesystem::path(config.args.session_dir));

  auto fail = [](std::string message) {
    return SessionRuntimeBundle{.error = std::move(message)};
  };

  std::optional<core::SessionRecord> loaded_session;
  if (config.args.session_continue) {
    auto id = session_store->latest_session_id();
    if (!id)
      return fail("no previous session found");
    loaded_session = session_store->load(*id);
  } else if (!config.args.session_resume.empty()) {
    auto matches = session_store->find_by_prefix(config.args.session_resume);
    if (matches.empty())
      return fail("no session matching \"" + config.args.session_resume + "\"");
    if (matches.size() > 1) {
      std::string detail = "ambiguous prefix \"" + config.args.session_resume +
                           "\" matches " + std::to_string(matches.size()) +
                           " sessions:";
      for (const auto &h : matches)
        detail += "\n  " + h.id + (h.name ? ("  " + *h.name) : "");
      return fail(std::move(detail));
    }
    loaded_session = session_store->load(matches[0].id);
  }

  core::SandboxMode sandbox_mode = core::SandboxMode::auto_mode;
  std::optional<std::string_view> sandbox_setting;
  if (loaded_session && !config.args.sandbox_mode_explicit &&
      loaded_session->header.sandbox_mode)
    sandbox_setting = *loaded_session->header.sandbox_mode;
  else if (!config.args.sandbox_mode.empty())
    sandbox_setting = config.args.sandbox_mode;
  if (sandbox_setting) {
    const auto parsed = core::sandbox_mode_from_string(*sandbox_setting);
    if (!parsed)
      return fail("invalid sandbox mode \"" + std::string(*sandbox_setting) +
                  "\"; valid: auto, required, disabled");
    sandbox_mode = *parsed;
  }
  auto sandbox_policy = std::make_shared<core::SandboxPolicy>(sandbox_mode);

  auto child_write_tools = core::AgentTaskManager::ChildWriteTools::none;
  if (config.args.agent_write_tools == "core")
    child_write_tools = core::AgentTaskManager::ChildWriteTools::core;
  else if (config.args.agent_write_tools == "all")
    child_write_tools = core::AgentTaskManager::ChildWriteTools::all;
  else if (!config.args.agent_write_tools.empty() &&
           config.args.agent_write_tools != "none")
    return fail("invalid agents.write_tools value \"" +
                config.args.agent_write_tools + "\"; valid: none, core, all");

  std::optional<std::string> warning;
  if (child_write_tools == core::AgentTaskManager::ChildWriteTools::all &&
      sandbox_mode == core::SandboxMode::disabled)
    warning = "agents.write_tools=all with sandboxing disabled; child "
              "agents get unrestricted bash access";

  std::vector<ContextFile> context_files;
  if (config.capabilities.enable_context_files &&
      !config.args.no_context_files) {
    context_files = load_context_files();
    for (const auto &cf : context_files) {
      if (config.args.verbose)
        std::cerr << "[context: " << cf.path << "]\n";
    }
  }

  std::shared_ptr<const core::SkillCatalog> skill_catalog;
  const core::SkillCatalog *skill_catalog_ptr = nullptr;
  if (config.capabilities.enable_skills && !config.args.no_skills) {
    const auto agent_dir =
        (config.args.config_path.empty()
             ? default_config_path()
             : std::filesystem::path(config.args.config_path))
            .parent_path();
    auto owned = std::make_shared<core::SkillCatalog>(
        core::discover_skills(std::filesystem::current_path(), agent_dir));
    for (const auto &diag : owned->diagnostics)
      std::cerr << "[skills: diagnostic] " << diag << "\n";
    if (!owned->skills.empty()) {
      skill_catalog_ptr = owned.get();
      skill_catalog = std::move(owned);
    }
  }

  std::shared_ptr<core::MailboxCoordinator> mailbox;
  if (config.capabilities.enable_mailbox && config.args.mailbox_enabled) {
    try {
      const auto launch = resolve_mailbox_launch_options(
          config.args, config.model, std::filesystem::current_path());
      mailbox = core::start_mailbox(launch);
      if (config.args.verbose) {
        const auto &options = launch.coordinator;
        std::cerr << "mailbox enabled: path=" << launch.resolved_path
                  << " workspace_id=" << options.store.workspace_id
                  << " workspace_path=" << launch.workspace_path
                  << " process_id=" << options.process_id
                  << " agent_id=" << options.root_agent_id
                  << " lease_ms=" << options.store.claim_lease_ms
                  << " poll_ms=" << options.poll_interval.count()
                  << " schema=1\n";
      }
    } catch (const core::MailboxError &error) {
      std::cerr << "mailbox disabled: "
                << core::mailbox_error_code_to_string(error.code()) << ": "
                << error.what() << "\n";
    } catch (const std::exception &error) {
      std::cerr << "mailbox disabled: " << error.what() << "\n";
    }
  }

  AgentOptionsConfig options_config{
      .args = config.args,
      .model = config.model,
      .model_registry = config.model_registry,
      .auth_resolver = config.auth_resolver,
      .diagnostics = config.diagnostics,
      .mailbox_active = static_cast<bool>(mailbox),
      .capabilities = config.capabilities,
  };
  auto options_result = build_agent_options(options_config);
  if (config.on_effective_context)
    options_result.options.on_effective_context = config.on_effective_context;

  auto session_config = build_agent_session_config(
      options_result.options, config.model_registry, /*tools=*/{},
      session_store, sandbox_policy, config.args, config.capabilities, mailbox);
  auto runtime =
      std::make_shared<core::SessionRuntime>(std::move(session_config));

  return SessionRuntimeBundle{
      .warning = std::move(warning),
      .session_store = std::move(session_store),
      .sandbox_policy = std::move(sandbox_policy),
      .loaded_session = std::move(loaded_session),
      .child_write_tools = child_write_tools,
      .context_files = std::move(context_files),
      .skill_catalog = std::move(skill_catalog),
      .skill_catalog_ptr = skill_catalog_ptr,
      .agent_options = std::move(options_result.options),
      .hook_runtime = std::move(options_result.hook_runtime),
      .hooks = std::move(options_result.hooks),
      .hooks_list_saved = std::move(options_result.hooks_list_saved),
      .runtime = std::move(runtime),
  };
}

void activate_session_runtime(
    SessionRuntimeBundle &bundle,
    core::AgentTaskEventCallback extra_task_event_callback,
    std::function<void()> wake_root) {
  bundle.runtime->activate(
      bundle.agent_options, core::AgentTaskManager::Limits{},
      bundle.child_write_tools, std::move(extra_task_event_callback),
      std::move(wake_root));
}

} // namespace pi::cli
