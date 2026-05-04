#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "core/message_types.h"

namespace pi::core {

std::vector<std::shared_ptr<const ToolDefinition>>
create_coding_tools(std::filesystem::path cwd = std::filesystem::current_path());

std::vector<std::shared_ptr<const ToolDefinition>>
create_read_only_tools(std::filesystem::path cwd =
                           std::filesystem::current_path());

std::vector<std::shared_ptr<const ToolDefinition>>
create_all_tools(std::filesystem::path cwd = std::filesystem::current_path());

} // namespace pi::core
