// Deliberately does not construct cli::MailboxAttachment -- see
// session_runtime_bundle.cpp's file comment for why that lives in a
// separate translation unit.

#include "cli/session_runtime.h"

#include <filesystem>
#include <iostream>
#include <string_view>
#include <utility>

namespace pi::cli {

namespace {

std::filesystem::path bundled_mailbox_addon_path() {
  // The project currently has no install target; keep the bundled addon
  // next to its source tree until runtime resource packaging is
  // introduced.
  return std::filesystem::path(PI_CPP_SOURCE_DIR) / "addons" / "mailbox.lua";
}

std::shared_ptr<core::LuaHooks> load_composed_hooks(
    const Args &args, bool allow_auto_mailbox,
    std::vector<std::shared_ptr<core::LuaHooks>> &hooks_list_saved) {
  std::vector<std::shared_ptr<core::LuaHooks>> hooks_list;
  const auto bundled_mailbox = bundled_mailbox_addon_path();
  auto same_path = [](const std::filesystem::path &left,
                      const std::filesystem::path &right) {
    std::error_code left_error;
    std::error_code right_error;
    const auto left_canonical =
        std::filesystem::weakly_canonical(left, left_error);
    const auto right_canonical =
        std::filesystem::weakly_canonical(right, right_error);
    if (!left_error && !right_error)
      return left_canonical == right_canonical;
    return left.lexically_normal() == right.lexically_normal();
  };
  bool mailbox_addon_explicit = false;
  for (const auto &path : args.hooks_files) {
    const bool is_bundled_mailbox = same_path(path, bundled_mailbox);
    mailbox_addon_explicit |= is_bundled_mailbox;
    try {
      hooks_list.push_back(core::load_lua_hooks(path, is_bundled_mailbox));
      if (args.verbose)
        std::cerr << "[hooks: " << path << "]\n";
    } catch (const std::exception &e) {
      std::cerr << "warning: failed to load hooks file " << path << ": "
                << e.what() << "\n";
    }
  }
  if (!args.hooks_dir.empty()) {
    const bool bundled_mailbox_dir = same_path(
        std::filesystem::path(args.hooks_dir) / "mailbox.lua", bundled_mailbox);
    auto dir_hooks =
        core::load_lua_hooks_dir(args.hooks_dir, bundled_mailbox_dir);
    if (dir_hooks) {
      hooks_list.push_back(dir_hooks);
      if (args.verbose)
        std::cerr << "[hooks-dir: " << args.hooks_dir << "]\n";
    }
  }
  if (!args.hooks_dir.empty()) {
    std::error_code mailbox_entry_error;
    const auto mailbox_entry =
        std::filesystem::path(args.hooks_dir) / "mailbox.lua";
    mailbox_addon_explicit =
        std::filesystem::exists(mailbox_entry, mailbox_entry_error) ||
        mailbox_addon_explicit;
  }
  if (allow_auto_mailbox && !mailbox_addon_explicit) {
    try {
      hooks_list.push_back(core::load_lua_hooks(bundled_mailbox, true));
      if (args.verbose)
        std::cerr << "[hooks: " << bundled_mailbox << "]\n";
    } catch (const std::exception &e) {
      std::cerr << "warning: failed to load bundled mailbox addon: " << e.what()
                << "\n";
    }
  }

  hooks_list_saved = hooks_list;
  return core::compose_hooks(std::move(hooks_list));
}

} // namespace

std::shared_ptr<core::LuaHooks>
reload_hooks(const Args &args, bool mailbox_active,
             std::vector<std::shared_ptr<core::LuaHooks>> &hooks_list_saved) {
  auto load_hooks = [&](bool allow_auto_mailbox) {
    return load_composed_hooks(args, allow_auto_mailbox && mailbox_active,
                               hooks_list_saved);
  };
  if (args.faux_control_socket.empty())
    return load_hooks(true);
  if (args.hooks_files.empty() && args.hooks_dir.empty())
    return nullptr;
  return core::tool_formatter_hooks_only(load_hooks(false));
}

core::ThinkingLevel to_core_thinking(ThinkingLevel t) {
  switch (t) {
  case ThinkingLevel::off:
    return core::ThinkingLevel::off;
  case ThinkingLevel::minimal:
    return core::ThinkingLevel::minimal;
  case ThinkingLevel::low:
    return core::ThinkingLevel::low;
  case ThinkingLevel::medium:
    return core::ThinkingLevel::medium;
  case ThinkingLevel::high:
    return core::ThinkingLevel::high;
  case ThinkingLevel::xhigh:
    return core::ThinkingLevel::xhigh;
  }
  return core::ThinkingLevel::off;
}

core::ModelResolution resolve_model_selection(
    const Args &args,
    const std::shared_ptr<const core::ModelCatalog> &registry) {
  if (!args.model.empty()) {
    core::ModelSelection selection;
    if (!args.provider.empty())
      selection.provider = args.provider;
    selection.model = args.model;
    if (!args.base_url.empty())
      selection.base_url = args.base_url;
    selection.source = "cli";
    return registry->resolve(selection);
  }

  if (!args.provider.empty()) {
    if (const auto *provider = registry->provider(args.provider)) {
      core::Model model;
      model.id = "default";
      model.name = "default";
      model.api = provider->api;
      model.provider = provider->id;
      model.base_url = provider->base_url;
      model.input_capabilities = {"text"};
      model.context_window = 128000;
      model.max_tokens = 4096;
      if (!args.base_url.empty())
        model.base_url = args.base_url;
      return {.model = std::move(model)};
    }
    if (args.base_url.empty())
      return {.error = "cli: unknown provider '" + args.provider + "'"};
  }

  core::Model model;
  model.id = "default";
  model.name = "default";
  model.api = "openai-completions";
  model.provider = args.provider.empty() ? "local" : args.provider;
  model.base_url =
      args.base_url.empty() ? "http://127.0.0.1:8080/v1" : args.base_url;
  model.input_capabilities = {"text"};
  model.context_window = 128000;
  model.max_tokens = 4096;
  return {.model = std::move(model)};
}

AgentOptionsResult build_agent_options(const AgentOptionsConfig &config) {
  AgentOptionsResult result;
  core::Agent::Options &opts = result.options;
  opts.model = config.model;
  opts.model_catalog = config.model_catalog;
  opts.system_prompt = config.args.system_prompt;
  opts.thinking_level = to_core_thinking(config.args.thinking);
  opts.diagnostics = config.diagnostics;
  opts.verbose = config.args.verbose;

  auto authentication = config.authentication;
  if (config.model_catalog && authentication)
    config.model_catalog->set_request_auth_resolver(
        [authentication](std::string_view provider,
                         const std::stop_token &stop_token) {
          return authentication->resolve(provider, {}, stop_token);
        });
  opts.get_auth =
      [authentication](std::string_view p) -> std::optional<core::RequestAuth> {
    return authentication->resolve(p);
  };
  opts.get_api_key =
      [authentication](std::string_view p) -> std::optional<std::string> {
    auto auth = authentication->resolve(p);
    if (!auth || !auth->bearer_token)
      return std::nullopt;
    return auth->bearer_token;
  };

  if (!config.capabilities.enable_hooks)
    return result;

  auto hook_runtime = std::make_shared<HookRuntime>();
  auto hooks =
      reload_hooks(config.args, config.mailbox_active, result.hooks_list_saved);
  {
    std::scoped_lock lock(hook_runtime->mutex);
    hook_runtime->hooks = hooks;
  }
  result.hook_runtime = hook_runtime;
  result.hooks = hooks;

  opts.before_tool_call =
      [hook_runtime](const core::BeforeToolCallContext &context,
                     std::stop_token stop_tok)
      -> std::optional<core::BeforeToolCallResult> {
    std::shared_ptr<core::LuaHooks> current;
    {
      std::scoped_lock lock(hook_runtime->mutex);
      current = hook_runtime->hooks;
    }
    if (current && current->before_tool_call)
      return current->before_tool_call(context, std::move(stop_tok));
    return std::nullopt;
  };
  opts.after_tool_call =
      [hook_runtime](const core::AfterToolCallContext &context,
                     std::stop_token stop_tok)
      -> std::optional<core::AfterToolCallResult> {
    std::shared_ptr<core::LuaHooks> current;
    {
      std::scoped_lock lock(hook_runtime->mutex);
      current = hook_runtime->hooks;
    }
    if (current && current->after_tool_call)
      return current->after_tool_call(context, std::move(stop_tok));
    return std::nullopt;
  };
  opts.should_stop_after_turn =
      [hook_runtime](const core::Message &message,
                     const std::vector<core::ToolResultMessage> &results,
                     const core::AgentContext &context) {
        std::shared_ptr<core::LuaHooks> current;
        {
          std::scoped_lock lock(hook_runtime->mutex);
          current = hook_runtime->hooks;
        }
        return current && current->should_stop_after_turn
                   ? current->should_stop_after_turn(message, results, context)
                   : false;
      };
  opts.on_event = [hook_runtime](const core::AgentEvent &event) {
    std::shared_ptr<core::LuaHooks> current;
    {
      std::scoped_lock lock(hook_runtime->mutex);
      current = hook_runtime->hooks;
    }
    if (current && current->on_event)
      current->on_event(event);
  };
  opts.prepare_context = [hook_runtime](const core::AgentContext &context,
                                        std::size_t estimated_tokens,
                                        std::stop_token stop_tok)
      -> std::optional<std::vector<core::Message>> {
    std::shared_ptr<core::LuaHooks> current;
    {
      std::scoped_lock lock(hook_runtime->mutex);
      current = hook_runtime->hooks;
    }
    if (current && current->prepare_context)
      return current->prepare_context(context, estimated_tokens,
                                      std::move(stop_tok));
    return std::nullopt;
  };

  return result;
}

core::SessionRuntime::Config build_agent_session_config(
    const core::Agent::Options &agent_options,
    std::shared_ptr<const core::ModelCatalog> model_catalog,
    std::vector<std::shared_ptr<const core::ToolDefinition>> tools,
    std::shared_ptr<core::SessionStore> session_store,
    core::SandboxPolicyPtr sandbox_policy, const Args &args,
    const SessionRuntimeCapabilities &capabilities,
    std::shared_ptr<core::Mailbox> mailbox,
    std::shared_ptr<pi::auth::Authentication> authentication) {
  core::SessionRuntime::Config config{
      .agent_options = agent_options,
      .model_catalog = std::move(model_catalog),
      .tools = std::move(tools),
      .session_store = std::move(session_store),
      .sandbox_policy = std::move(sandbox_policy),
      .mailbox = std::move(mailbox),
  };
  // SessionRuntime::Config takes a narrow availability callback rather than
  // the concrete Authentication type (see that struct's comment): this
  // translation unit already links pi-http, so it's the right place to
  // close over the real object.
  if (authentication) {
    config.auth_availability = [authentication](std::string_view provider) {
      return authentication->availability(provider);
    };
  }
  if (capabilities.enable_auto_compaction) {
    config.auto_compaction = {.enabled = args.remote_compaction_enabled,
                              .threshold_pct =
                                  args.compaction_threshold_pct > 0.0
                                      ? args.compaction_threshold_pct
                                      : 0.85};
  }
  return config;
}

} // namespace pi::cli
