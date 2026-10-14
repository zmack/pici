#pragma once

#include "cli/args.h"
#include "core/agent_task.h"
#include "core/mcp/mcp_config.h"
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
using McpServerConfig = core::mcp::McpServerConfig;

struct MailboxSettings {
  bool enabled{false};
  std::filesystem::path path;
  std::string scope{"workspace"};
  std::int64_t heartbeat_interval_ms{2000};
  std::int64_t stale_after_ms{10000};
  std::int64_t poll_interval_ms{250};
  std::int64_t claim_lease_ms{30000};
  std::int64_t retention_days{30};
};

// User-overridable subset of core::TaskTree::Limits (agent_task.h) -- how
// much subagent concurrency/nesting a session allows. Every field is
// unset by default, meaning "keep TaskTree::Limits{}'s own built-in
// default"; see build_task_tree_limits() in session_runtime_bundle.cpp,
// the one place these are turned into a real core::TaskTree::Limits.
// Deliberately does not expose max_mailbox_items/max_message_bytes/
// max_output_bytes/max_context_bytes -- those are safety/memory bounds,
// not a "how much parallelism do I want" knob.
struct SubagentLimitsConfig {
  std::optional<std::size_t> max_active_executions;
  std::optional<std::size_t> max_resident_tasks;
  std::optional<std::size_t> max_nesting_depth;
  std::optional<std::size_t> max_direct_children;
  std::optional<std::size_t> max_wait_seconds;
  // Opt-in attributed delivery of child completions to the parent session.
  std::optional<bool> deliver_completions;
};

struct Config {
  Args defaults;
  MailboxSettings mailbox;
  std::map<std::string, ProviderConfig> providers;
  std::vector<McpServerConfig> mcp_servers;
  SubagentLimitsConfig subagents;
  std::vector<Args::Diagnostic> diagnostics;

  bool has_errors() const;
};

// Returns the default config file path: $XDG_CONFIG_HOME/pici/config.toml
// or ~/.config/pici/config.toml if XDG_CONFIG_HOME is unset.
std::filesystem::path default_config_path();
std::filesystem::path
default_mailbox_path(const std::filesystem::path &config_path);

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

// Starts from core::TaskTree::Limits{}'s own built-in defaults and
// overrides only the fields the user actually set in [subagents]
// (config.toml, reached via args.config_document) -- a null
// config_document, or an absent/empty [subagents] table, reproduces
// today's exact behavior. See SubagentLimitsConfig's own comment above for
// why max_mailbox_items/max_message_bytes/max_output_bytes/
// max_context_bytes are deliberately not user-overridable here.
core::TaskTree::Limits build_task_tree_limits(const Args &args);

// Load config from the appropriate path (respects --config and env vars),
// then merge with the parsed CLI args. Call instead of parse_args when
// you want the full config+CLI pipeline.
Args load_and_merge(int argc, char *argv[]);

} // namespace pi::cli
