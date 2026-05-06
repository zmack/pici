#pragma once

#include "cli/args.h"

#include <filesystem>

namespace pi::cli {

// Returns the default config file path: $XDG_CONFIG_HOME/pici/config.toml
// or ~/.config/pici/config.toml if XDG_CONFIG_HOME is unset.
std::filesystem::path default_config_path();

// Parse a TOML config file into an Args struct.
// Missing keys are left at their zero-value defaults.
// Unknown keys are silently ignored.
// Throws std::runtime_error on parse errors.
Args load_config(const std::filesystem::path &path);

// Merge config defaults with CLI-provided values.
// CLI wins for every field that was explicitly set (non-empty string,
// non-empty vector, or true boolean). Config wins otherwise.
Args merge_args(const Args &config, const Args &cli);

// Load config from the appropriate path (respects --config and env vars),
// then merge with the parsed CLI args. Call instead of parse_args when
// you want the full config+CLI pipeline.
Args load_and_merge(int argc, char *argv[]);

} // namespace pi::cli
