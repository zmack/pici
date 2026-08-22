#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "core/skills.h"

namespace pi::cli {

struct ContextFile {
  std::string path;
  std::string content;
};

// Injects a compact '# Skills' index when |skills| is non-null and its
// catalog is non-empty (progressive disclosure: names + descriptions only,
// bodies load via the 'skill' tool).
std::string build_system_prompt(std::string_view custom_prompt,
                                const std::vector<std::string> &append_prompts,
                                const std::vector<ContextFile> &context_files,
                                const std::vector<std::string> &tool_names,
                                const std::filesystem::path &cwd,
                                const pi::core::SkillCatalog *skills = nullptr);

} // namespace pi::cli
