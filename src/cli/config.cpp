#include "cli/config.h"
#include "cli/args.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// toml++ requires these compile-time configuration macros before its header.
#define TOML_HEADER_ONLY 1 // NOLINT(cppcoreguidelines-macro-usage)
#define TOML_EXCEPTIONS 1  // NOLINT(cppcoreguidelines-macro-usage)
#include <toml++/toml.hpp> // NOLINT(misc-include-cleaner)

namespace pi::cli {

namespace {

// expand_tilde() and default_config_path() are only ever called during CLI
// startup (arg/config parsing), before any worker threads exist, so the
// std::getenv() calls below never race with a concurrent setenv/putenv.
std::string expand_tilde(std::string path) {
  if (path.empty() || path[0] != '~')
    return path;
  const char *home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe)
  if (home == nullptr)
    return path;
  return std::string(home) + path.substr(1);
}

} // namespace

namespace {

using Diagnostic = Args::Diagnostic;

void diagnostic(Config &config, bool is_error, std::string path,
                std::string message) {
  config.diagnostics.push_back(
      Diagnostic{.is_error = is_error,
                 .message = std::move(path) + ": " + std::move(message)});
}

std::string lower_ascii(std::string_view value) {
  std::string result(value);
  for (char &c : result)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

bool is_protected_header(std::string_view header) {
  const auto lower = lower_ascii(header);
  return lower == "authorization" || lower == "proxy-authorization" ||
         lower == "cookie" || lower == "set-cookie" || lower == "x-api-key";
}

std::string child_path(std::string_view parent, std::string_view child) {
  std::string result(parent);
  result += '.';
  result += child;
  return result;
}

const toml::node *get(const toml::table &table, std::string_view key) {
  return table.get(key);
} // NOLINT(misc-include-cleaner)

std::optional<std::string> read_string(const toml::table &table,
                                       std::string_view key,
                                       std::string_view path, Config &config) {
  const auto *node = get(table, key);
  if (node == nullptr)
    return std::nullopt;
  if (const auto *value = node->as_string())
    return value->get();
  diagnostic(config, true, std::string(path) + "." + std::string(key),
             "expected a string");
  return std::nullopt;
}

std::optional<bool> read_boolean(const toml::table &table, std::string_view key,
                                 std::string_view path, Config &config) {
  const auto *node = get(table, key);
  if (node == nullptr)
    return std::nullopt;
  if (const auto *value = node->as_boolean())
    return value->get();
  diagnostic(config, true, std::string(path) + "." + std::string(key),
             "expected a boolean");
  return std::nullopt;
}

std::optional<std::uint64_t> read_positive_integer(const toml::table &table,
                                                   std::string_view key,
                                                   std::string_view path,
                                                   Config &config) {
  const auto *node = get(table, key);
  if (node == nullptr)
    return std::nullopt;
  const auto *value = node->as_integer();
  const auto field_path = std::string(path) + "." + std::string(key);
  if (value == nullptr) {
    diagnostic(config, true, field_path, "expected a positive integer");
    return std::nullopt;
  }
  if (value->get() <= 0) {
    diagnostic(config, true, field_path, "must be greater than zero");
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(value->get());
}

std::optional<double> read_nonnegative_number(const toml::table &table,
                                              std::string_view key,
                                              std::string_view path,
                                              Config &config) {
  const auto *node = get(table, key);
  if (node == nullptr)
    return std::nullopt;

  double value = 0.0;
  if (const auto *integer = node->as_integer()) {
    value = static_cast<double>(integer->get());
  } else if (const auto *floating = node->as_floating_point()) {
    value = floating->get();
  } else {
    diagnostic(config, true, std::string(path) + "." + std::string(key),
               "expected a non-negative number");
    return std::nullopt;
  }

  const auto field_path = std::string(path) + "." + std::string(key);
  if (!std::isfinite(value) || value < 0.0) {
    diagnostic(config, true, field_path,
               "must be a finite non-negative number");
    return std::nullopt;
  }
  return value;
}

void parse_headers(const toml::table &table, std::string_view path,
                   std::map<std::string, std::string> &headers,
                   Config &config) {
  const auto *node = get(table, "headers");
  if (node == nullptr)
    return;
  const auto *header_table = node->as_table();
  const auto header_path = std::string(path) + ".headers";
  if (header_table == nullptr) {
    diagnostic(config, true, header_path, "expected a table of strings");
    return;
  }

  for (const auto &[key, value] : *header_table) {
    const auto name = std::string(key.str());
    const auto field_path = child_path(header_path, name);
    if (is_protected_header(name)) {
      diagnostic(config, true, field_path,
                 "authentication-owned header is not configurable");
      continue;
    }
    const auto *string_value = value.as_string();
    if (string_value == nullptr) {
      diagnostic(config, true, field_path, "expected a string");
      continue;
    }
    headers[name] = string_value->get();
  }
}

void parse_capabilities(const toml::table &table, std::string_view path,
                        ConfiguredModel &model, Config &config) {
  const auto *node = get(table, "input_capabilities");
  if (node == nullptr)
    return;
  const auto field_path = std::string(path) + ".input_capabilities";
  const auto *array = node->as_array();
  if (array == nullptr) {
    diagnostic(config, true, field_path, "expected an array of strings");
    return;
  }

  std::vector<std::string> capabilities;
  for (std::size_t index = 0; index < array->size(); ++index) {
    const auto &value = (*array)[index];
    const auto item_path = field_path + "[" + std::to_string(index) + "]";
    const auto *string_value = value.as_string();
    if (string_value == nullptr) {
      diagnostic(config, true, item_path, R"(expected "text" or "image")");
      continue;
    }
    const auto capability = string_value->get();
    if (capability != "text" && capability != "image") {
      diagnostic(config, true, item_path,
                 R"(unsupported capability; expected "text" or "image")");
      continue;
    }
    if (std::ranges::find(capabilities, capability) == capabilities.end())
      capabilities.push_back(capability);
  }
  if (capabilities.empty())
    diagnostic(config, true, field_path,
               "must contain at least one capability");
  model.input_capabilities = std::move(capabilities);
}

void parse_thinking_level_map(const toml::table &table, std::string_view path,
                              ConfiguredModel &model, Config &config) {
  const auto *node = get(table, "thinking_level_map");
  if (node == nullptr)
    return;
  const auto field_path = std::string(path) + ".thinking_level_map";
  const auto *map_table = node->as_table();
  if (map_table == nullptr) {
    diagnostic(config, true, field_path, "expected a table");
    return;
  }

  static constexpr std::array<std::string_view, 6> valid_levels = {
      "off", "minimal", "low", "medium", "high", "xhigh"};
  std::map<std::string, std::optional<std::string>> result;
  for (const auto &[key, value] : *map_table) {
    const auto level = std::string(key.str());
    const auto item_path = child_path(field_path, level);
    if (std::ranges::find(valid_levels, level) == valid_levels.end()) {
      diagnostic(config, true, item_path,
                 "unknown thinking level; expected off, minimal, low, medium, "
                 "high, or xhigh");
      continue;
    }
    if (const auto *string_value = value.as_string()) {
      if (string_value->get().empty()) {
        diagnostic(config, true, item_path, "mapped value must not be empty");
        continue;
      }
      result[level] = string_value->get();
    } else if (const auto *boolean_value = value.as_boolean()) {
      if (boolean_value->get()) {
        diagnostic(config, true, item_path,
                   "boolean thinking mappings must be false");
        continue;
      }
      result[level] = std::nullopt;
    } else {
      diagnostic(config, true, item_path, "expected a string or false");
    }
  }
  model.thinking_level_map = std::move(result);
  if (model.reasoning.has_value() && !*model.reasoning &&
      !model.thinking_level_map->empty()) {
    diagnostic(config, true, field_path,
               "cannot be set when reasoning is false");
  }
}

void parse_model_fields(const toml::table &table, std::string_view path,
                        ConfiguredModel &model, Config &config) {
  model.name = read_string(table, "name", path, config);
  model.api = read_string(table, "api", path, config);
  model.base_url = read_string(table, "base_url", path, config);
  model.reasoning = read_boolean(table, "reasoning", path, config);
  model.context_window =
      read_positive_integer(table, "context_window", path, config);
  model.max_tokens = read_positive_integer(table, "max_tokens", path, config);
  parse_capabilities(table, path, model, config);
  parse_headers(table, path, model.headers, config);

  if (const auto *cost_node = get(table, "cost")) {
    const auto *cost_table = cost_node->as_table();
    const auto cost_path = std::string(path) + ".cost";
    if (cost_table == nullptr) {
      diagnostic(config, true, cost_path, "expected a table");
    } else {
      model.cost.input_per_mtok = read_nonnegative_number(
          *cost_table, "input_per_mtok", cost_path, config);
      model.cost.output_per_mtok = read_nonnegative_number(
          *cost_table, "output_per_mtok", cost_path, config);
      model.cost.cache_read_per_mtok = read_nonnegative_number(
          *cost_table, "cache_read_per_mtok", cost_path, config);
      model.cost.cache_write_per_mtok = read_nonnegative_number(
          *cost_table, "cache_write_per_mtok", cost_path, config);
    }
  }
  parse_thinking_level_map(table, path, model, config);
}

ProviderAuthPolicy parse_auth_policy(std::string_view value,
                                     std::string_view path, Config &config) {
  if (value == "required")
    return ProviderAuthPolicy::required;
  if (value == "optional")
    return ProviderAuthPolicy::optional;
  if (value == "none")
    return ProviderAuthPolicy::none;
  if (value == "oauth")
    return ProviderAuthPolicy::oauth;
  diagnostic(config, true, std::string(path) + ".auth",
             "expected required, optional, none, or oauth");
  return ProviderAuthPolicy::required;
}

void parse_provider(const std::string &provider_id, const toml::table &table,
                    Config &config) {
  const auto path = "providers." + provider_id;
  ProviderConfig provider;
  provider.id = provider_id;
  provider.api = read_string(table, "api", path, config);
  provider.base_url = read_string(table, "base_url", path, config);
  provider.api_key.literal = read_string(table, "api_key", path, config);
  provider.api_key.env_var = read_string(table, "api_key_env", path, config);
  if (provider.api_key.literal && provider.api_key.env_var)
    diagnostic(config, true, path + ".api_key",
               "api_key and api_key_env are mutually exclusive");

  if (const auto auth = read_string(table, "auth", path, config))
    provider.auth = parse_auth_policy(*auth, path, config);
  parse_headers(table, path, provider.headers, config);

  if (provider_id == "openai-codex" &&
      (provider.api_key.literal || provider.api_key.env_var))
    diagnostic(
        config, true, path,
        "openai-codex accepts OAuth only; remove api_key or api_key_env");

  if (const auto *models_node = get(table, "models")) {
    const auto *models = models_node->as_array();
    const auto models_path = path + ".models";
    if (models == nullptr) {
      diagnostic(config, true, models_path, "expected an array of tables");
    } else {
      for (std::size_t index = 0; index < models->size(); ++index) {
        const auto item_path = models_path + "[" + std::to_string(index) + "]";
        const auto *model_table = (*models)[index].as_table();
        if (model_table == nullptr) {
          diagnostic(config, true, item_path, "expected a table");
          continue;
        }
        auto id = read_string(*model_table, "id", item_path, config);
        if (!id || id->empty()) {
          diagnostic(config, true, item_path,
                     "id is required and must not be empty");
          continue;
        }
        if (std::ranges::any_of(provider.models, [&](const auto &existing) {
              return existing.id == *id;
            })) {
          diagnostic(config, true, item_path,
                     "duplicate model id in provider " + provider_id);
          continue;
        }
        ConfiguredModel model;
        model.id = std::move(*id);
        parse_model_fields(*model_table, item_path, model, config);
        provider.models.push_back(std::move(model));
      }
    }
  }

  if (const auto *overrides_node = get(table, "model_overrides")) {
    const auto *overrides = overrides_node->as_table();
    const auto overrides_path = path + ".model_overrides";
    if (overrides == nullptr) {
      diagnostic(config, true, overrides_path, "expected a table of tables");
    } else {
      for (const auto &[key, value] : *overrides) {
        const auto id = std::string(key.str());
        const auto item_path = child_path(overrides_path, id);
        const auto *override_table = value.as_table();
        if (override_table == nullptr) {
          diagnostic(config, true, item_path, "expected a table");
          continue;
        }
        ConfiguredModel model;
        parse_model_fields(*override_table, item_path, model, config);
        provider.model_overrides.emplace(id, std::move(model));
      }
    }
  }

  config.providers.emplace(provider_id, std::move(provider));
}

} // namespace

std::filesystem::path default_config_path() {
  const char *xdg =
      std::getenv("XDG_CONFIG_HOME"); // NOLINT(concurrency-mt-unsafe)
  std::filesystem::path base;
  if ((xdg != nullptr) && *xdg != '\0') {
    base = xdg;
  } else {
    const char *home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe)
    base = (home != nullptr) ? std::filesystem::path(home) / ".config" : ".";
  }
  return base / "pici" / "config.toml";
}

static void parse_legacy_defaults(const toml::table &tbl, Args &cfg) {
  auto str = [&](std::string_view section,
                 std::string_view key) -> std::string {
    if (const auto *s = tbl[section][key].as_string())
      return s->get();
    return {};
  };
  auto boolean = [&](std::string_view section, std::string_view key) -> bool {
    if (const auto *b = tbl[section][key].as_boolean())
      return b->get();
    return false;
  };
  auto str_array = [&](std::string_view section,
                       std::string_view key) -> std::vector<std::string> {
    std::vector<std::string> result;
    if (const auto *arr = tbl[section][key].as_array()) {
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
}

bool Config::has_errors() const {
  return std::ranges::any_of(diagnostics,
                             [](const auto &item) { return item.is_error; });
}

Config load_config_document(const std::filesystem::path &path) {
  Config config;

  std::ifstream f(path);
  if (!f)
    return config; // file absent → empty config (not an error)

  toml::table tbl; // NOLINT(misc-include-cleaner)
  try {
    tbl = toml::parse(f, path.string()); // NOLINT(misc-include-cleaner)
  } catch (const toml::parse_error &e) { // NOLINT(misc-include-cleaner)
    throw std::runtime_error(std::string("config parse error: ") + e.what());
  }

  parse_legacy_defaults(tbl, config.defaults);

  if (const auto *providers_node = tbl.get("providers")) {
    const auto *providers = providers_node->as_table();
    if (providers == nullptr) {
      diagnostic(config, true, "providers", "expected a table");
    } else {
      for (const auto &[key, value] : *providers) {
        const auto provider_id = std::string(key.str());
        const auto path_name = "providers." + provider_id;
        const auto *provider = value.as_table();
        if (provider == nullptr) {
          diagnostic(config, true, path_name, "expected a table");
          continue;
        }
        if (provider_id.empty()) {
          diagnostic(config, true, path_name, "provider id must not be empty");
          continue;
        }
        parse_provider(provider_id, *provider, config);
      }
    }
  }

  return config;
}

Args load_config(const std::filesystem::path &path) {
  return load_config_document(path).defaults;
}

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
  out.auth_action = cli.auth_action;
  out.auth_provider = cli.auth_provider;
  out.auth_device = cli.auth_device;
  out.auth_browser = cli.auth_browser;
  out.diagnostics = cli.diagnostics;

  return out;
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
Args load_and_merge(int argc, char *argv[]) {
  // 1. Parse CLI first (we need --config path before loading the file)
  Args cli = parse_args(argc, argv);

  // 2. Determine config path
  std::filesystem::path cfg_path;
  if (!cli.config_path.empty()) {
    cfg_path = expand_tilde(cli.config_path);
  } else if (const char *env =
                 std::getenv("PICI_CONFIG"); // NOLINT(concurrency-mt-unsafe)
             (env != nullptr) && (*env != 0)) {
    cfg_path = expand_tilde(env);
  } else {
    cfg_path = default_config_path();
  }

  // 3. Load config (silently ignore missing file)
  Args config;
  std::shared_ptr<const Config> config_document;
  try {
    auto parsed = std::make_shared<Config>(load_config_document(cfg_path));
    config = parsed->defaults;
    config_document = std::move(parsed);
  } catch (const std::exception &e) {
    cli.diagnostics.push_back(
        {.is_error = false, .message = std::string("config: ") + e.what()});
  }

  // 4. Merge: CLI wins over config
  auto merged = merge_args(config, cli);
  merged.config_document = std::move(config_document);
  if (merged.config_document != nullptr) {
    for (const auto &diagnostic : merged.config_document->diagnostics)
      merged.diagnostics.push_back(diagnostic);
  }
  return merged;
}

} // namespace pi::cli
