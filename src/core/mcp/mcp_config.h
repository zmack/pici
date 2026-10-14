#pragma once

// McpServerConfig: the canonical, core-side description of one configured
// MCP server (mirrors how ProviderConfig lives core-side in core/models.h
// and is aliased, not redefined, by cli::Config -- see cli/config.h). Kept
// under core/mcp/ rather than models.h since it's specific to this
// subsystem, not a model/provider concept.

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace pi::core::mcp {

struct McpServerConfig {
  // Required: the server's name, used both as a config key (duplicate
  // detection is case-insensitive, mirroring providers) and as the
  // "<server>__<tool>" qualification prefix for every tool it advertises
  // (see mcp_tool.h's sanitize_mcp_tool_name).
  std::string name;

  // Stdio transport: the command to spawn (resolved against PATH the same
  // way McpStdioTransport::spawn already does -- see mcp_transport.cpp).
  // Exactly one of `command` and `url` is set (config parsing enforces it).
  std::string command;

  // Streamable-HTTP transport: the MCP endpoint (http:// or https://,
  // no userinfo). When set, args/env/cwd are unused.
  std::string url;

  // Extra non-secret request headers for the HTTP transport. Authentication-
  // owned headers (Authorization, Cookie, ...) are rejected at parse time.
  std::map<std::string, std::string> headers;

  // Name of the environment variable holding the bearer token for the HTTP
  // transport. Only the NAME lives in config; the value is read when the
  // transport is built and held only inside McpHttpTransport -- it never
  // enters the config, tool catalog, transcripts or logs.
  std::string bearer_token_env;

  std::vector<std::string> args;

  // Env policy (must match McpServerProcessConfig's existing Phase 1
  // behavior, not introduce a second one): the child inherits pici's own
  // environment, and these entries are overlaid on top (present keys
  // override, new keys are added). Values here are literal strings -- no
  // ApiKeyConfig::api_key_env-style indirection is supported for this
  // field; an MCP server's env table is not a credentials mechanism the
  // way ProviderConfig::api_key is; users who need a secret here can
  // already reference an env var pici itself inherited (since the child
  // inherits pici's environment first) without pici ever seeing the value.
  std::map<std::string, std::string> env;

  // Empty/unset means "inherit pici's own current working directory"
  // (McpStdioTransport already treats std::nullopt this way).
  std::optional<std::string> cwd;

  // Lets a user keep a server's config block around but temporarily turn it
  // off without deleting it. Defaults to true (enabled).
  bool enabled{true};

  // Declared now so Phase 3's per-call timeout doesn't need another
  // config-schema change; unused by Phase 2's discovery/call-mapping code,
  // which uses its own fixed timeouts.
  std::optional<std::int64_t> call_timeout_ms;
};

} // namespace pi::core::mcp
