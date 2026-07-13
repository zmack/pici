#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pi::cli {

struct ContextFile {
  std::string path;
  std::string content;
};

std::string build_system_prompt(std::string_view custom_prompt,
                                const std::vector<std::string> &append_prompts,
                                const std::vector<ContextFile> &context_files,
                                const std::vector<std::string> &tool_names,
                                const std::filesystem::path &cwd);

} // namespace pi::cli
