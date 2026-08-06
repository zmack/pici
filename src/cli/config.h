#pragma once

#include "cli/args.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pi::cli {

enum class ProviderAuthPolicy { required, optional, none, oauth };

struct ApiKeyConfig {
  std::optional<std::string> literal;
  std::optional<std::string> env_var;
};

struct ConfiguredCost {
  std::optional<double> input_per_mtok;
  std::optional<double> output_per_mtok;
  std::optional<double> cache_read_per_mtok;
  std::optional<double> cache_write_per_mtok;
};

// Optional model fields are intentional. The same representation is used for
// custom models and sparse model_overrides; the registry supplies defaults and
// performs the merge against built-in metadata.
struct ConfiguredModel {
  // Required for entries in ProviderConfig::models. Empty for an override,
  // where the surrounding map key is the model ID.
  std::string id;
  std::optional<std::string> name;
  std::optional<std::string> api;
  std::optional<std::string> base_url;
  std::optional<bool> reasoning;
  std::optional<std::vector<std::string>> input_capabilities;
  std::optional<std::uint64_t> context_window;
  std::optional<std::uint64_t> max_tokens;
  std::map<std::string, std::string> headers;
  ConfiguredCost cost;
  std::optional<std::map<std::string, std::optional<std::string>>>
      thinking_level_map;
};

struct ProviderConfig {
  std::string id;
  std::optional<std::string> api;
  std::optional<std::string> base_url;
  ApiKeyConfig api_key;
  std::optional<ProviderAuthPolicy> auth;
  std::map<std::string, std::string> headers;
  std::vector<ConfiguredModel> models;
  std::map<std::string, ConfiguredModel> model_overrides;
};

struct Config {
  Args defaults;
  std::map<std::string, ProviderConfig> providers;
  std::vector<Args::Diagnostic> diagnostics;

  bool has_errors() const;
};

// Returns the default config file path: $XDG_CONFIG_HOME/pici/config.toml
// or ~/.config/pici/config.toml if XDG_CONFIG_HOME is unset.
std::filesystem::path default_config_path();

// Parse the legacy default fields from a TOML config file.
// Provider/model diagnostics are available from load_config_document().
Args load_config(const std::filesystem::path &path);

// Parse the complete TOML document, including provider and custom-model
// definitions. Parse errors still throw; recognized schema errors are
// returned as path-aware diagnostics so callers can report all problems.
Config load_config_document(const std::filesystem::path &path);

// Merge config defaults with CLI-provided values.
// CLI wins for every field that was explicitly set (non-empty string,
// non-empty vector, or true boolean). Config wins otherwise.
Args merge_args(const Args &config, const Args &cli);

// Load config from the appropriate path (respects --config and env vars),
// then merge with the parsed CLI args. Call instead of parse_args when
// you want the full config+CLI pipeline.
Args load_and_merge(int argc, char *argv[]);

} // namespace pi::cli
