#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "core/message_types.h"
#include "core/sandbox.h"
#include "core/skills.h"

namespace pi::core {

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
