#include "core/skills.h"

#include <algorithm>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>

namespace pi::core {

namespace {

constexpr int kMaxSkillDirDepth = 8;
const char *const kSkillFileName = "SKILL.md";

// Directories skipped when scanning *inside* a skill root.
bool is_skipped_scan_dir(const std::string &name) {
  return name == "node_modules" || name == ".git" ||
         (!name.empty() && name.front() == '.');
}

std::string_view strip_bom(std::string_view text) {
  if (text.size() >= 3 && text.substr(0, 3) == "\xEF\xBB\xBF")
    text.remove_prefix(3);
  return text;
}

std::string_view strip_cr(std::string_view line) {
  if (!line.empty() && line.back() == '\r')
    line.remove_suffix(1);
  return line;
}

std::string trim(const std::string_view s) {
  const auto begin = s.find_first_not_of(" \t");
  if (begin == std::string::npos)
    return {};
  const auto end = s.find_last_not_of(" \t");
  return std::string(s.substr(begin, end - begin + 1));
}

// Split into lines, tolerating CRLF and a missing trailing newline.
std::vector<std::string_view> split_lines(std::string_view text) {
  std::vector<std::string_view> lines;
  size_t pos = 0;
  while (pos < text.size()) {
    const auto nl = text.find('\n', pos);
    if (nl == std::string_view::npos) {
      lines.push_back(strip_cr(text.substr(pos)));
      break;
    }
    lines.push_back(strip_cr(text.substr(pos, nl - pos)));
    pos = nl + 1;
  }
  return lines;
}

bool valid_skill_name(std::string_view name) {
  if (name.empty() || name.size() > kSkillMaxNameLength)
    return false;
  for (const char c : name) {
    if (!(c >= 'a' && c <= 'z') && !(c >= '0' && c <= '9') && c != '-' &&
        c != '_')
      return false;
  }
  return true;
}

// Strip one layer of matching quotes around a scalar value.
std::string unquote(std::string value) {
  if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') ||
                            (value.front() == '\'' && value.back() == '\'')))
    return value.substr(1, value.size() - 2);
  return value;
}

bool is_indented(std::string_view line) {
  return !line.empty() && (line.front() == ' ' || line.front() == '\t');
}

struct FrontmatterSplit {
  std::vector<std::string> frontmatter;
  std::string_view body;
};

// Returns nullopt-equivalent (empty frontmatter) when the content does not
// carry a `---` delimited header closed by an exact `---` line.
FrontmatterSplit split_frontmatter(std::string_view content) {
  FrontmatterSplit result;
  content = strip_bom(content);
  const auto lines = split_lines(content);
  if (lines.empty() || trim(lines[0]) != "---")
    return result;

  size_t close = 0;
  for (size_t i = 1; i < lines.size(); ++i) {
    if (lines[i] == "---") { // exact match only (plan §2)
      close = i;
      break;
    }
  }
  if (close == 0)
    return result;

  result.frontmatter.reserve(close - 1);
  for (size_t i = 1; i < close; ++i)
    result.frontmatter.emplace_back(lines[i]);

  // Body begins right after the closing delimiter; compute its byte offset.
  size_t offset = 0;
  for (size_t i = 0; i <= close && i < lines.size(); ++i)
    offset += lines[i].size() + 1; // +1 for the newline
  if (offset > content.size())
    offset = content.size();
  result.body = content.substr(offset);
  while (!result.body.empty() && result.body.front() == '\n')
    result.body.remove_prefix(1); // drop the blank separator line
  return result;
}

struct KeyValue {
  std::string key;
  std::string value;
};

// Line-oriented subset: `key: value`, quoted scalars, and folded blocks
// (`key: >` / `key: |` followed by an indented block flattened with spaces).
// A value containing ": " needs no special casing — everything after the
// FIRST ": " is the scalar, so codex-style prose (`description: Build for
// AWS: ECS`) parses without a YAML dependency.
std::expected<std::vector<KeyValue>, std::string>
parse_key_values(const std::vector<std::string> &lines) {
  std::vector<KeyValue> pairs;
  size_t i = 0;
  while (i < lines.size()) {
    const std::string line = trim(lines[i]);
    if (line.empty()) {
      ++i;
      continue;
    }
    const auto colon = line.find(':');
    if (colon == std::string::npos)
      return std::unexpected("malformed frontmatter line: '" + line + "'");
    const std::string key = trim(line.substr(0, colon));
    if (key.empty() || key.find(' ') != std::string::npos)
      return std::unexpected("malformed frontmatter key: '" + key + "'");
    const std::string rest = trim(line.substr(colon + 1));

    if (rest == ">" || rest == "|") {
      // Fold the following indented block; ends at the first non-indented,
      // non-empty line.
      ++i;
      std::string joined;
      while (i < lines.size()) {
        const std::string_view raw = lines[i];
        if (trim(raw).empty()) {
          // Blank line: continue only if the block resumes after it.
          size_t j = i;
          while (j < lines.size() && trim(lines[j]).empty())
            ++j;
          if (j < lines.size() && is_indented(lines[j])) {
            ++i;
            continue;
          }
          break;
        }
        if (!is_indented(raw))
          break;
        if (!joined.empty())
          joined += ' ';
        joined += trim(raw);
        ++i;
      }
      pairs.push_back({key, joined});
      continue;
    }

    pairs.push_back({key, unquote(rest)});
    ++i;
  }
  return pairs;
}

bool contains_path(const std::filesystem::path &outer,
                   const std::filesystem::path &inner) {
  const auto o = outer.lexically_normal().string();
  const auto i = inner.lexically_normal().string();
  if (i.size() < o.size())
    return false;
  if (!o.empty() && i.compare(0, o.size(), o) != 0)
    return false;
  if (i.size() == o.size())
    return true;
  return i[o.size()] == '/' || o.back() == '/';
}

// Collect candidate skill directories under `root`, outermost-first,
// bounded depth, skipping hidden/node_modules dirs. Stops descending below
// a directory that is itself a skill (contains SKILL.md).
void collect_skill_dirs(const std::filesystem::path &root,
                        std::vector<std::filesystem::path> &out,
                        std::vector<std::string> &diagnostics) {
  std::error_code ec;
  if (!std::filesystem::is_directory(root, ec) || ec)
    return;

  std::function<void(const std::filesystem::path &, int)> walk =
      [&](const std::filesystem::path &dir, int depth) {
        std::error_code dec;
        std::filesystem::directory_iterator it(dir, dec), end;
        if (dec) {
          diagnostics.push_back("skills: cannot read directory '" +
                                dir.string() + "': " + dec.message());
          return;
        }
        // Deterministic order: sort entries by path.
        std::vector<std::filesystem::path> entries;
        for (const auto &e : it)
          entries.push_back(e.path());
        std::sort(entries.begin(), entries.end());

        std::error_code rec;
        const auto canonical_root = std::filesystem::weakly_canonical(dir, rec);

        for (const auto &entry : entries) {
          // Symlink containment: a link resolving outside the scanned root
          // is reported and skipped rather than followed.
          std::error_code lec;
          if (std::filesystem::is_symlink(entry, lec)) {
            const auto resolved_entry =
                std::filesystem::weakly_canonical(entry, lec);
            if (lec || !contains_path(canonical_root, resolved_entry)) {
              diagnostics.push_back("skills: '" + entry.string() +
                                    "' escapes its root (symlink?); skipped");
              continue;
            }
          }
          std::error_code fec;
          const bool is_dir = std::filesystem::is_directory(entry, fec) && !fec;
          std::error_code sec;
          const bool has_skill_file =
              std::filesystem::is_regular_file(entry / kSkillFileName, sec) &&
              !sec;
          if (has_skill_file) {
            out.push_back(entry);
            continue; // do not descend into a skill
          }
          if (is_dir && depth + 1 < kMaxSkillDirDepth &&
              !is_skipped_scan_dir(entry.filename().string()))
            walk(entry, depth + 1);
        }
      };
  walk(root, 0);
}

struct Candidate {
  SkillMetadata meta;
  int rank = 0; // root precedence; higher wins collisions
};

} // namespace

std::expected<SkillFrontmatter, std::string>
parse_skill_frontmatter(std::string_view content) {
  const auto parts = split_frontmatter(content);
  if (parts.frontmatter.empty())
    return std::unexpected(
        "missing frontmatter: expected a leading '---' line closed by '---'");

  auto pairs = parse_key_values(parts.frontmatter);
  if (!pairs)
    return std::unexpected(pairs.error());

  SkillFrontmatter result;
  for (const auto &kv : *pairs) {
    if (kv.key == "name") {
      if (!result.name.empty())
        return std::unexpected("duplicate 'name' key in frontmatter");
      result.name = kv.value;
    } else if (kv.key == "description") {
      if (!result.description.empty())
        return std::unexpected("duplicate 'description' key in frontmatter");
      result.description = kv.value;
    } else if (kv.key == "short-description") {
      if (!result.short_description.empty())
        return std::unexpected(
            "duplicate 'short-description' key in frontmatter");
      result.short_description = kv.value;
    }
    // Unknown keys parsed and ignored (forward compatibility).
  }

  if (result.description.empty())
    return std::unexpected("missing required 'description'");
  if (result.description.size() > kSkillMaxDescriptionLength)
    return std::unexpected("'description' exceeds " +
                           std::to_string(kSkillMaxDescriptionLength) +
                           " characters");
  if (!result.short_description.empty() &&
      result.short_description.size() > kSkillMaxShortDescriptionLength)
    return std::unexpected("'short-description' exceeds " +
                           std::to_string(kSkillMaxShortDescriptionLength) +
                           " characters");
  if (!result.name.empty() && !valid_skill_name(result.name))
    return std::unexpected("invalid 'name' '" + result.name +
                           "': use 1-64 lowercase letters, digits, '-', '_'");

  // Folded-block flattening can leave trailing spaces; normalize.
  result.description = trim(result.description);
  return result;
}

SkillCatalog discover_skills(const std::filesystem::path &cwd,
                             const std::filesystem::path &agent_dir) {
  SkillCatalog catalog;

  // Workspace ancestors, outermost -> innermost (same direction as
  // load_context_files()). Deduped via weakly_canonical.
  std::vector<std::filesystem::path> ancestors;
  {
    std::error_code ec;
    auto cur = std::filesystem::weakly_canonical(cwd, ec);
    if (ec)
      cur = cwd;
    while (true) {
      ancestors.push_back(cur);
      const auto parent = cur.parent_path();
      if (parent.empty() || parent == cur)
        break;
      cur = parent;
    }
    std::reverse(ancestors.begin(), ancestors.end()); // outermost first
  }

  int rank = 0;
  std::map<std::string, Candidate> by_name;
  auto add_root = [&](const std::filesystem::path &root,
                      const std::string &scope) {
    std::vector<std::filesystem::path> dirs;
    collect_skill_dirs(root, dirs, catalog.diagnostics);

    size_t accepted_from_root = 0;
    for (const auto &dir : dirs) {
      if (accepted_from_root >= kSkillMaxPerRoot) {
        catalog.diagnostics.push_back("skills: root '" + root.string() +
                                      "' exceeds " +
                                      std::to_string(kSkillMaxPerRoot) +
                                      " skills; remaining entries skipped");
        break;
      }

      const std::filesystem::path skill_file = dir / kSkillFileName;

      // Containment: reject symlink escapes discovered during the scan.
      std::error_code cec;
      const auto resolved = std::filesystem::weakly_canonical(skill_file, cec);
      const auto resolved_root = std::filesystem::weakly_canonical(root, cec);
      if (cec || !contains_path(resolved_root, resolved)) {
        catalog.diagnostics.push_back("skills: '" + skill_file.string() +
                                      "' escapes its root (symlink?); skipped");
        continue;
      }

      std::ifstream in(skill_file, std::ios::binary);
      if (!in) {
        catalog.diagnostics.push_back("skills: cannot read '" +
                                      skill_file.string() + "'");
        continue;
      }
      std::ostringstream buffer;
      buffer << in.rdbuf();
      const std::string content = buffer.str();

      auto fm = parse_skill_frontmatter(content);
      if (!fm) {
        catalog.diagnostics.push_back("skills: '" + skill_file.string() +
                                      "': " + fm.error());
        continue;
      }

      Candidate candidate;
      candidate.meta.dir = dir;
      candidate.meta.path = skill_file.string();
      candidate.meta.scope = scope;
      candidate.meta.description = fm->description;
      candidate.meta.name =
          !fm->name.empty() ? fm->name : dir.filename().string();
      if (!valid_skill_name(candidate.meta.name)) {
        catalog.diagnostics.push_back(
            "skills: '" + skill_file.string() + "': derived name '" +
            candidate.meta.name + "' is invalid; skipped");
        continue;
      }
      candidate.rank = rank++;

      const auto existing = by_name.find(candidate.meta.name);
      if (existing == by_name.end()) {
        by_name.emplace(candidate.meta.name, candidate);
        ++accepted_from_root;
      } else if (candidate.rank > existing->second.rank) {
        catalog.diagnostics.push_back(
            "skills: '" + existing->second.meta.path + "' overridden by '" +
            candidate.meta.path + "' (higher-precedence root)");
        existing->second = candidate;
        ++accepted_from_root;
      } else {
        catalog.diagnostics.push_back(
            "skills: '" + candidate.meta.path + "' collides with '" +
            existing->second.meta.path + "' and was skipped");
      }
    }
  };

  // Ascending precedence (plan §1):
  //   compat scans (lowest), user scope, project scopes (highest).
  for (const auto &ancestor : ancestors) {
    add_root(ancestor / ".claude" / "skills", "compat");
    add_root(ancestor / ".codex" / "skills", "compat");
  }
  add_root(agent_dir / "skills", "user");
  for (const auto &ancestor : ancestors) {
    add_root(ancestor / ".pici" / "skills", "project");
    add_root(ancestor / "skills", "project");
    add_root(ancestor / ".agents" / "skills", "project");
  }

  // Total cap: keep a deterministic subset (sorted by name).
  if (by_name.size() > kSkillMaxTotal) {
    catalog.diagnostics.push_back("skills: total count exceeds " +
                                  std::to_string(kSkillMaxTotal) +
                                  "; dropping lowest-named overflow");
  }
  catalog.skills.reserve(by_name.size());
  for (auto &entry : by_name)
    catalog.skills.push_back(entry.second.meta);
  std::sort(catalog.skills.begin(), catalog.skills.end(),
            [](const SkillMetadata &a, const SkillMetadata &b) {
              return a.name < b.name;
            });
  if (catalog.skills.size() > kSkillMaxTotal) {
    for (size_t i = kSkillMaxTotal; i < catalog.skills.size(); ++i)
      catalog.diagnostics.push_back("skills: '" + catalog.skills[i].path +
                                    "' dropped (total cap " +
                                    std::to_string(kSkillMaxTotal) + ")");
    catalog.skills.resize(kSkillMaxTotal);
  }
  return catalog;
}

std::expected<std::string, std::string>
load_skill_body(const SkillMetadata &skill) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(skill.path, ec) || ec)
    return std::unexpected("skill file not found: " + skill.path);

  std::ifstream in(skill.path, std::ios::binary);
  if (!in)
    return std::unexpected("cannot read skill file: " + skill.path);
  std::ostringstream buffer;
  buffer << in.rdbuf();
  const std::string content = buffer.str();

  const auto parts = split_frontmatter(content);
  if (parts.frontmatter.empty())
    return std::unexpected("skill file has no valid frontmatter: " +
                           skill.path);

  std::string body(parts.body);
  if (body.size() > kSkillMaxBodyBytes)
    return std::unexpected("skill body exceeds " +
                           std::to_string(kSkillMaxBodyBytes) +
                           " bytes; refusing to load '" + skill.name + "'");

  // Trim trailing whitespace-only tail to avoid pointless token spend?
  // No — body is returned verbatim per plan §5.
  return body;
}

} // namespace pi::core
