#pragma once

#include <cstddef>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pi::core {

// Hardening limits (plans/agent-skills.md §2, §5).
inline constexpr size_t kSkillMaxNameLength = 64;
inline constexpr size_t kSkillMaxDescriptionLength = 1024;
inline constexpr size_t kSkillMaxShortDescriptionLength = 256;
inline constexpr size_t kSkillMaxBodyBytes = 128 * 1024;
inline constexpr size_t kSkillMaxPerRoot = 256;
inline constexpr size_t kSkillMaxTotal = 512;

struct SkillMetadata {
  std::string name;          // canonical, unique across the catalog
  std::string description;   // from frontmatter, <= 1024 chars
  std::string path;          // absolute path to SKILL.md
  std::filesystem::path dir; // skill directory (for sibling-file reads)
  std::string scope;         // "user" | "project" | "compat"
};

struct SkillCatalog {
  std::vector<SkillMetadata> skills;    // sorted by name
  std::vector<std::string> diagnostics; // parse/validation/collision notes
};

struct SkillFrontmatter {
  std::string name; // empty if absent (caller defaults)
  std::string description;
  std::string short_description; // parsed, unused in v1
};

// Splits and parses the YAML-frontmatter subset used by SKILL.md files
// (line-oriented, no yaml dependency; tolerates unknown keys/shapes).
// Returns an error string when frontmatter is missing or malformed.
std::expected<SkillFrontmatter, std::string>
parse_skill_frontmatter(std::string_view content);

// Scans all skill roots; pure function of (cwd, agent_dir).
//
// Roots, ascending precedence (later wins on name collision):
//   1. compat:   .claude/skills/, .codex/skills/ in workspace ancestors
//                (outermost -> innermost) — lowest precedence by design
//   2. user:     <agent_dir>/skills/
//   3. project:  .pici/skills/, skills/, .agents/skills/ in workspace
//                ancestors (outermost -> innermost)
SkillCatalog discover_skills(const std::filesystem::path &cwd,
                             const std::filesystem::path &agent_dir);

// Load and validate one skill body (frontmatter stripped). Reads the file
// fresh from disk. Returns error text on failure.
std::expected<std::string, std::string>
load_skill_body(const SkillMetadata &skill);

} // namespace pi::core
