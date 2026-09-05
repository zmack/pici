#pragma once

// McpTool: adapts one MCP server's advertised tool (name/description/
// inputSchema, discovered via McpClient::list_tools()) into pi::core's
// ToolDefinition interface, so it can sit in the same tool vector as every
// builtin/Lua tool and be dispatched by agent_loop.cpp's ordinary
// find_tool()/execute() path.
//
// Two review-driven decisions baked in here, not left implicit:
//   - execution_mode() stays the ToolDefinition default (parallel). An
//     earlier draft overrode it to `sequential`, but that flag is a
//     BATCH-WIDE de-parallelizer (agent_loop.cpp's is_sequential_tool is
//     checked with std::ranges::any_of across the whole tool-call batch) --
//     one MCP call in a turn would have serialized every unrelated builtin
//     call alongside it, without even addressing the real concurrency risk
//     (multiple ACP sessions/threads sharing one McpClient). The transport
//     itself (mcp_transport.cpp) is already thread-safe by construction:
//     write_mutex_ serializes writers, and the promise-per-request-id
//     pending map makes concurrent in-flight calls from multiple threads
//     safe. See test_mcp_tool.cpp's ConcurrentCallsFromMultipleThreadsAreSafe.
//   - Schema validation happens at discovery time (McpClient::list_tools()),
//     not here. A malformed server-advertised inputSchema must never reach
//     agent_loop.cpp:268's unguarded `tool->schema().validate_arguments()`
//     call, which has no surrounding try/catch (unlike prepare_arguments
//     immediately above it) and would fail the entire turn, not just one
//     tool call.

#include "core/builtin_tools.h" // TruncatedContent/truncate_head_ex reuse
#include "core/mcp/mcp_client.h"
#include "core/message_types.h"

#include <cstddef>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace pi::core::mcp {

// Builds the `<server>__<tool>` qualified name, replaces every character
// outside [A-Za-z0-9_-] with '_', and caps the result at 64 characters --
// provider tool-name limits are typically ^[a-zA-Z0-9_-]{1,64}$, and an
// unsanitized/over-long name doesn't just fail that one tool, it gets the
// entire request rejected by the provider. When truncation is needed, a
// short hash suffix of the FULL untruncated qualified name is appended so
// two names that would otherwise truncate to the same prefix stay distinct.
std::string sanitize_mcp_tool_name(std::string_view server_name,
                                   std::string_view tool_name);

// Returns the current builtin (create_all_tools()) tool names, as strings.
// Used to seed the "don't shadow a builtin" collision check at MCP
// discovery time -- computed from the real tool set (rather than a
// hand-maintained hardcoded list) so it can't silently drift out of sync as
// builtins are added/renamed.
std::vector<std::string> builtin_tool_names();

// Claims `name` in `taken_names` (inserting it) and returns true, unless it
// is already present, in which case it logs a warning to std::cerr
// identifying `context` (e.g. an MCP server name) and returns false without
// inserting. Shared by McpClient::list_tools() (checking one server's tools
// against builtins + its own siblings) and PiciProcess's cross-server merge
// (checking each server's tools against every other already-merged server).
bool claim_tool_name(const std::string &name, std::string_view context,
                     std::unordered_set<std::string> &taken_names);

class McpToolSchema : public JsonSchemaToolSchema {
public:
  explicit McpToolSchema(nlohmann::json input_schema)
      : serialized_(input_schema.dump()) {}

  std::string serialize() const override { return serialized_; }

  // Mirrors StaticJsonSchema::to_definition() (builtin_tools.cpp) exactly:
  // flattens the schema's top-level keys into a string map, falling back to
  // {"type": "object"} if the stored JSON isn't a parseable object.
  std::map<std::string, std::string> to_definition() const override;

private:
  std::string serialized_;
};

class McpTool : public ToolDefinition {
public:
  McpTool(std::string server_name, std::string tool_name,
          std::string description, nlohmann::json input_schema,
          std::shared_ptr<McpClient> client);

  std::string_view name() const override { return qualified_name_; }
  std::string_view description() const override { return description_; }
  std::string_view source_path() const override { return source_path_; }
  ToolSchema &schema() const override { return schema_; }

  std::shared_ptr<ToolResult>
  execute(std::string_view call_id, std::string_view args_json,
          std::stop_token stop_tok = std::stop_token{},
          ToolUpdateCallback on_update = {}) const override;

  ToolCapabilities capabilities() const override {
    return {.child_safe = false};
  }

  // Deliberately NOT overridden to `sequential` -- see file comment above.
  ToolExecutionMode execution_mode() const override {
    return ToolExecutionMode::parallel;
  }

  // Exposed for tests: the original, unsanitized tool name as advertised by
  // the server (what actually goes over the wire in tools/call).
  const std::string &original_tool_name_for_testing() const {
    return tool_name_;
  }

private:
  std::string server_name_;
  std::string tool_name_; // original, unsanitized -- sent to the server
  std::string description_;
  std::string qualified_name_; // sanitized "<server>__<tool>", cached
  std::string source_path_;    // "mcp:<server_name>", cached
  mutable McpToolSchema schema_;
  std::shared_ptr<McpClient> client_;
};

} // namespace pi::core::mcp
