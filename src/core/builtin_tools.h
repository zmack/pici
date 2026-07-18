#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "core/message_types.h"
#include "core/sandbox.h"

namespace pi::core {

std::vector<std::shared_ptr<const ToolDefinition>> create_coding_tools(
    const std::filesystem::path &cwd = std::filesystem::current_path(),
    SandboxPolicyPtr sandbox_policy = {});

std::vector<std::shared_ptr<const ToolDefinition>> create_read_only_tools(
    const std::filesystem::path &cwd = std::filesystem::current_path());

std::vector<std::shared_ptr<const ToolDefinition>> create_all_tools(
    const std::filesystem::path &cwd = std::filesystem::current_path(),
    SandboxPolicyPtr sandbox_policy = {});

} // namespace pi::core
