#include "core/builtin_tools.h"
#include "core/apply_patch.h"
#include "core/message_types.h"
#include "core/sandbox.h"
#include "core/skills.h"
#include "core/terminal.h"
#include "nlohmann/json_fwd.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <queue>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/poll.h>
#include <system_error>
#include <utility>
#include <vector>

#include <chrono>
#include <thread>

#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace pi::core {
namespace {

constexpr std::size_t kMaxBytes = static_cast<std::size_t>(64 * 1024);
// Separate, much larger cap on the total bytes a single bash command may
// write to its on-disk spill file (see BashTool::execute). kMaxBytes is the
// small in-memory/visible-content cap; once a command's output crosses it,
// the read loop no longer breaks early (so a slower command with large but
// finite output can still capture its full stream on disk) -- but that only
// works if *something* still bounds a truly unbounded producer (e.g.
// `yes`), or it would run for the full timeout while writing unbounded data
// into the workspace. Once the spill file reaches kMaxSpillBytes, BashTool
// kills the child exactly as it does on timeout.
constexpr std::size_t kMaxSpillBytes = static_cast<std::size_t>(16 * 1024 * 1024);
constexpr std::size_t kReadMaxLines = 2000;
constexpr int kLsDefaultLimit = 500;
constexpr int kFindDefaultLimit = 1000;
constexpr int kFuzzyFindDefaultLimit = 20;
constexpr int kGrepDefaultLimit = 100;
// Suggestions appended when the skill tool is called with an unknown name
// (plans/agent-skills.md §5: cheap miss handling saves a whole retry turn).
constexpr std::size_t kSkillMissSuggestions = 3;

class TextToolResult : public ToolResult {
public:
  TextToolResult(std::string content, bool is_error = false,
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

class StaticJsonSchema : public JsonSchemaToolSchema {
public:
  explicit StaticJsonSchema(std::string schema) : schema_(std::move(schema)) {}

  std::string serialize() const override { return schema_; }

  std::map<std::string, std::string> to_definition() const override {
    auto parsed = nlohmann::json::parse(schema_, nullptr, false);
    std::map<std::string, std::string> result;
    if (parsed.is_discarded() || !parsed.is_object()) {
      result["type"] = "object";
      return result;
    }
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
      if (it.value().is_string()) {
        result[it.key()] = it.value().get<std::string>();
      } else {
        result[it.key()] = it.value().dump();
      }
    }
    return result;
  }

private:
  std::string schema_;
};

class BuiltinTool : public ToolDefinition {
public:
  BuiltinTool(std::string name, std::string description, std::string schema,
              const std::filesystem::path &cwd, bool child_safe = false)
      : name_(std::move(name)), description_(std::move(description)),
        schema_(std::move(schema)), cwd_(std::filesystem::absolute(cwd)),
        child_safe_(child_safe) {}

  std::string_view name() const override { return name_; }
  std::string_view description() const override { return description_; }
  ToolSchema &schema() const override { return schema_; }
  ToolCapabilities capabilities() const override {
    return {.child_safe = child_safe_};
  }

protected:
  std::filesystem::path resolve_workspace_path(const std::string &path) const {
    std::filesystem::path candidate =
        path.empty() ? cwd_ : std::filesystem::path(path);
    if (candidate.is_relative()) {
      candidate = cwd_ / candidate;
    }
    candidate = candidate.lexically_normal();

    std::error_code ec;
    auto canonical_cwd = std::filesystem::weakly_canonical(cwd_, ec);
    if (ec) {
      canonical_cwd = cwd_.lexically_normal();
    }

    auto probe = candidate;
    auto existing_probe = candidate;
    while (!std::filesystem::exists(existing_probe, ec) &&
           existing_probe.has_parent_path() &&
           existing_probe != existing_probe.parent_path()) {
      existing_probe = existing_probe.parent_path();
    }

    auto canonical_probe =
        std::filesystem::weakly_canonical(existing_probe, ec);
    if (ec) {
      canonical_probe = existing_probe.lexically_normal();
    }

    const auto rel =
        std::filesystem::relative(canonical_probe, canonical_cwd, ec);
    if (ec || rel.empty() || rel.native().starts_with("..")) {
      throw std::runtime_error("Path is outside the workspace: " + path);
    }
    return probe;
  }

  const std::filesystem::path &cwd() const { return cwd_; }

private:
  std::string name_;
  std::string description_;
  mutable StaticJsonSchema schema_;
  std::filesystem::path cwd_;
  bool child_safe_{false};
};

nlohmann::json parse_args(std::string_view args_json) {
  if (args_json.empty()) {
    return nlohmann::json::object();
  }
  auto parsed = nlohmann::json::parse(args_json, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object()) {
    throw std::runtime_error("Tool arguments must be a JSON object");
  }
  return parsed;
}

std::shared_ptr<ToolResult> error_result(const std::exception &err) {
  return std::make_shared<TextToolResult>(err.what(), true);
}

std::string read_text_file(const std::filesystem::path &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Cannot read file: " + path.string());
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void write_text_file(const std::filesystem::path &path,
                     const std::string &content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) {
    throw std::runtime_error("Cannot write file: " + path.string());
  }
  out << content;
}

std::vector<std::string> split_lines(const std::string &content) {
  std::vector<std::string> lines;
  std::istringstream in(content);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    lines.push_back(line);
  }
  if (!content.empty() && content.back() == '\n') {
    lines.emplace_back();
  }
  return lines;
}

// Structured metadata about a truncate_head_ex() call, computed as a
// byproduct of the same single walk over `content` that produces the
// (possibly truncated) output string.
//
// Naming note: `input_lines`/`input_bytes` describe the size of the string
// truncate_head_ex actually saw -- NOT necessarily the real underlying
// command/file output. At every call site, that string may already be
// lossy upstream of here: BashTool's own read loop caps `output` before
// this ever sees it (see the bash spill-file design for what actually
// bounds that), ReadTool's string already has its own
// "[N more lines in file...]" note counted as content, and Ls/Find/Grep
// are already limit-capped upstream. Calling these fields "total_*" would
// wrongly imply they describe the command's real, uncapped output size.
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

TruncatedContent truncate_head_ex(const std::string &content,
                                  std::size_t max_lines,
                                  std::size_t max_bytes) {
  std::size_t total_lines = 0;
  for (char c : content)
    if (c == '\n')
      ++total_lines;
  if (!content.empty() && content.back() != '\n')
    ++total_lines;

  std::string out;
  std::size_t out_lines = 0;
  bool hit_lines = false;
  bool hit_bytes = false;

  for (char ch : content) {
    if (out_lines >= max_lines) {
      hit_lines = true;
      break;
    }
    if (out.size() >= max_bytes) {
      hit_bytes = true;
      break;
    }
    out.push_back(ch);
    if (ch == '\n')
      ++out_lines;
  }

  TruncationInfo info;
  info.input_lines = total_lines;
  info.input_bytes = content.size();
  info.output_lines = out_lines;
  info.output_bytes = out.size();
  info.max_lines = max_lines;
  info.max_bytes = max_bytes;
  info.truncated = hit_lines || hit_bytes;
  if (hit_lines) {
    info.truncated_by = "lines";
  } else if (hit_bytes) {
    info.truncated_by = "bytes";
  }
  // The break above happens before `ch` is pushed, so when truncation was
  // triggered by the byte cap the output can end mid-line (the last
  // character need not be '\n'). When triggered by the line cap, out_lines
  // has already reached max_lines via a completed '\n', so this is always
  // false in that case -- but we compute it generically off `out` itself
  // rather than branching on hit_lines/hit_bytes, since that's the actual
  // condition callers care about.
  info.last_line_partial =
      info.truncated && !out.empty() && out.back() != '\n';
  // True when even the first line alone was too big to fit under the byte
  // cap (truncation hit on the byte limit before a single '\n' was ever
  // written to `out`).
  info.first_line_exceeds_limit = hit_bytes && out_lines == 0;

  if (hit_lines || hit_bytes) {
    if (hit_lines) {
      out += "\n\n[Truncated: showing " + std::to_string(max_lines) + " of " +
             std::to_string(total_lines) + " lines]";
    } else {
      out += "\n\n[Truncated: " + std::to_string(out_lines) + " lines shown (" +
             std::to_string(max_bytes / 1024) + "KB limit)]";
    }
  }
  return {std::move(out), info};
}

std::string truncate_head(const std::string &content, std::size_t max_lines,
                          std::size_t max_bytes) {
  return truncate_head_ex(content, max_lines, max_bytes).content;
}

std::string glob_to_regex(std::string_view glob) {
  std::string out = "^";
  for (std::size_t i = 0; i < glob.size(); ++i) {
    const char ch = glob[i];
    if (ch == '*') {
      if (i + 1 < glob.size() && glob[i + 1] == '*') {
        if (i + 2 < glob.size() && glob[i + 2] == '/') {
          out += "(?:.*/)?";
          i += 2;
        } else {
          out += ".*";
          ++i;
        }
      } else {
        out += "[^/]*";
      }
    } else if (ch == '?') {
      out += "[^/]";
    } else if (std::string_view(".+()[]{}^$\\|").contains(ch)) {
      out.push_back('\\');
      out.push_back(ch);
    } else {
      out.push_back(ch);
    }
  }
  out += '$';
  return out;
}

bool should_skip_dir(const std::filesystem::path &path) {
  const auto name = path.filename().string();
  return name == ".git" || name == "node_modules" || name == "build" ||
         name == "build-asan" || name == ".pici";
}

class GitIgnore {
public:
  void load(const std::filesystem::path &dir) {
    auto path = dir / ".gitignore";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
      return;
    std::ifstream f(path);
    if (!f)
      return;
    std::string line;
    while (std::getline(f, line)) {
      if (!line.empty() && line.back() == '\r')
        line.pop_back();
      if (line.empty() || line[0] == '#' || line[0] == '!')
        continue;
      add_pattern(line);
    }
  }

  bool ignored(const std::filesystem::path &abs_path,
               const std::filesystem::path &root, bool is_dir) const {
    if (patterns_.empty())
      return false;
    const std::string name = abs_path.filename().string();
    const std::string rel = abs_path.lexically_relative(root).generic_string();
    return std::ranges::any_of(patterns_, [&](const auto &p) {
      if (p.dirs_only && !is_dir)
        return false;
      const std::string &subject = p.has_slash ? rel : name;
      return std::regex_match(subject, p.re);
    });
  }

  bool empty() const { return patterns_.empty(); }

private:
  struct Pattern {
    std::regex re;
    bool dirs_only{false};
    bool has_slash{false};
  };

  void add_pattern(std::string pat) {
    Pattern p;
    if (!pat.empty() && pat.back() == '/') {
      p.dirs_only = true;
      pat.pop_back();
    }
    if (!pat.empty() && pat.front() == '/') {
      p.has_slash = true;
      pat = pat.substr(1);
    } else {
      p.has_slash = pat.contains('/');
    }
    try {
      p.re = std::regex(glob_to_regex(pat));
      patterns_.push_back(std::move(p));
    } catch (...) {
      static_cast<void>(0); // Ignore malformed ignore-file patterns.
    }
  }

  std::vector<Pattern> patterns_;
};

std::string image_mime_type(const std::filesystem::path &path) {
  static const std::array<std::pair<std::string_view, std::string_view>, 8>
      kMimes = {{
          {".jpg", "image/jpeg"},
          {".jpeg", "image/jpeg"},
          {".png", "image/png"},
          {".gif", "image/gif"},
          {".webp", "image/webp"},
          {".bmp", "image/bmp"},
          {".ico", "image/x-icon"},
          {".svg", "image/svg+xml"},
      }};
  std::string ext = path.extension().string();
  for (char &c : ext)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  for (const auto &[e, m] : kMimes)
    if (e == ext)
      return std::string(m);
  return {};
}

std::string base64_encode(const std::string &data) {
  static constexpr std::string_view kTable =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  for (std::size_t i = 0; i < data.size(); i += 3) {
    unsigned int b = static_cast<unsigned char>(data[i]) << 16U;
    if (i + 1 < data.size())
      b |= static_cast<unsigned char>(data[i + 1]) << 8U;
    if (i + 2 < data.size())
      b |= static_cast<unsigned char>(data[i + 2]);
    out += kTable[(b >> 18U) & 63U];
    out += kTable[(b >> 12U) & 63U];
    out += (i + 1 < data.size()) ? kTable[(b >> 6U) & 63U] : '=';
    out += (i + 2 < data.size()) ? kTable[b & 63U] : '=';
  }
  return out;
}

class ImageToolResult : public ToolResult {
public:
  ImageToolResult(std::string b64, std::string mime, std::string desc)
      : b64_(std::move(b64)), mime_(std::move(mime)), desc_(std::move(desc)) {}
  bool is_error() const override { return false; }
  std::string content() const override { return desc_; }
  std::optional<std::string> details() const override { return std::nullopt; }
  std::vector<ToolResultContentBlock> content_blocks() const override {
    return {TextContent{.text = desc_},
            ImageContent{.data = b64_, .mime_type = mime_}};
  }

private:
  std::string b64_, mime_, desc_;
};

std::string relative_posix(const std::filesystem::path &path,
                           const std::filesystem::path &base) {
  std::error_code ec;
  auto rel = std::filesystem::relative(path, base, ec);
  if (ec) {
    rel = path.filename();
  }
  auto value = rel.generic_string();
  return value.empty() ? "." : value;
}

std::optional<int> fuzzy_atom_score(std::string_view candidate,
                                    std::string_view atom) {
  if (atom.empty())
    return 0;
  if (candidate.empty() || atom.size() > candidate.size())
    return std::nullopt;

  constexpr int kMissing = std::numeric_limits<int>::min() / 4;
  const std::size_t n = candidate.size();
  const std::size_t m = atom.size();
  std::vector<std::vector<int>> best(n, std::vector<int>(m, kMissing));
  std::vector<std::vector<int>> ending(n, std::vector<int>(m, kMissing));
  const auto folded = [](char ch) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  };
  for (std::size_t i = 0; i < n; ++i) {
    for (std::size_t j = 0; j < m; ++j) {
      if (i > 0)
        best[i][j] = best[i - 1][j];
      if (folded(candidate[i]) != folded(atom[j]))
        continue;
      int prior = j == 0 ? 0 : kMissing;
      if (j > 0 && i > 0)
        prior = best[i - 1][j - 1];
      if (j > 0 && i > 0 && ending[i - 1][j - 1] != kMissing)
        prior = std::max(prior, ending[i - 1][j - 1] + 1);
      if (prior == kMissing)
        continue;
      int bonus = 0;
      const bool boundary =
          i == 0 || candidate[i - 1] == '/' || candidate[i - 1] == '_' ||
          candidate[i - 1] == '-' || candidate[i - 1] == '.' ||
          (static_cast<bool>(
               std::islower(static_cast<unsigned char>(candidate[i - 1]))) &&
           static_cast<bool>(
               std::isupper(static_cast<unsigned char>(candidate[i]))));
      if (boundary)
        bonus += 2;
      const auto slash = candidate.rfind('/');
      if (slash == std::string_view::npos || i > slash)
        bonus += 1;
      ending[i][j] = prior + bonus;
      best[i][j] = ending[i][j];
      if (i > 0)
        best[i][j] = std::max(best[i][j], best[i - 1][j]);
    }
  }
  int result = kMissing;
  for (std::size_t i = 0; i < n; ++i)
    result = std::max(result, ending[i][m - 1]);
  return result == kMissing ? std::nullopt : std::optional<int>(result);
}

std::optional<int> fuzzy_score_path(std::string_view candidate,
                                    std::string_view query) {
  int total = 0;
  std::size_t start = 0;
  while (start <= query.size()) {
    const auto end = query.find_first_of(" \t\n\r", start);
    const auto atom =
        query.substr(start, end == std::string_view::npos ? query.size() - start
                                                          : end - start);
    if (!atom.empty()) {
      const auto score = fuzzy_atom_score(candidate, atom);
      if (!score)
        return std::nullopt;
      total += *score;
    }
    if (end == std::string_view::npos)
      break;
    start = end + 1;
  }
  return total;
}

class ReadTool final : public BuiltinTool {
public:
  explicit ReadTool(const std::filesystem::path &cwd)
      : BuiltinTool(
            "read",
            "Read the contents of a text file. Supports path, offset, and "
            "limit. Output is truncated for large files.",
            R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to read (relative or absolute)"},"offset":{"type":"number","description":"Line number to start reading from (1-indexed)"},"limit":{"type":"number","description":"Maximum number of lines to read"}},"required":["path"],"additionalProperties":false})json",
            cwd, true) {}

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view args,
                                      std::stop_token,
                                      ToolUpdateCallback) const override {
    try {
      const auto json = parse_args(args);
      const auto path = resolve_workspace_path(json.value("path", ""));

      // Image detection — return as base64 content block
      const auto mime = image_mime_type(path);
      if (!mime.empty()) {
        std::ifstream f(path, std::ios::binary);
        if (!f)
          throw std::runtime_error("Cannot read file: " + path.string());
        std::string raw((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        const auto kb = raw.size() / 1024;
        std::string desc = "[image: " + path.filename().string() + ", " + mime +
                           ", " + std::to_string(kb) + "KB]";
        return std::make_shared<ImageToolResult>(base64_encode(raw), mime,
                                                 std::move(desc));
      }

      const auto content = read_text_file(path);
      const auto lines = split_lines(content);
      const auto offset = std::max(1, json.value("offset", 1));
      const auto limit = json.contains("limit")
                             ? std::max(0, json.value("limit", 0))
                             : static_cast<int>(kReadMaxLines);
      if (offset > static_cast<int>(lines.size()) + 1) {
        throw std::runtime_error("Offset is beyond end of file");
      }
      const auto start = static_cast<std::size_t>(offset - 1);
      const auto end =
          std::min(lines.size(), start + static_cast<std::size_t>(limit));
      std::ostringstream out;
      for (std::size_t i = start; i < end; ++i) {
        out << lines[i];
        if (i + 1 < end) {
          out << '\n';
        }
      }
      if (end < lines.size()) {
        out << "\n\n[" << (lines.size() - end)
            << " more lines in file. Use offset=" << (end + 1)
            << " to continue.]";
      }
      // Line limit already applied above — only enforce byte limit here
      return std::make_shared<TextToolResult>(truncate_head(
          out.str(), std::numeric_limits<std::size_t>::max(), kMaxBytes));
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

// Loads the full body of a named skill from the catalog (plan
// plans/agent-skills.md §5). Read-only, so it is registered in both coding
// and read-only tool sets and inherited by mailbox children.
class SkillTool final : public BuiltinTool {
public:
  SkillTool(const std::filesystem::path &cwd,
            std::shared_ptr<const SkillCatalog> catalog)
      : BuiltinTool(
            "skill",
            "Load the full instructions of a named skill. Use when the task "
            "matches a skill listed under '# Skills' in your system prompt.",
            R"json({"type":"object","properties":{"name":{"type":"string","description":"Exact skill name from the '# Skills' index"}},"required":["name"],"additionalProperties":false})json",
            cwd, true),
        catalog_(std::move(catalog)) {}

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view args,
                                      std::stop_token,
                                      ToolUpdateCallback) const override {
    try {
      const auto json = parse_args(args);
      const auto name = json.value("name", "");
      if (name.empty()) {
        throw std::runtime_error("Missing required argument: name");
      }

      const SkillMetadata *match = nullptr;
      if (catalog_ != nullptr) {
        for (const auto &skill : catalog_->skills) {
          if (skill.name == name) {
            match = &skill;
            break;
          }
        }
      }
      if (match == nullptr) {
        // Exact-name lookup only; suggest the closest names to save a retry
        // turn.
        throw std::runtime_error("Unknown skill '" + name + "'." +
                                 closest_names(name));
      }

      auto body = load_skill_body(*match);
      if (!body) {
        return std::make_shared<TextToolResult>(body.error(), true);
      }
      return std::make_shared<TextToolResult>(
          "Loaded skill '" + match->name + "' from " + match->path +
          ". Sibling files referenced below can be read relative to " +
          match->dir.string() + ".\n\n" + *body);
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }

private:
  // Up to three closest catalog names by common-prefix length.
  [[nodiscard]] std::string closest_names(const std::string &name) const {
    if (catalog_ == nullptr || catalog_->skills.empty()) {
      return {};
    }
    const auto prefix_len = [](const std::string &a, const std::string &b) {
      size_t n = 0;
      while (n < a.size() && n < b.size() && a[n] == b[n]) {
        ++n;
      }
      return n;
    };
    std::vector<std::pair<size_t, std::string>> scored;
    for (const auto &skill : catalog_->skills) {
      scored.emplace_back(prefix_len(skill.name, name), skill.name);
    }
    std::sort(scored.begin(), scored.end(),
              [](const auto &a, const auto &b) { return a.first > b.first; });
    std::string out = " Available skills:";
    for (size_t i = 0; i < scored.size() && i < kSkillMissSuggestions; ++i) {
      out += (i == 0 ? " " : ", ") + scored[i].second;
    }
    return out;
  }

  std::shared_ptr<const SkillCatalog> catalog_;
};

class WriteTool final : public BuiltinTool {
public:
  explicit WriteTool(const std::filesystem::path &cwd)
      : BuiltinTool(
            "write",
            "Write content to a file. Creates parent directories and "
            "overwrites existing content. Path must be inside the current "
            "working directory (relative paths are resolved from it); paths "
            "outside it, including /tmp, are rejected -- put scratch files "
            "in a workspace subdirectory instead.",
            R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to write (relative or absolute)"},"content":{"type":"string","description":"Content to write to the file"}},"required":["path","content"],"additionalProperties":false})json",
            cwd, true) {}

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view args,
                                      std::stop_token,
                                      ToolUpdateCallback) const override {
    try {
      const auto json = parse_args(args);
      const auto path = resolve_workspace_path(json.value("path", ""));
      const auto content = json.value("content", std::string{});
      write_text_file(path, content);
      return std::make_shared<TextToolResult>(
          "Successfully wrote " + std::to_string(content.size()) +
          " bytes to " + json.value("path", ""));
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

std::string normalize_to_lf(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\r') {
      out += '\n';
      if (i + 1 < s.size() && s[i + 1] == '\n')
        ++i;
    } else {
      out += s[i];
    }
  }
  return out;
}

// Strip UTF-8 BOM if present, return remainder.
std::string strip_bom(const std::string &s, bool &had_bom) {
  if (s.size() >= 3 && static_cast<unsigned char>(s[0]) == 0xEF &&
      static_cast<unsigned char>(s[1]) == 0xBB &&
      static_cast<unsigned char>(s[2]) == 0xBF) {
    had_bom = true;
    return s.substr(3);
  }
  had_bom = false;
  return s;
}

// Normalize text for fuzzy matching:
//   - strip trailing whitespace per line
//   - smart single quotes  → '
//   - smart double quotes  → "
//   - Unicode dashes       → -
//   - non-breaking / special spaces → regular space
std::string normalize_for_fuzzy_match(const std::string &text) {
  // Pass 1: strip trailing whitespace per line
  std::string pass1;
  pass1.reserve(text.size());
  std::size_t line_start = 0;
  for (std::size_t i = 0; i <= text.size(); ++i) {
    if (i == text.size() || text[i] == '\n') {
      std::size_t end = i;
      while (end > line_start &&
             (text[end - 1] == ' ' || text[end - 1] == '\t'))
        --end;
      pass1.append(text, line_start, end - line_start);
      if (i < text.size())
        pass1 += '\n';
      line_start = i + 1;
    }
  }

  // Pass 2: replace known multi-byte Unicode sequences
  std::string out;
  out.reserve(pass1.size());
  for (std::size_t i = 0; i < pass1.size();) {
    auto b0 = static_cast<unsigned char>(pass1[i]);
    auto b1 =
        i + 1 < pass1.size() ? static_cast<unsigned char>(pass1[i + 1]) : 0U;
    auto b2 =
        i + 2 < pass1.size() ? static_cast<unsigned char>(pass1[i + 2]) : 0U;

    // U+00A0 NBSP → space  (2-byte: C2 A0)
    if (b0 == 0xC2 && b1 == 0xA0) {
      out += ' ';
      i += 2;
      continue;
    }

    // 3-byte sequences starting with E2
    if (b0 == 0xE2) {
      if (b1 == 0x80) {
        // Smart single quotes U+2018–U+201B  → '
        if (b2 >= 0x98 && b2 <= 0x9B) {
          out += '\'';
          i += 3;
          continue;
        }
        // Smart double quotes U+201C–U+201F  → "
        if (b2 >= 0x9C && b2 <= 0x9F) {
          out += '"';
          i += 3;
          continue;
        }
        // Dashes U+2010–U+2015            → -
        if (b2 >= 0x90 && b2 <= 0x95) {
          out += '-';
          i += 3;
          continue;
        }
        // Special spaces U+2002–U+200A    → space
        if (b2 >= 0x82 && b2 <= 0x8A) {
          out += ' ';
          i += 3;
          continue;
        }
        // U+202F narrow NBSP (E2 80 AF)   → space
        if (b2 == 0xAF) {
          out += ' ';
          i += 3;
          continue;
        }
      }
      // U+2212 minus sign (E2 88 92)      → -
      if (b1 == 0x88 && b2 == 0x92) {
        out += '-';
        i += 3;
        continue;
      }
      // U+205F medium math space (E2 81 9F) → space
      if (b1 == 0x81 && b2 == 0x9F) {
        out += ' ';
        i += 3;
        continue;
      }
    }
    // U+3000 ideographic space (E3 80 80) → space
    if (b0 == 0xE3 && b1 == 0x80 && b2 == 0x80) {
      out += ' ';
      i += 3;
      continue;
    }

    out += pass1[i++];
  }
  return out;
}

struct FuzzyFindResult {
  bool found{false};
  std::size_t index{0};
  std::size_t match_length{0};
  bool used_fuzzy{false};
};

// Try exact match, fall back to fuzzy. Returns result in the space
// (original or fuzzy-normalized) where the match was found.
FuzzyFindResult fuzzy_find(const std::string &content,
                           const std::string &old_text) {
  auto pos = content.find(old_text);
  if (pos != std::string::npos)
    return {.found = true,
            .index = pos,
            .match_length = old_text.size(),
            .used_fuzzy = false};

  const auto fc = normalize_for_fuzzy_match(content);
  const auto fo = normalize_for_fuzzy_match(old_text);
  pos = fc.find(fo);
  if (pos != std::string::npos)
    return {.found = true,
            .index = pos,
            .match_length = fo.size(),
            .used_fuzzy = true};

  return {};
}

std::size_t count_occurrences(const std::string &content,
                              const std::string &needle) {
  const auto fc = normalize_for_fuzzy_match(content);
  const auto fn = normalize_for_fuzzy_match(needle);
  std::size_t count = 0;
  std::size_t pos = 0;
  while ((pos = fc.find(fn, pos)) != std::string::npos) {
    ++count;
    pos += fn.size();
  }
  return count;
}

// Best-effort hint for a failed oldText match: locate where the edit's
// first line still occurs (exactly, or fuzzily under the same whitespace/
// Unicode normalization apply_edits itself uses) and show a few lines of
// the file's *current* content around it, so the caller can correct
// oldText on the next call instead of re-reading the whole file. Returns
// an empty string when no plausible anchor is found.
std::string near_miss_hint(const std::string &lf_content,
                           const std::string &old_text) {
  const auto nl = old_text.find('\n');
  std::string needle =
      nl == std::string::npos ? old_text : old_text.substr(0, nl);
  const auto first = needle.find_first_not_of(" \t");
  if (first == std::string::npos)
    return {};
  const auto last = needle.find_last_not_of(" \t");
  needle = needle.substr(first, last - first + 1);
  if (needle.size() < 6) // too short to anchor on with any confidence
    return {};

  const auto r = fuzzy_find(lf_content, needle);
  if (!r.found)
    return {};

  const std::string searched =
      r.used_fuzzy ? normalize_for_fuzzy_match(lf_content) : lf_content;
  const auto line_idx = static_cast<std::size_t>(std::count(
      searched.begin(), searched.begin() + static_cast<std::ptrdiff_t>(r.index),
      '\n'));

  const auto lines = split_lines(lf_content);
  if (line_idx >= lines.size())
    return {};
  const auto start = line_idx > 1 ? line_idx - 1 : std::size_t{0};
  const auto end = std::min(lines.size() - 1, line_idx + 4);

  std::ostringstream out;
  out << " Closest match is near line " << (line_idx + 1) << ":\n";
  for (auto i = start; i <= end; ++i) {
    out << "  " << (i + 1) << ": " << lines[i] << "\n";
  }
  return out.str();
}

struct MatchedEdit {
  std::size_t edit_index;
  std::size_t match_index;
  std::size_t match_length;
  std::string new_text; // LF-normalized
};

// Apply edits to LF-normalized content. All edits are matched against the same
// base content. If any needed fuzzy matching, the base is normalized first.
// Returns the new content (still LF-normalized).
std::string
apply_edits(const std::string &lf_content,
            const std::vector<std::pair<std::string, std::string>> &edits,
            const std::string &path) {
  // First pass: determine whether any edit needs fuzzy matching
  bool any_fuzzy = false;
  for (const auto &[old_text, _] : edits) {
    if (!lf_content.contains(old_text)) {
      any_fuzzy = true;
      break;
    }
  }
  const std::string base =
      any_fuzzy ? normalize_for_fuzzy_match(lf_content) : lf_content;

  // Second pass: match all edits against base
  std::vector<MatchedEdit> matched;
  matched.reserve(edits.size());
  for (std::size_t i = 0; i < edits.size(); ++i) {
    const auto &[old_text, new_text] = edits[i];
    if (old_text.empty()) {
      if (edits.size() == 1)
        throw std::runtime_error("oldText must not be empty in " + path);
      throw std::runtime_error("edits[" + std::to_string(i) +
                               "].oldText must not be empty in " + path);
    }
    auto r = fuzzy_find(base, old_text);
    if (!r.found) {
      const auto hint = near_miss_hint(lf_content, old_text);
      std::string message;
      if (edits.size() == 1) {
        message = "Could not find the text in " + path +
                  ". The old text must match exactly including all whitespace "
                  "and newlines.";
      } else {
        message = "Could not find edits[" + std::to_string(i) + "] in " + path +
                  ". The oldText must match exactly including all whitespace "
                  "and newlines.";
      }
      message += hint;
      throw std::runtime_error(message);
    }
    auto occ = count_occurrences(base, old_text);
    if (occ > 1) {
      if (edits.size() == 1)
        throw std::runtime_error("Found " + std::to_string(occ) +
                                 " occurrences of the text in " + path +
                                 ". The text must be unique. Please provide "
                                 "more context to make it unique.");
      throw std::runtime_error("Found " + std::to_string(occ) +
                               " occurrences of edits[" + std::to_string(i) +
                               "] in " + path +
                               ". Each oldText must be unique. Please provide "
                               "more context to make it unique.");
    }
    matched.push_back({.edit_index = i,
                       .match_index = r.index,
                       .match_length = r.match_length,
                       .new_text = normalize_to_lf(new_text)});
  }

  // Sort by position and check for overlaps
  std::ranges::sort(matched, [](const MatchedEdit &a, const MatchedEdit &b) {
    return a.match_index < b.match_index;
  });
  for (std::size_t i = 1; i < matched.size(); ++i) {
    const auto &prev = matched[i - 1];
    const auto &cur = matched[i];
    if (prev.match_index + prev.match_length > cur.match_index) {
      throw std::runtime_error(
          "edits[" + std::to_string(prev.edit_index) + "] and edits[" +
          std::to_string(cur.edit_index) + "] overlap in " + path +
          ". Merge them into one edit or target disjoint regions.");
    }
  }

  // Apply in reverse order (stable offsets)
  std::string result = base;
  for (int i = static_cast<int>(matched.size()) - 1; i >= 0; --i) {
    const auto &m = matched[static_cast<std::size_t>(i)];
    result.replace(m.match_index, m.match_length, m.new_text);
  }
  return result;
}

class ApplyPatchTool final : public BuiltinTool {
public:
  explicit ApplyPatchTool(const std::filesystem::path &cwd)
      : BuiltinTool(
            "apply_patch",
            "Apply a multi-file patch atomically using the *** Begin Patch "
            "format.",
            R"json({"type":"object","properties":{"input":{"type":"string","description":"Full patch text starting with *** Begin Patch"}},"required":["input"],"additionalProperties":false})json",
            cwd, false) {}

  std::shared_ptr<ToolResult>
  execute(std::string_view, std::string_view args, std::stop_token,
          ToolUpdateCallback on_update) const override {
    try {
      auto json = parse_args(args);
      if (!json.contains("input") || !json["input"].is_string())
        throw std::runtime_error("apply_patch requires a string input");
      ParseDiagnostic diagnostic;
      auto patch =
          parse_patch(json["input"].get<std::string>(), true, diagnostic);
      if (!patch)
        throw std::runtime_error("Invalid patch at line " +
                                 std::to_string(diagnostic.line) + ": " +
                                 diagnostic.message);
      if (on_update)
        on_update(
            std::make_shared<TextToolResult>(json["input"].get<std::string>()));
      const auto applied = pi::core::apply_patch(cwd(), *patch);
      const auto count = applied.added.size() + applied.updated.size() +
                         applied.deleted.size() + applied.moved.size();
      return std::make_shared<TextToolResult>(
          "Applied patch: " + std::to_string(count) + " file(s)\n" +
          applied.unified_diff);
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

class EditTool final : public BuiltinTool {
public:
  explicit EditTool(const std::filesystem::path &cwd)
      : BuiltinTool(
            "edit",
            "Edit a single file using exact text replacement. Each "
            "edits[].oldText must match exactly once.",
            R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to edit (relative or absolute)"},"edits":{"type":"array","description":"One or more targeted replacements","items":{"type":"object","properties":{"oldText":{"type":"string","description":"Exact text to replace"},"newText":{"type":"string","description":"Replacement text"}},"required":["oldText","newText"],"additionalProperties":false}}},"required":["path","edits"],"additionalProperties":false})json",
            cwd, true) {}

  // Some models default to the old_text/new_text naming convention used by
  // other tools in the wild instead of this tool's camelCase oldText/
  // newText. Normalize the alias before schema validation runs rather than
  // hard-rejecting it.
  static void rename_snake_case_alias(nlohmann::json &obj, const char *snake,
                                      const char *camel) {
    if (obj.contains(snake) && !obj.contains(camel)) {
      obj[camel] = std::move(obj[snake]);
      obj.erase(snake);
    }
  }

  ToolArguments
  prepare_arguments(const ToolArguments &arguments) const override {
    auto prepared = arguments;
    rename_snake_case_alias(prepared, "old_text", "oldText");
    rename_snake_case_alias(prepared, "new_text", "newText");
    if (prepared.contains("oldText") && prepared.contains("newText") &&
        !prepared.contains("edits")) {
      prepared["edits"] =
          nlohmann::json::array({{{"oldText", prepared["oldText"]},
                                  {"newText", prepared["newText"]}}});
      prepared.erase("oldText");
      prepared.erase("newText");
    }
    if (prepared.contains("edits") && prepared["edits"].is_string()) {
      auto parsed = nlohmann::json::parse(prepared["edits"].get<std::string>(),
                                          nullptr, false);
      if (!parsed.is_discarded() && parsed.is_array()) {
        prepared["edits"] = std::move(parsed);
      }
    }
    if (prepared.contains("edits") && prepared["edits"].is_array()) {
      for (auto &edit : prepared["edits"]) {
        if (!edit.is_object())
          continue;
        rename_snake_case_alias(edit, "old_text", "oldText");
        rename_snake_case_alias(edit, "new_text", "newText");
      }
    }
    return prepared;
  }

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view args,
                                      std::stop_token,
                                      ToolUpdateCallback) const override {
    try {
      auto json = parse_args(args);
      json = prepare_arguments(json);
      const auto rel_path = json.value("path", "");
      const auto abs_path = resolve_workspace_path(rel_path);

      bool had_bom = false;
      const auto raw = read_text_file(abs_path);
      const auto no_bom = strip_bom(raw, had_bom);
      const auto lf_content = normalize_to_lf(no_bom);

      std::vector<std::pair<std::string, std::string>> edits;
      for (const auto &e : json.at("edits")) {
        edits.emplace_back(normalize_to_lf(e.value("oldText", std::string{})),
                           e.value("newText", std::string{}));
      }

      auto result = apply_edits(lf_content, edits, rel_path);

      // Restore BOM and original line endings
      const bool crlf = !no_bom.empty() && no_bom.contains("\r\n");
      if (crlf) {
        std::string restored;
        restored.reserve(result.size() + (result.size() / 40));
        for (char c : result) {
          if (c == '\n')
            restored += '\r';
          restored += c;
        }
        result = std::move(restored);
      }
      if (had_bom)
        result = "\xEF\xBB\xBF" + result;

      write_text_file(abs_path, result);
      return std::make_shared<TextToolResult>("Successfully replaced " +
                                              std::to_string(edits.size()) +
                                              " block(s) in " + rel_path);
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

class LsTool final : public BuiltinTool {
public:
  explicit LsTool(const std::filesystem::path &cwd)
      : BuiltinTool(
            "ls",
            "List directory contents. Returns entries sorted alphabetically, "
            "with '/' suffix for directories.",
            R"json({"type":"object","properties":{"path":{"type":"string","description":"Directory to list (default: current directory)"},"limit":{"type":"number","description":"Maximum number of entries to return (default: 500)"}},"additionalProperties":false})json",
            cwd, true) {}

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view args,
                                      std::stop_token,
                                      ToolUpdateCallback) const override {
    try {
      const auto json = parse_args(args);
      const auto path = resolve_workspace_path(json.value("path", "."));
      const auto limit = std::max(1, json.value("limit", kLsDefaultLimit));
      if (!std::filesystem::is_directory(path)) {
        throw std::runtime_error("Not a directory: " + path.string());
      }
      std::vector<std::string> entries;
      for (const auto &entry : std::filesystem::directory_iterator(path)) {
        auto name = entry.path().filename().string();
        if (entry.is_directory()) {
          name += '/';
        }
        entries.push_back(std::move(name));
      }
      std::ranges::sort(entries, {}, [](const std::string &value) {
        std::string lower = value; // NOLINT(clang-analyzer-cplusplus.Move): the
                                   // projection must preserve entries.
        std::ranges::transform(lower, lower.begin(), [](unsigned char ch) {
          return std::tolower(ch);
        });
        return lower;
      });
      if (entries.empty()) {
        return std::make_shared<TextToolResult>("(empty directory)");
      }
      std::ostringstream out;
      int emitted = 0;
      for (const auto &entry : entries) {
        if (emitted >= limit) {
          out << "\n[" << limit << " entries limit reached]";
          break;
        }
        if (emitted > 0) {
          out << '\n';
        }
        out << entry;
        ++emitted;
      }
      return std::make_shared<TextToolResult>(
          truncate_head(out.str(), static_cast<std::size_t>(limit), kMaxBytes));
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

class FindTool final : public BuiltinTool {
public:
  explicit FindTool(const std::filesystem::path &cwd)
      : BuiltinTool(
            "find",
            "Search for files by glob pattern, or by fuzzy name match when "
            "mode is 'fuzzy'. Returns matching file paths relative to the "
            "search directory, ranked by relevance in fuzzy mode.",
            R"json({"type":"object","properties":{"pattern":{"type":"string","description":"Glob pattern, or fuzzy query when mode is 'fuzzy'"},"path":{"type":"string","description":"Directory to search in (default: current directory)"},"limit":{"type":"number","description":"Maximum number of results (default: 1000)"},"mode":{"type":"string","enum":["glob","fuzzy"],"description":"Search mode (default: glob)"}},"required":["pattern"],"additionalProperties":false})json",
            cwd, true) {}

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view args,
                                      std::stop_token stop,
                                      ToolUpdateCallback) const override {
    try {
      const auto json = parse_args(args);
      const auto root = resolve_workspace_path(json.value("path", "."));
      const auto pattern = json.value("pattern", std::string{"*"});
      const auto mode = json.value("mode", std::string{"glob"});
      const bool fuzzy = mode == "fuzzy";
      const auto limit =
          std::max(1, json.value("limit", fuzzy ? kFuzzyFindDefaultLimit
                                                : kFindDefaultLimit));
      if (!std::filesystem::is_directory(root))
        throw std::runtime_error("Not a directory: " + root.string());
      GitIgnore gi;
      gi.load(cwd());
      if (root != cwd())
        gi.load(root);
      std::optional<std::regex> matcher;
      if (!fuzzy)
        matcher.emplace(glob_to_regex(pattern));
      struct Match {
        int score;
        std::string path;
      };
      struct Worse {
        bool operator()(const Match &a, const Match &b) const {
          return a.score > b.score || (a.score == b.score && a.path < b.path);
        }
      };
      std::priority_queue<Match, std::vector<Match>, Worse> top;
      std::vector<std::string> results;
      std::error_code ec;
      for (std::filesystem::recursive_directory_iterator it(
               root, std::filesystem::directory_options::skip_permission_denied,
               ec),
           end;
           it != end && !ec; it.increment(ec)) {
        if (stop.stop_requested())
          break;
        if (it->is_directory(ec) && (should_skip_dir(it->path()) ||
                                     gi.ignored(it->path(), root, true))) {
          it.disable_recursion_pending();
          continue;
        }
        if (!it->is_regular_file(ec) || gi.ignored(it->path(), root, false))
          continue;
        auto rel = relative_posix(it->path(), root);
        if (!fuzzy) {
          if (std::regex_match(rel, *matcher) ||
              std::regex_match(it->path().filename().generic_string(),
                               *matcher)) {
            results.push_back(std::move(rel));
            if (std::cmp_greater_equal(results.size(), limit))
              break;
          }
          continue;
        }
        const auto score = fuzzy_score_path(rel, pattern);
        if (!score)
          continue;
        top.push({*score, std::move(rel)});
        if (std::cmp_greater(top.size(), limit))
          top.pop();
      }
      if (fuzzy) {
        while (!top.empty()) {
          results.push_back(top.top().path);
          top.pop();
        }
        std::ranges::sort(results, [&](const auto &a, const auto &b) {
          const auto sa = fuzzy_score_path(a, pattern).value_or(0);
          const auto sb = fuzzy_score_path(b, pattern).value_or(0);
          return sa > sb || (sa == sb && a < b);
        });
      } else {
        std::ranges::sort(results);
      }
      if (results.empty())
        return std::make_shared<TextToolResult>(
            "No files found matching pattern");
      std::ostringstream out;
      for (std::size_t i = 0; i < results.size(); ++i) {
        if (i > 0)
          out << '\n';
        out << results[i];
      }
      if (std::cmp_greater_equal(results.size(), limit))
        out << "\n\n[" << limit << " results limit reached]";
      return std::make_shared<TextToolResult>(
          truncate_head(out.str(), limit, kMaxBytes));
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

class GrepTool final : public BuiltinTool {
public:
  explicit GrepTool(const std::filesystem::path &cwd)
      : BuiltinTool(
            "grep",
            "Search file contents for a pattern. Returns matching lines with "
            "file paths and line numbers.",
            R"json({"type":"object","properties":{"pattern":{"type":"string","description":"Search pattern (regex or literal string)"},"path":{"type":"string","description":"Directory or file to search (default: current directory)"},"glob":{"type":"string","description":"Filter files by glob pattern, e.g. '*.cpp'"},"ignoreCase":{"type":"boolean","description":"Case-insensitive search (default: false)"},"literal":{"type":"boolean","description":"Treat pattern as literal string instead of regex (default: false)"},"context":{"type":"number","description":"Number of lines to show before and after each match (default: 0)"},"limit":{"type":"number","description":"Maximum number of matches to return (default: 100)"}},"required":["pattern"],"additionalProperties":false})json",
            cwd, true) {}

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view args,
                                      std::stop_token,
                                      ToolUpdateCallback) const override {
    try {
      const auto json = parse_args(args);
      const auto root = resolve_workspace_path(json.value("path", "."));
      const auto pattern = json.value("pattern", std::string{});
      const auto glob = json.value("glob", std::string{});
      const auto ignore_case = json.value("ignoreCase", false);
      const auto literal = json.value("literal", false);
      const auto context = std::max(0, json.value("context", 0));
      const auto limit = std::max(1, json.value("limit", kGrepDefaultLimit));
      const auto flags =
          ignore_case ? std::regex::icase : std::regex::ECMAScript;
      std::regex matcher;
      try {
        matcher = std::regex(
            literal ? std::regex_replace(pattern,
                                         std::regex(R"([.^$|()\\[\]{}*+?])"),
                                         R"(\$&)")
                    : pattern,
            flags);
      } catch (const std::regex_error &) {
        throw std::runtime_error(
            "pattern is not valid regex: \"" + pattern +
            "\". If you meant to search for it literally (e.g. a function "
            "call or symbol like \"foo(\"), pass literal: true instead of "
            "escaping the regex metacharacters yourself.");
      }
      const std::optional<std::regex> glob_matcher =
          glob.empty() ? std::nullopt
                       : std::optional<std::regex>(glob_to_regex(glob));

      GitIgnore gi;
      gi.load(cwd());
      if (root != cwd())
        gi.load(root);
      std::vector<std::filesystem::path> files;
      std::error_code ec;
      if (std::filesystem::is_regular_file(root, ec)) {
        files.push_back(root);
      } else if (std::filesystem::is_directory(root, ec)) {
        for (std::filesystem::recursive_directory_iterator
                 it(root,
                    std::filesystem::directory_options::skip_permission_denied,
                    ec),
             end;
             it != end && !ec; it.increment(ec)) {
          if (it->is_directory(ec) && (should_skip_dir(it->path()) ||
                                       gi.ignored(it->path(), root, true))) {
            it.disable_recursion_pending();
            continue;
          }
          if (!it->is_regular_file(ec))
            continue;
          if (gi.ignored(it->path(), root, false))
            continue;
          const auto rel = relative_posix(it->path(), root);
          if (!glob_matcher || std::regex_match(rel, *glob_matcher) ||
              std::regex_match(it->path().filename().generic_string(),
                               *glob_matcher)) {
            files.push_back(it->path());
          }
        }
      } else {
        throw std::runtime_error("Path not found: " + root.string());
      }

      std::ostringstream out;
      int matches = 0;
      for (const auto &file : files) {
        const auto content = read_text_file(file);
        const auto lines = split_lines(content);
        for (std::size_t i = 0; i < lines.size(); ++i) {
          if (!std::regex_search(lines[i], matcher)) {
            continue;
          }
          const auto start = std::cmp_greater(i, context)
                                 ? i - static_cast<std::size_t>(context)
                                 : 0;
          const auto end =
              std::min(lines.size() - 1, i + static_cast<std::size_t>(context));
          for (std::size_t line = start; line <= end; ++line) {
            if (out.tellp() > 0) {
              out << '\n';
            }
            out << relative_posix(file, std::filesystem::is_directory(root)
                                            ? root
                                            : root.parent_path())
                << (line == i ? ":" : "-") << (line + 1)
                << (line == i ? ": " : "- ") << lines[line];
          }
          ++matches;
          if (matches >= limit) {
            out << "\n\n[" << limit
                << " matches limit reached. Refine pattern or increase limit.]";
            return std::make_shared<TextToolResult>(
                truncate_head(out.str(), kMaxBytes, kMaxBytes));
          }
        }
      }
      if (matches == 0) {
        return std::make_shared<TextToolResult>("No matches found");
      }
      return std::make_shared<TextToolResult>(
          truncate_head(out.str(), kMaxBytes, kMaxBytes));
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

// Relative path (from `cwd`) of the directory bash spill files live under.
// Kept workspace-relative and named consistently so should_skip_dir() (which
// excludes ".pici" by name) and the spill-file helpers below always agree on
// where it is.
std::filesystem::path bash_spill_dir(const std::filesystem::path &cwd) {
  return cwd / ".pici" / "bash-output";
}

// Deletes spill files older than 24h under <cwd>/.pici/bash-output/.
//
// Spill files now live inside the workspace (not host /tmp, which at least
// had OS-level tmpfs/tmp-cleaner conventions backing "eventually goes
// away"), so they need an explicit retention policy or they'd accumulate in
// the user's repo forever. This is invoked once per BashTool construction
// (i.e. once per session in the normal CLI flow) rather than once per bash
// call: it's an O(files-in-directory) walk, cheap at session-start cadence
// but wasteful to repeat on every single command. It intentionally doesn't
// try to be smarter (e.g. deleting a spill file as soon as its result is
// durably persisted) -- that refinement is left for later; simple
// age-based cleanup is enough to avoid "never cleaned up" against a
// directory that now lives in the user's repo.
void sweep_old_spill_files(const std::filesystem::path &cwd) {
  std::error_code ec;
  const auto dir = bash_spill_dir(cwd);
  if (!std::filesystem::exists(dir, ec) || ec)
    return;
  const auto cutoff =
      std::filesystem::file_time_type::clock::now() - std::chrono::hours(24);
  std::filesystem::directory_iterator it(dir, ec);
  if (ec)
    return;
  for (const auto &entry : it) {
    std::error_code entry_ec;
    if (!entry.is_regular_file(entry_ec))
      continue;
    const auto mtime = std::filesystem::last_write_time(entry.path(), entry_ec);
    if (entry_ec)
      continue;
    if (mtime < cutoff)
      std::filesystem::remove(entry.path(), entry_ec);
  }
}

// An open bash-output spill file: `fd` stays open (and positioned for
// sequential appends) for the lifetime of BashTool::execute's read loop;
// `path` is its final, absolute on-disk path.
struct SpillFile {
  int fd = -1;
  std::filesystem::path path;
};

// Creates a new, uniquely-named spill file directly at its final path under
// <cwd>/.pici/bash-output/ using mkstemp, mirroring the precedent in
// credential_store.cpp's save_document(): mkstemp + fchmod(0600), with the
// temp file removed on any failure path. Unlike credential_store's usage,
// there is no following rename -- this file *is* the final artifact, so
// composing a filename (e.g. from generate_session_id()) and opening it
// with a plain std::ofstream would be a symlink-plant hazard once the path
// lives inside a directory anything else could write to; mkstemp creates
// and opens the uniquely-named file atomically, so there's no window where
// a predictable path exists for something else to have planted first.
SpillFile create_spill_file(const std::filesystem::path &cwd) {
  const auto dir = bash_spill_dir(cwd);
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  if (ec)
    throw std::runtime_error("Failed to create bash spill directory: " +
                             ec.message());

  auto tmpl = dir / "spill-XXXXXX";
  std::string tmpl_string = tmpl.string();
  std::vector<char> buffer(tmpl_string.begin(), tmpl_string.end());
  buffer.push_back('\0');
  // mkstemp is a POSIX extension declared via <cstdlib> on this platform;
  // the tool has no better header to suggest for it.
  // NOLINTNEXTLINE(misc-include-cleaner)
  const int fd = ::mkstemp(buffer.data());
  if (fd < 0)
    throw std::runtime_error("Failed to create bash spill file");
  const std::filesystem::path spill_path(buffer.data());
  if (::fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
    ::close(fd);
    std::filesystem::remove(spill_path, ec);
    throw std::runtime_error("Failed to secure bash spill file");
  }
  return {fd, spill_path};
}

// PICI_TEST_MAX_SPILL_BYTES lets tests (see
// Bash_runaway_output_is_bounded_by_spill_cap) exercise the kill-on-spill-
// cap path with a small cap instead of waiting on a real 16MB write. Not a
// documented user-facing knob; read once per bash call, which is cheap and
// avoids threading a test-only constructor parameter through
// create_all_tools/create_coding_tools and BashTool's public constructor.
std::size_t effective_max_spill_bytes() {
  // NOLINTNEXTLINE(concurrency-mt-unsafe): read-only env access; nothing in
  // this process calls setenv/putenv concurrently with tool execution.
  if (const char *override_bytes = std::getenv("PICI_TEST_MAX_SPILL_BYTES")) {
    char *end = nullptr;
    const auto parsed = std::strtoull(override_bytes, &end, 10);
    if (end != override_bytes && parsed > 0)
      return static_cast<std::size_t>(parsed);
  }
  return kMaxSpillBytes;
}

class BashTool final : public BuiltinTool {
public:
  explicit BashTool(const std::filesystem::path &cwd,
                    SandboxPolicyPtr sandbox_policy)
      : BuiltinTool(
            "bash",
            "Execute a bash command in the current working directory. Returns "
            "stdout and stderr. Optionally provide timeout in seconds.",
            R"json({"type":"object","properties":{"command":{"type":"string","description":"Bash command to execute"},"timeout":{"type":"number","description":"Timeout in seconds (optional)"}},"required":["command"],"additionalProperties":false})json",
            cwd, true),
        sandbox_policy_(std::move(sandbox_policy)) {
    sweep_old_spill_files(this->cwd());
  }

  std::shared_ptr<ToolResult>
  execute(std::string_view, std::string_view args, std::stop_token stop_tok,
          ToolUpdateCallback on_update) const override {
    try {
      const auto json = parse_args(args);
      const auto command = json.value("command", std::string{});
      if (command.empty())
        throw std::runtime_error("command must not be empty");

      const int timeout_secs = json.value("timeout", 120);
      const auto mode = sandbox_policy_->mode();
      SandboxLauncher::validate(mode);

      // Create pipe for stdout+stderr
      std::array<int, 2> pipefd{};
      if (::pipe(pipefd.data()) != 0)
        throw std::runtime_error("Failed to create pipe");

      const auto pid = ::fork();
      if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        throw std::runtime_error("Failed to fork");
      }

      if (pid == 0) {
        // Child: new process group so we can kill the whole tree
        ::setpgid(0, 0);
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::dup2(pipefd[1], STDERR_FILENO);
        ::close(pipefd[1]);
        SandboxLauncher::exec(
            SandboxCommand{.mode = mode, .cwd = cwd(), .command = command});
      }

      // Parent
      ::close(pipefd[1]);

      auto kill_tree = [pid]() {
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX signal declarations vary.
        ::kill(-pid, SIGTERM);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // NOLINTNEXTLINE(misc-include-cleaner): POSIX signal declarations vary.
        ::kill(-pid, SIGKILL);
      };

      // NOLINTNEXTLINE(misc-include-cleaner): POSIX fcntl declarations vary.
      ::fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

      // Self-pipe: stop_token callback writes here to unblock poll immediately.
      std::array<int, 2> wakeup{};
      if (::pipe(wakeup.data()) != 0) {
        ::close(pipefd[0]);
        ::waitpid(pid, nullptr, 0);
        throw std::runtime_error("Failed to create wakeup pipe");
      }

      std::string output;
      output.reserve(4096);
      std::array<char, 4096> buf{};
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::seconds(timeout_secs);
      bool aborted = false;
      bool timed_out = false;

      // Once `output` (the small, in-memory/visible-content cap) is first
      // crossed, further chunks are appended to an on-disk spill file
      // instead of to `output` -- see kMaxSpillBytes for why a distinct,
      // much larger cap still bounds this instead of letting a runaway
      // producer (e.g. `yes`) write unbounded data for the full timeout.
      // `output` itself is left frozen at whatever it held at that moment
      // (later trimmed to exactly kMaxBytes by truncate_head_ex below) --
      // it's the visible/truncated preview; the spill file is the full
      // stream.
      std::optional<SpillFile> spill;
      std::size_t spill_bytes = 0;
      bool spill_truncated = false;

      // Appends one chunk of freshly-read output to whichever of
      // `output`/the spill file is currently active, lazily creating (and
      // seeding with everything accumulated in `output` so far) the spill
      // file the moment `output` first reaches kMaxBytes. Returns true if
      // the spill cap was just hit, in which case the caller must kill the
      // child and stop polling -- exactly as it already does on timeout.
      // `allow_on_update` preserves the existing behavior of only
      // streaming updates while `output` hasn't yet been capped: once it
      // has, `output` no longer changes, so repeating the same on_update
      // call would add nothing (and, once the loop no longer exits early,
      // could otherwise fire a very large number of times).
      auto handle_chunk = [&](const char *data, std::size_t len,
                              bool allow_on_update) {
        if (output.size() < kMaxBytes) {
          output.append(data, len);
          if (allow_on_update && on_update)
            on_update(std::make_shared<TextToolResult>(output));
          if (output.size() >= kMaxBytes) {
            spill = create_spill_file(cwd());
            write_best_effort(spill->fd, output.data(), output.size());
            spill_bytes = output.size();
            // The seed alone can already reach a small (e.g. test-only)
            // spill cap; check immediately rather than waiting for the
            // next chunk.
            if (spill_bytes >= effective_max_spill_bytes())
              return true;
          }
          return false;
        }
        // `output` is already frozen at/above kMaxBytes; `spill` must
        // already be open (created the moment that first happened, above).
        write_best_effort(spill->fd, data, len);
        spill_bytes += len;
        return spill_bytes >= effective_max_spill_bytes();
      };

      {
        // stop_callback writes to the wakeup pipe; destructor unregisters it
        // and synchronises with any in-progress invocation before we close.
        std::stop_callback sc(stop_tok, [wfd = wakeup[1]] {
          char b = 1;
          write_best_effort(wfd, &b, 1);
        });

        while (true) {
          const auto now = std::chrono::steady_clock::now();
          if (now >= deadline) {
            kill_tree();
            timed_out = true;
            break;
          }
          const auto remaining_ms =
              std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                    now)
                  .count();

          std::array<struct pollfd, 2> fds = {{
              {.fd = pipefd[0], .events = POLLIN | POLLHUP, .revents = 0},
              {.fd = wakeup[0], .events = POLLIN, .revents = 0},
          }};
          const int n = ::poll(fds.data(), static_cast<nfds_t>(fds.size()),
                               static_cast<int>(remaining_ms));

          if (n == 0) {
            kill_tree();
            timed_out = true;
            break;
          }
          if (n < 0 && errno == EINTR)
            continue;
          if (n < 0)
            break;

          if ((fds[1].revents & POLLIN) != 0) {
            kill_tree();
            aborted = true;
            break;
          }
          if ((fds[0].revents & POLLIN) != 0) {
            const auto nr = ::read(pipefd[0], buf.data(), buf.size());
            if (nr <= 0)
              break;
            if (handle_chunk(buf.data(), static_cast<std::size_t>(nr),
                             /*allow_on_update=*/true)) {
              kill_tree();
              spill_truncated = true;
              break;
            }
          } else if ((fds[0].revents & POLLHUP) != 0) {
            break;
          }
        }
      } // sc destroyed here — safe to close wakeup pipe

      ::close(wakeup[0]);
      ::close(wakeup[1]);

      // Drain any data that arrived between the last read and process exit.
      // pipefd[0] is O_NONBLOCK, so this terminates as soon as it hits
      // EAGAIN/EOF regardless of how much has been read -- no additional
      // wall-clock risk from applying the same spill-aware logic here in
      // place of the old `output.size() < kMaxBytes` guard.
      while (true) {
        const auto nr = ::read(pipefd[0], buf.data(), buf.size());
        if (nr <= 0)
          break;
        if (handle_chunk(buf.data(), static_cast<std::size_t>(nr),
                         /*allow_on_update=*/false)) {
          kill_tree();
          spill_truncated = true;
          break;
        }
      }
      ::close(pipefd[0]);
      if (spill)
        ::close(spill->fd);

      int status = 0;
      ::waitpid(pid, &status, 0);

      if (output.empty())
        output = "(no output)";
      auto truncated = truncate_head_ex(output, kMaxBytes, kMaxBytes);
      output = std::move(truncated.content);

      // Structured sidecar mirroring the human-readable "[Truncated: ...]"
      // footer above, for callers that want to act on truncation instead of
      // just reading the note. NOTE: an after-tool Lua hook can overwrite
      // `details` on the returned result (see agent_loop.cpp), which would
      // clobber this sidecar -- not guarded against here.
      std::optional<std::string> details;
      // A spill file only ever exists because `output` reached kMaxBytes at
      // some point (see handle_chunk above), which by construction means
      // more was produced than is visible in `output` -- so a spill implies
      // truncation even in the one case where truncate_head_ex's own
      // `truncated` flag says otherwise: `output` landing on *exactly*
      // kMaxBytes bytes with zero overshoot (a real, observed case --
      // `yes`'s 2-byte lines read in exact 4096-byte chunks can land
      // precisely on the 64KB boundary), which truncate_head_ex correctly
      // treats as "fits exactly, no [in-band] truncation note needed" for
      // its own generic callers, but which must still surface the spill
      // metadata here.
      const bool effectively_truncated = truncated.info.truncated || spill.has_value();
      if (effectively_truncated) {
        nlohmann::json details_json;
        details_json["truncation"] = {
            {"truncated", true},
            {"truncated_by",
             truncated.info.truncated_by.value_or(spill ? "bytes" : "")},
            {"input_lines", truncated.info.input_lines},
            {"input_bytes", truncated.info.input_bytes},
            {"output_lines", truncated.info.output_lines},
            {"output_bytes", truncated.info.output_bytes},
            {"last_line_partial", truncated.info.last_line_partial},
            {"first_line_exceeds_limit",
             truncated.info.first_line_exceeds_limit},
            {"max_lines", truncated.info.max_lines},
            {"max_bytes", truncated.info.max_bytes},
        };
        if (spill) {
          // Workspace-relative, deliberately: this is what makes the path
          // resolvable by ReadTool's resolve_workspace_path (and reachable
          // by a sandboxed `bash cat`, which mounts the workspace at the
          // same relative layout) -- an absolute host path would be neither.
          const auto rel_path =
              std::filesystem::relative(spill->path, cwd()).generic_string();
          details_json["full_output_path"] = rel_path;
          if (spill_truncated)
            details_json["spill_truncated"] = true;
          // Additional bash-specific footer (appended the same way as the
          // "[Command timed out...]"/"[Command aborted]" notes below) name
          // the full-output path; distinct from truncate_head_ex's own
          // generic "[Truncated: ... KB limit)]" note already folded into
          // `output` above, which doesn't know about spill files at all.
          output += "\n\n[Truncated: " +
                    std::to_string(truncated.info.output_bytes) + " of " +
                    std::to_string(spill_bytes) +
                    " bytes shown. Full output: " + rel_path + "]";
          if (spill_truncated)
            output += " (incomplete: exceeded spill cap)";
        }
        details = details_json.dump();
      }

      if (aborted) {
        output += "\n\n[Command aborted]";
        return std::make_shared<TextToolResult>(output, true, details);
      }
      if (timed_out) {
        output += "\n\n[Command timed out after " +
                  std::to_string(timeout_secs) + "s]";
        return std::make_shared<TextToolResult>(output, true, details);
      }
      if (status != 0 && WIFEXITED(status)) { // NOLINT(misc-include-cleaner)
        output +=
            "\n\nCommand exited with code " +
            std::to_string(WEXITSTATUS(status)); // NOLINT(misc-include-cleaner)
        return std::make_shared<TextToolResult>(output, true, details);
      }
      return std::make_shared<TextToolResult>(output, false, details);
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }

private:
  SandboxPolicyPtr sandbox_policy_;
};

} // namespace

std::vector<std::shared_ptr<const ToolDefinition>>
create_coding_tools(const std::filesystem::path &cwd,
                    SandboxPolicyPtr sandbox_policy,
                    std::shared_ptr<const SkillCatalog> skills) {
  if (!sandbox_policy)
    sandbox_policy = std::make_shared<SandboxPolicy>();
  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  tools.push_back(std::make_shared<ReadTool>(cwd));
  tools.push_back(std::make_shared<BashTool>(cwd, std::move(sandbox_policy)));
  tools.push_back(std::make_shared<ApplyPatchTool>(cwd));
  tools.push_back(std::make_shared<EditTool>(cwd));
  tools.push_back(std::make_shared<WriteTool>(cwd));
  if (skills != nullptr && !skills->skills.empty())
    tools.push_back(std::make_shared<SkillTool>(cwd, std::move(skills)));
  return tools;
}

std::vector<std::shared_ptr<const ToolDefinition>>
create_read_only_tools(const std::filesystem::path &cwd,
                       std::shared_ptr<const SkillCatalog> skills) {
  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  tools.push_back(std::make_shared<ReadTool>(cwd));
  tools.push_back(std::make_shared<GrepTool>(cwd));
  tools.push_back(std::make_shared<FindTool>(cwd));
  tools.push_back(std::make_shared<LsTool>(cwd));
  if (skills != nullptr && !skills->skills.empty())
    tools.push_back(std::make_shared<SkillTool>(cwd, std::move(skills)));
  return tools;
}

std::vector<std::shared_ptr<const ToolDefinition>>
create_all_tools(const std::filesystem::path &cwd,
                 SandboxPolicyPtr sandbox_policy,
                 std::shared_ptr<const SkillCatalog> skills) {
  auto tools =
      create_coding_tools(cwd, std::move(sandbox_policy), std::move(skills));
  tools.push_back(std::make_shared<GrepTool>(cwd));
  tools.push_back(std::make_shared<FindTool>(cwd));
  tools.push_back(std::make_shared<LsTool>(cwd));
  return tools;
}

} // namespace pi::core
