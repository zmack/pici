#pragma once

#include <filesystem>
#include <memory>
#include <vector>

#include "core/message_types.h"

namespace pi::core {

// Load a single Lua tool from a .lua file.
// Throws std::runtime_error if the file cannot be loaded or is invalid.
std::shared_ptr<const ToolDefinition>
load_lua_tool(const std::filesystem::path &path);

// Load all .lua files from a directory as tools.
// Files that fail to load are silently skipped.
std::vector<std::shared_ptr<const ToolDefinition>>
load_lua_tools(const std::filesystem::path &directory);

} // namespace pi::core
