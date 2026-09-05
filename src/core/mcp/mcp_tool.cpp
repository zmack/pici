#include "core/mcp/mcp_tool.h"

#include <cstdio>
#include <functional>
#include <iostream>
#include <utility>

namespace pi::core::mcp {

namespace {

// Matches builtin_tools.cpp's ReadTool/GrepTool/etc. size caps (kMaxBytes,
// kReadMaxLines) -- MCP results aren't otherwise bounded anywhere, and
// shouldn't go into the transcript/next provider request unbounded.
constexpr std::size_t kMaxResultLines = 2000;
constexpr std::size_t kMaxResultBytes = 64 * 1024;

constexpr std::size_t kMaxSanitizedNameLength = 64;

bool is_name_safe_char(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '_' || c == '-';
}

class McpToolResult : public ToolResult {
public:
  McpToolResult(std::string content, bool is_error,
                std::optional<std::string> details = std::nullopt)
      : content_(std::move(content)), details_(std::move(details)),
        is_error_(is_error) {}

  bool is_error() const override { return is_error_; }
  std::string content() const override { return content_; }
  std::optional<std::string> details() const override { return details_; }

private:
  std::string content_;
  std::optional<std::string> details_;
  bool is_error_{false};
};

// Builds the details() payload for a failed call: the server's captured
// stderr tail (if any), as a small JSON object -- never attached on the
// success path, never printed to pici's own stderr directly (see
// McpStdioTransport::recent_stderr()'s doc comment).
std::optional<std::string> stderr_details(const McpClient &client) {
  auto tail = client.transport().recent_stderr();
  if (tail.empty())
    return std::nullopt;
  return nlohmann::json{{"stderr_tail", std::move(tail)}}.dump();
}

std::shared_ptr<ToolResult>
error_result(std::string message,
             std::optional<std::string> details = std::nullopt) {
  return std::make_shared<McpToolResult>(std::move(message), true,
                                         std::move(details));
}

} // namespace

std::string sanitize_mcp_tool_name(std::string_view server_name,
                                   std::string_view tool_name) {
  std::string qualified;
  qualified.reserve(server_name.size() + tool_name.size() + 2);
  qualified += server_name;
  qualified += "__";
  qualified += tool_name;

  std::string sanitized;
  sanitized.reserve(qualified.size());
  for (char c : qualified)
    sanitized.push_back(is_name_safe_char(c) ? c : '_');

  if (sanitized.size() <= kMaxSanitizedNameLength)
    return sanitized;

  // Truncate and append a short hash suffix of the FULL untruncated
  // qualified name (not the sanitized-but-untruncated one -- doesn't
  // matter which, since sanitization is a deterministic per-character
  // map, but hashing the original is marginally cheaper to reason about)
  // so two names that would otherwise truncate to the same prefix stay
  // distinct.
  const std::size_t hash_value = std::hash<std::string>{}(qualified);
  char suffix[8];
  std::snprintf(suffix, sizeof(suffix), "_%06zx",
                static_cast<std::size_t>(hash_value & 0xFFFFFFULL));
  constexpr std::size_t kSuffixLength = 7; // '_' + 6 hex chars
  std::string truncated =
      sanitized.substr(0, kMaxSanitizedNameLength - kSuffixLength);
  truncated += suffix;
  return truncated;
}

std::vector<std::string> builtin_tool_names() {
  // Reuses the real builtin tool set (rather than a hand-maintained literal
  // list) so this can't silently drift out of sync as builtins are
  // added/renamed. Defaults (cwd/sandbox_policy/skills) are irrelevant here
  // -- only the resulting names are used.
  std::vector<std::string> names;
  for (const auto &tool : create_all_tools())
    names.emplace_back(tool->name());
  return names;
}

bool claim_tool_name(const std::string &name, std::string_view context,
                     std::unordered_set<std::string> &taken_names) {
  if (!taken_names.insert(name).second) {
    std::cerr << "mcp: server \"" << context << "\" tool name \"" << name
              << "\" collides with a builtin or another already-registered "
                 "tool name; skipping this tool\n";
    return false;
  }
  return true;
}

std::map<std::string, std::string> McpToolSchema::to_definition() const {
  auto parsed = nlohmann::json::parse(serialized_, nullptr, false);
  std::map<std::string, std::string> result;
  if (parsed.is_discarded() || !parsed.is_object()) {
    result["type"] = "object";
    return result;
  }
  for (auto it = parsed.begin(); it != parsed.end(); ++it) {
    if (it.value().is_string())
      result[it.key()] = it.value().get<std::string>();
    else
      result[it.key()] = it.value().dump();
  }
  return result;
}

McpTool::McpTool(std::string server_name, std::string tool_name,
                 std::string description, nlohmann::json input_schema,
                 std::shared_ptr<McpClient> client)
    : server_name_(std::move(server_name)), tool_name_(std::move(tool_name)),
      description_(std::move(description)),
      qualified_name_(sanitize_mcp_tool_name(server_name_, tool_name_)),
      source_path_("mcp:" + server_name_), schema_(std::move(input_schema)),
      client_(std::move(client)) {}

std::shared_ptr<ToolResult>
McpTool::execute(std::string_view call_id, std::string_view args_json,
                 std::stop_token stop_tok, ToolUpdateCallback on_update) const {
  // No streaming in MCP's base protocol: on_update is never invoked.
  // call_id is unused: McpClient mints its own JSON-RPC request id
  // independently of pici's tool-call id.
  (void)call_id;
  (void)on_update;

  // "Mark unavailable, no auto-reconnect" (see McpClient::is_alive()'s doc
  // comment): if the transport is already known dead, fail immediately with
  // a clear message instead of attempting a write at all. This is belt-and-
  // suspenders on top of the transport's own EPIPE/reader-death handling,
  // which already makes a dead-transport write safe -- this just avoids the
  // pointless attempt and gives a clearer error than a generic "write
  // failed".
  if (!client_->is_alive()) {
    return error_result("MCP server \"" + server_name_ +
                            "\" is no longer running",
                        stderr_details(*client_));
  }

  auto args = nlohmann::json::parse(args_json, nullptr, false);
  if (args.is_discarded()) {
    return error_result("invalid JSON arguments for mcp tool \"" + tool_name_ +
                        "\"");
  }

  nlohmann::json result;
  try {
    // The ORIGINAL, unsanitized tool name goes over the wire -- the server
    // has never heard of pici's "<server>__<tool>" qualification scheme.
    // `stop_tok` races the wait against cooperative cancellation (see
    // McpStdioTransport::call()'s stop_token overload) -- if the caller
    // stops it mid-call, this returns promptly with a cancellation error
    // instead of waiting out the full per-call timeout.
    result = client_->call_tool(tool_name_, std::move(args),
                                client_->effective_call_timeout(), stop_tok);
  } catch (const std::exception &err) {
    return error_result(std::string("mcp tool call failed: ") + err.what(),
                        stderr_details(*client_));
  }

  const bool is_error = result.is_object() && result.value("isError", false);

  std::string joined;
  if (result.is_object() && result.contains("content") &&
      result.at("content").is_array()) {
    bool first = true;
    for (const auto &block : result.at("content")) {
      if (!block.is_object())
        continue;
      // Only "text" content blocks are mapped for v1 -- "image"/"resource"
      // blocks are dropped rather than mapped into content_blocks(), per
      // the plan's explicit "for v1" scope note.
      if (block.value("type", std::string{}) != "text")
        continue;
      if (!block.contains("text") || !block.at("text").is_string())
        continue;
      if (!first)
        joined += "\n";
      joined += block.at("text").get<std::string>();
      first = false;
    }
  }

  auto truncated = truncate_head_ex(joined, kMaxResultLines, kMaxResultBytes);
  return std::make_shared<McpToolResult>(std::move(truncated.content),
                                         is_error);
}

} // namespace pi::core::mcp
