#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/message_types.h"
#include "core/sandbox.h"
#include "core/skills.h"

namespace pi::core {

// Structured metadata about a truncate_head_ex() call -- see the doc
// comment on truncate_head_ex's definition in builtin_tools.cpp for field
// semantics.
struct TruncationInfo {
  bool truncated = false;
  std::optional<std::string> truncated_by; // "lines" or "bytes"
  std::size_t input_lines = 0, input_bytes = 0;
  std::size_t output_lines = 0, output_bytes = 0;
  bool last_line_partial = false;
  bool first_line_exceeds_limit = false;
  std::size_t max_lines = 0, max_bytes = 0;
};

struct TruncatedContent {
  std::string content;
  TruncationInfo info;
};

// Truncates `content` to at most `max_lines` lines and `max_bytes` bytes
// (whichever limit is hit first), appending a "[Truncated: ...]" marker
// noting which limit fired. Used by BashTool and other builtins to cap
// output before it enters the transcript; McpTool reuses it verbatim to cap
// MCP tools/call results the same way.
TruncatedContent truncate_head_ex(const std::string &content,
                                 std::size_t max_lines, std::size_t max_bytes);

// Creates the built-in tool sets. When |skills| is provided and non-empty, a
// read-only `skill` tool is appended (plans/agent-skills.md §5).
std::vector<std::shared_ptr<const ToolDefinition>> create_coding_tools(
    const std::filesystem::path &cwd = std::filesystem::current_path(),
    SandboxPolicyPtr sandbox_policy = {},
    std::shared_ptr<const SkillCatalog> skills = nullptr);

std::vector<std::shared_ptr<const ToolDefinition>> create_read_only_tools(
    const std::filesystem::path &cwd = std::filesystem::current_path(),
    std::shared_ptr<const SkillCatalog> skills = nullptr);

std::vector<std::shared_ptr<const ToolDefinition>> create_all_tools(
    const std::filesystem::path &cwd = std::filesystem::current_path(),
    SandboxPolicyPtr sandbox_policy = {},
    std::shared_ptr<const SkillCatalog> skills = nullptr);

} // namespace pi::core
