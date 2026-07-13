#include "cli/config.h"
#include "cli/args.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#define TOML_HEADER_ONLY 1
#define TOML_EXCEPTIONS 1
#include <toml++/toml.hpp>

namespace pi::cli {

// ─────────────────────────────────────────────────────────────

// expand_tilde() and default_config_path() are only ever called during CLI
// startup (arg/config parsing), before any worker threads exist, so the
// std::getenv() calls below never race with a concurrent setenv/putenv.
static std::string expand_tilde(std::string path) {
  if (path.empty() || path[0] != '~')
    return path;
  const char *home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe)
  if (home == nullptr)
    return path;
  return std::string(home) + path.substr(1);
}

std::filesystem::path default_config_path() {
  const char *xdg =
      std::getenv("XDG_CONFIG_HOME"); // NOLINT(concurrency-mt-unsafe)
  std::filesystem::path base;
  if ((xdg != nullptr) && xdg[0] != '\0') {
    base = xdg;
  } else {
    const char *home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe)
    base = (home != nullptr) ? std::filesystem::path(home) / ".config" : ".";
  }
  return base / "pici" / "config.toml";
}

// ─────────────────────────────────────────────────────────────

Args load_config(const std::filesystem::path &path) {
  Args cfg;

  std::ifstream f(path);
  if (!f)
    return cfg; // file absent → empty config (not an error)

  toml::table tbl;
  try {
    tbl = toml::parse(f, path.string());
  } catch (const toml::parse_error &e) {
    throw std::runtime_error(std::string("config parse error: ") + e.what());
  }

  auto str = [&](std::string_view section,
                 std::string_view key) -> std::string {
    if (auto *s = tbl[section][key].as_string())
      return s->get();
    return {};
  };
  auto boolean = [&](std::string_view section, std::string_view key) -> bool {
    if (auto *b = tbl[section][key].as_boolean())
      return b->get();
    return false;
  };
  auto str_array = [&](std::string_view section,
                       std::string_view key) -> std::vector<std::string> {
    std::vector<std::string> result;
    if (auto *arr = tbl[section][key].as_array()) {
      for (const auto &v : *arr)
        if (const auto *s = v.as_string())
          result.push_back(expand_tilde(s->get()));
    }
    return result;
  };

  // [model]
  cfg.model = str("model", "id");
  cfg.provider = str("model", "provider");
  cfg.base_url = str("model", "base_url");
  cfg.api_key = str("model", "api_key");

  // [agent]
  cfg.system_prompt = str("agent", "system_prompt");
  cfg.append_system_prompts = str_array("agent", "append_system_prompt");
  if (auto ts = str("agent", "thinking"); !ts.empty()) {
    if (ts == "minimal")
      cfg.thinking = ThinkingLevel::minimal;
    else if (ts == "low")
      cfg.thinking = ThinkingLevel::low;
    else if (ts == "medium")
      cfg.thinking = ThinkingLevel::medium;
    else if (ts == "high")
      cfg.thinking = ThinkingLevel::high;
    else if (ts == "xhigh")
      cfg.thinking = ThinkingLevel::xhigh;
  }

  // [tools]
  cfg.no_tools = boolean("tools", "disabled");
  cfg.no_builtin_tools = boolean("tools", "no_builtin");
  cfg.tools = str_array("tools", "list");
  if (auto d = str("tools", "dir"); !d.empty())
    cfg.tools_dir = expand_tilde(d);

  // [addons]
  cfg.hooks_files = str_array("addons", "files");
  if (auto d = str("addons", "dir"); !d.empty())
    cfg.hooks_dir = expand_tilde(d);

  // [display]
  cfg.render = str("display", "render");
  cfg.verbose = boolean("display", "verbose");

  // [context]
  cfg.no_context_files = boolean("context", "disabled");

  // [session]
  if (auto d = str("session", "dir"); !d.empty())
    cfg.session_dir = expand_tilde(d);

  return cfg;
}

// ────────────────────────────────────────────────────────────────────

Args merge_args(const Args &config, const Args &cli) {
  Args out = config; // start with config defaults

  // String: CLI non-empty wins
  auto merge_str = [](const std::string &conf,
                      const std::string &c) -> std::string {
    return c.empty() ? conf : c;
  };

  // Vector: CLI non-empty wins; otherwise union of both (config first)
  auto merge_vec =
      [](const std::vector<std::string> &conf,
         const std::vector<std::string> &c) -> std::vector<std::string> {
    if (!c.empty())
      return c;
    return conf;
  };

  out.model = merge_str(config.model, cli.model);
  out.provider = merge_str(config.provider, cli.provider);
  out.base_url = merge_str(config.base_url, cli.base_url);
  out.api_key = merge_str(config.api_key, cli.api_key);

  out.system_prompt = merge_str(config.system_prompt, cli.system_prompt);
  // append_system_prompt: accumulate both
  out.append_system_prompts = config.append_system_prompts;
  for (const auto &s : cli.append_system_prompts)
    out.append_system_prompts.push_back(s);

  // thinking: CLI beats config if CLI is non-off (or if config is off)
  if (cli.thinking != ThinkingLevel::off)
    out.thinking = cli.thinking;

  out.no_tools = config.no_tools || cli.no_tools;
  out.no_builtin_tools = config.no_builtin_tools || cli.no_builtin_tools;
  out.tools = merge_vec(config.tools, cli.tools);
  out.tools_dir = merge_str(config.tools_dir, cli.tools_dir);

  // hooks_files: accumulate both (config first, then CLI)
  out.hooks_files = config.hooks_files;
  for (const auto &f : cli.hooks_files)
    out.hooks_files.push_back(f);
  out.hooks_dir = merge_str(config.hooks_dir, cli.hooks_dir);

  out.render = merge_str(config.render, cli.render);
  out.verbose = config.verbose || cli.verbose;
  out.no_context_files = config.no_context_files || cli.no_context_files;

  // Session: CLI --session-dir wins over config
  out.session_dir = merge_str(config.session_dir, cli.session_dir);
  // session_continue and session_resume are CLI-only
  out.session_continue = cli.session_continue;
  out.session_resume = cli.session_resume;

  // Pass-through CLI-only fields
  out.messages = cli.messages;
  out.print_mode = cli.print_mode;
  out.list_models = cli.list_models;
  out.list_models_filter = cli.list_models_filter;
  out.list_tools = cli.list_tools;
  out.list_addons = cli.list_addons;
  out.test_files = cli.test_files;
  out.help = cli.help;
  out.version = cli.version;
  out.config_path = cli.config_path;
  out.stream_trace = cli.stream_trace;
  out.diagnostics = cli.diagnostics;

  return out;
}

// ────────────────────────────────────────────────────────────

Args load_and_merge(int argc, char *argv[]) {
  // 1. Parse CLI first (we need --config path before loading the file)
  Args cli = parse_args(argc, argv);

  // 2. Determine config path
  std::filesystem::path cfg_path;
  if (!cli.config_path.empty()) {
    cfg_path = expand_tilde(cli.config_path);
  } else if (const char *env = std::getenv(
                 "PICI_CONFIG"); // NOLINT(concurrency-mt-unsafe) — called once
                                 // at CLI startup, before any worker threads
             (env != nullptr) && (env[0] != 0)) {
    cfg_path = expand_tilde(env);
  } else {
    cfg_path = default_config_path();
  }

  // 3. Load config (silently ignore missing file)
  Args config;
  try {
    config = load_config(cfg_path);
  } catch (const std::exception &e) {
    cli.diagnostics.push_back(
        {.is_error = false, .message = std::string("config: ") + e.what()});
  }

  // 4. Merge: CLI wins over config
  return merge_args(config, cli);
}

} // namespace pi::cli
