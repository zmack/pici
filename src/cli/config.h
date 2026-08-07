#pragma once

#include "cli/args.h"
#include "core/models.h"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pi::cli {

using ProviderAuthPolicy = core::ProviderAuthPolicy;
using ApiKeyConfig = core::ApiKeyConfig;
using ConfiguredCost = core::ConfiguredCost;
using ConfiguredModel = core::ConfiguredModel;
using ProviderConfig = core::ProviderConfig;

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
