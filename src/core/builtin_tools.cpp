#include "core/builtin_tools.h"
#include "core/message_types.h"
#include "core/sandbox.h"
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
#include <sys/wait.h>
#include <unistd.h>

namespace pi::core {
namespace {

constexpr std::size_t kMaxBytes = static_cast<const std::size_t>(64 * 1024);
constexpr std::size_t kReadMaxLines = 2000;
constexpr int kLsDefaultLimit = 500;
constexpr int kFindDefaultLimit = 1000;
constexpr int kGrepDefaultLimit = 100;

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

std::string truncate_head(const std::string &content, std::size_t max_lines,
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

  if (hit_lines || hit_bytes) {
    if (hit_lines) {
      out += "\n\n[Truncated: showing " + std::to_string(max_lines) + " of " +
             std::to_string(total_lines) + " lines]";
    } else {
      out += "\n\n[Truncated: " + std::to_string(out_lines) + " lines shown (" +
             std::to_string(max_bytes / 1024) + "KB limit)]";
    }
  }
  return out;
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
         name == "build-asan";
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

class WriteTool final : public BuiltinTool {
public:
  explicit WriteTool(const std::filesystem::path &cwd)
      : BuiltinTool(
            "write",
            "Write content to a file. Creates parent directories and "
            "overwrites existing content.",
            R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to write (relative or absolute)"},"content":{"type":"string","description":"Content to write to the file"}},"required":["path","content"],"additionalProperties":false})json",
            cwd) {}

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
      if (edits.size() == 1)
        throw std::runtime_error("Could not find the text in " + path +
                                 ". The old text must match exactly including "
                                 "all whitespace and newlines.");
      throw std::runtime_error("Could not find edits[" + std::to_string(i) +
                               "] in " + path +
                               ". The oldText must match exactly including all "
                               "whitespace and newlines.");
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

class EditTool final : public BuiltinTool {
public:
  explicit EditTool(const std::filesystem::path &cwd)
      : BuiltinTool(
            "edit",
            "Edit a single file using exact text replacement. Each "
            "edits[].oldText must match exactly once.",
            R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to edit (relative or absolute)"},"edits":{"type":"array","description":"One or more targeted replacements","items":{"type":"object","properties":{"oldText":{"type":"string","description":"Exact text to replace"},"newText":{"type":"string","description":"Replacement text"}},"required":["oldText","newText"],"additionalProperties":false}}},"required":["path","edits"],"additionalProperties":false})json",
            cwd) {}

  ToolArguments
  prepare_arguments(const ToolArguments &arguments) const override {
    auto prepared = arguments;
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
            "Search for files by glob pattern. Returns matching file paths "
            "relative to the search directory.",
            R"json({"type":"object","properties":{"pattern":{"type":"string","description":"Glob pattern to match files, e.g. '*.cpp' or 'src/**/*.h'"},"path":{"type":"string","description":"Directory to search in (default: current directory)"},"limit":{"type":"number","description":"Maximum number of results (default: 1000)"}},"required":["pattern"],"additionalProperties":false})json",
            cwd, true) {}

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view args,
                                      std::stop_token,
                                      ToolUpdateCallback) const override {
    try {
      const auto json = parse_args(args);
      const auto root = resolve_workspace_path(json.value("path", "."));
      const auto pattern = json.value("pattern", std::string{"*"});
      const auto limit = std::max(1, json.value("limit", kFindDefaultLimit));
      if (!std::filesystem::is_directory(root)) {
        throw std::runtime_error("Not a directory: " + root.string());
      }
      GitIgnore gi;
      gi.load(cwd());
      if (root != cwd())
        gi.load(root);
      const std::regex matcher(glob_to_regex(pattern));
      std::vector<std::string> results;
      std::error_code ec;
      for (std::filesystem::recursive_directory_iterator it(
               root, std::filesystem::directory_options::skip_permission_denied,
               ec),
           end;
           it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec) && (should_skip_dir(it->path()) ||
                                     gi.ignored(it->path(), root, true))) {
          it.disable_recursion_pending();
          continue;
        }
        if (!it->is_regular_file(ec)) {
          continue;
        }
        if (gi.ignored(it->path(), root, false))
          continue;
        auto rel = relative_posix(it->path(), root);
        if (std::regex_match(rel, matcher) ||
            std::regex_match(it->path().filename().generic_string(), matcher)) {
          results.push_back(std::move(rel));
          if (std::cmp_greater_equal(results.size(), limit)) {
            break;
          }
        }
      }
      if (results.empty()) {
        return std::make_shared<TextToolResult>(
            "No files found matching pattern");
      }
      std::ranges::sort(results);
      std::ostringstream out;
      for (std::size_t i = 0; i < results.size(); ++i) {
        if (i > 0) {
          out << '\n';
        }
        out << results[i];
      }
      if (std::cmp_greater_equal(results.size(), limit)) {
        out << "\n\n[" << limit << " results limit reached]";
      }
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
      const std::regex matcher(
          literal ? std::regex_replace(
                        pattern, std::regex(R"([.^$|()\\[\]{}*+?])"), R"(\$&)")
                  : pattern,
          flags);
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

class BashTool final : public BuiltinTool {
public:
  explicit BashTool(const std::filesystem::path &cwd,
                    SandboxPolicyPtr sandbox_policy)
      : BuiltinTool(
            "bash",
            "Execute a bash command in the current working directory. Returns "
            "stdout and stderr. Optionally provide timeout in seconds.",
            R"json({"type":"object","properties":{"command":{"type":"string","description":"Bash command to execute"},"timeout":{"type":"number","description":"Timeout in seconds (optional)"}},"required":["command"],"additionalProperties":false})json",
            cwd),
        sandbox_policy_(std::move(sandbox_policy)) {}

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
            output.append(buf.data(), static_cast<std::size_t>(nr));
            if (on_update)
              on_update(std::make_shared<TextToolResult>(output));
            if (output.size() >= kMaxBytes)
              break;
          } else if ((fds[0].revents & POLLHUP) != 0) {
            break;
          }
        }
      } // sc destroyed here — safe to close wakeup pipe

      ::close(wakeup[0]);
      ::close(wakeup[1]);

      // Drain any data that arrived between the last read and process exit
      while (output.size() < kMaxBytes) {
        const auto nr = ::read(pipefd[0], buf.data(), buf.size());
        if (nr <= 0)
          break;
        output.append(buf.data(), static_cast<std::size_t>(nr));
      }
      ::close(pipefd[0]);

      int status = 0;
      ::waitpid(pid, &status, 0);

      if (output.empty())
        output = "(no output)";
      output = truncate_head(output, kMaxBytes, kMaxBytes);

      if (aborted) {
        output += "\n\n[Command aborted]";
        return std::make_shared<TextToolResult>(output, true);
      }
      if (timed_out) {
        output += "\n\n[Command timed out after " +
                  std::to_string(timeout_secs) + "s]";
        return std::make_shared<TextToolResult>(output, true);
      }
      if (status != 0 && WIFEXITED(status)) { // NOLINT(misc-include-cleaner)
        output +=
            "\n\nCommand exited with code " +
            std::to_string(WEXITSTATUS(status)); // NOLINT(misc-include-cleaner)
        return std::make_shared<TextToolResult>(output, true);
      }
      return std::make_shared<TextToolResult>(output);
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
                    SandboxPolicyPtr sandbox_policy) {
  if (!sandbox_policy)
    sandbox_policy = std::make_shared<SandboxPolicy>();
  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  tools.push_back(std::make_shared<ReadTool>(cwd));
  tools.push_back(std::make_shared<BashTool>(cwd, std::move(sandbox_policy)));
  tools.push_back(std::make_shared<EditTool>(cwd));
  tools.push_back(std::make_shared<WriteTool>(cwd));
  return tools;
}

std::vector<std::shared_ptr<const ToolDefinition>>
create_read_only_tools(const std::filesystem::path &cwd) {
  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  tools.push_back(std::make_shared<ReadTool>(cwd));
  tools.push_back(std::make_shared<GrepTool>(cwd));
  tools.push_back(std::make_shared<FindTool>(cwd));
  tools.push_back(std::make_shared<LsTool>(cwd));
  return tools;
}

std::vector<std::shared_ptr<const ToolDefinition>>
create_all_tools(const std::filesystem::path &cwd,
                 SandboxPolicyPtr sandbox_policy) {
  auto tools = create_coding_tools(cwd, std::move(sandbox_policy));
  tools.push_back(std::make_shared<GrepTool>(cwd));
  tools.push_back(std::make_shared<FindTool>(cwd));
  tools.push_back(std::make_shared<LsTool>(cwd));
  return tools;
}

} // namespace pi::core
