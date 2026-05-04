#include "core/builtin_tools.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/wait.h>

namespace pi::core {
namespace {

constexpr std::size_t kMaxBytes = 64 * 1024;
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
              std::filesystem::path cwd)
      : name_(std::move(name)), description_(std::move(description)),
        schema_(std::move(schema)), cwd_(std::filesystem::absolute(cwd)) {}

  std::string_view name() const override { return name_; }
  std::string_view description() const override { return description_; }
  ToolSchema &schema() const override { return schema_; }

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

    auto canonical_probe = std::filesystem::weakly_canonical(existing_probe, ec);
    if (ec) {
      canonical_probe = existing_probe.lexically_normal();
    }

    const auto rel = std::filesystem::relative(canonical_probe, canonical_cwd, ec);
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
  std::string out;
  std::size_t lines = 0;
  for (char ch : content) {
    if (out.size() >= max_bytes || lines >= max_lines) {
      out += "\n\n[Output truncated]";
      break;
    }
    out.push_back(ch);
    if (ch == '\n') {
      ++lines;
    }
  }
  return out;
}

std::string shell_quote(const std::string &value) {
  std::string quoted = "'";
  for (char ch : value) {
    if (ch == '\'') {
      quoted += "'\\''";
    } else {
      quoted.push_back(ch);
    }
  }
  quoted += "'";
  return quoted;
}

std::string glob_to_regex(std::string_view glob) {
  std::string out = "^";
  for (std::size_t i = 0; i < glob.size(); ++i) {
    const char ch = glob[i];
    if (ch == '*') {
      if (i + 1 < glob.size() && glob[i + 1] == '*') {
        out += ".*";
        ++i;
      } else {
        out += "[^/]*";
      }
    } else if (ch == '?') {
      out += "[^/]";
    } else if (std::string_view(".+()[]{}^$\\|").find(ch) !=
               std::string_view::npos) {
      out.push_back('\\');
      out.push_back(ch);
    } else {
      out.push_back(ch);
    }
  }
  out += "$";
  return out;
}

bool should_skip_dir(const std::filesystem::path &path) {
  const auto name = path.filename().string();
  return name == ".git" || name == "node_modules" || name == "build" ||
         name == "build-asan";
}

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
  explicit ReadTool(std::filesystem::path cwd)
      : BuiltinTool(
            "read",
            "Read the contents of a text file. Supports path, offset, and "
            "limit. Output is truncated for large files.",
            R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to read (relative or absolute)"},"offset":{"type":"number","description":"Line number to start reading from (1-indexed)"},"limit":{"type":"number","description":"Maximum number of lines to read"}},"required":["path"],"additionalProperties":false})json",
            std::move(cwd)) {}

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view args,
                                      std::stop_token,
                                      ToolUpdateCallback) const override {
    try {
      const auto json = parse_args(args);
      const auto path = resolve_workspace_path(json.value("path", ""));
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
      const auto end = std::min(lines.size(), start + static_cast<std::size_t>(limit));
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
      return std::make_shared<TextToolResult>(
          truncate_head(out.str(), kReadMaxLines, kMaxBytes));
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

class WriteTool final : public BuiltinTool {
public:
  explicit WriteTool(std::filesystem::path cwd)
      : BuiltinTool(
            "write",
            "Write content to a file. Creates parent directories and "
            "overwrites existing content.",
            R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to write (relative or absolute)"},"content":{"type":"string","description":"Content to write to the file"}},"required":["path","content"],"additionalProperties":false})json",
            std::move(cwd)) {}

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view args,
                                      std::stop_token,
                                      ToolUpdateCallback) const override {
    try {
      const auto json = parse_args(args);
      const auto path = resolve_workspace_path(json.value("path", ""));
      const auto content = json.value("content", std::string{});
      write_text_file(path, content);
      return std::make_shared<TextToolResult>("Successfully wrote " +
                                              std::to_string(content.size()) +
                                              " bytes to " +
                                              json.value("path", ""));
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

class EditTool final : public BuiltinTool {
public:
  explicit EditTool(std::filesystem::path cwd)
      : BuiltinTool(
            "edit",
            "Edit a single file using exact text replacement. Each "
            "edits[].oldText must match exactly once.",
            R"json({"type":"object","properties":{"path":{"type":"string","description":"Path to the file to edit (relative or absolute)"},"edits":{"type":"array","description":"One or more targeted replacements","items":{"type":"object","properties":{"oldText":{"type":"string","description":"Exact text to replace"},"newText":{"type":"string","description":"Replacement text"}},"required":["oldText","newText"],"additionalProperties":false}}},"required":["path","edits"],"additionalProperties":false})json",
            std::move(cwd)) {}

  ToolArguments prepare_arguments(const ToolArguments &arguments) const override {
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
      auto parsed =
          nlohmann::json::parse(prepared["edits"].get<std::string>(), nullptr,
                                false);
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
      const auto path = resolve_workspace_path(json.value("path", ""));
      auto content = read_text_file(path);
      int count = 0;
      for (const auto &edit : json.at("edits")) {
        const auto old_text = edit.value("oldText", std::string{});
        const auto new_text = edit.value("newText", std::string{});
        if (old_text.empty()) {
          throw std::runtime_error("oldText must not be empty");
        }
        const auto pos = content.find(old_text);
        if (pos == std::string::npos) {
          throw std::runtime_error("oldText did not match file content");
        }
        if (content.find(old_text, pos + old_text.size()) !=
            std::string::npos) {
          throw std::runtime_error("oldText matched more than once");
        }
        content.replace(pos, old_text.size(), new_text);
        ++count;
      }
      write_text_file(path, content);
      return std::make_shared<TextToolResult>("Successfully replaced " +
                                              std::to_string(count) +
                                              " block(s) in " +
                                              json.value("path", ""));
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

class LsTool final : public BuiltinTool {
public:
  explicit LsTool(std::filesystem::path cwd)
      : BuiltinTool(
            "ls",
            "List directory contents. Returns entries sorted alphabetically, "
            "with '/' suffix for directories.",
            R"json({"type":"object","properties":{"path":{"type":"string","description":"Directory to list (default: current directory)"},"limit":{"type":"number","description":"Maximum number of entries to return (default: 500)"}},"additionalProperties":false})json",
            std::move(cwd)) {}

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
          name += "/";
        }
        entries.push_back(std::move(name));
      }
      std::ranges::sort(entries, {}, [](const std::string &value) {
        std::string lower = value;
        std::ranges::transform(lower, lower.begin(),
                               [](unsigned char ch) { return std::tolower(ch); });
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
  explicit FindTool(std::filesystem::path cwd)
      : BuiltinTool(
            "find",
            "Search for files by glob pattern. Returns matching file paths "
            "relative to the search directory.",
            R"json({"type":"object","properties":{"pattern":{"type":"string","description":"Glob pattern to match files, e.g. '*.cpp' or 'src/**/*.h'"},"path":{"type":"string","description":"Directory to search in (default: current directory)"},"limit":{"type":"number","description":"Maximum number of results (default: 1000)"}},"required":["pattern"],"additionalProperties":false})json",
            std::move(cwd)) {}

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
      const std::regex matcher(glob_to_regex(pattern));
      std::vector<std::string> results;
      std::error_code ec;
      for (std::filesystem::recursive_directory_iterator it(
               root, std::filesystem::directory_options::skip_permission_denied,
               ec),
           end;
           it != end && !ec; it.increment(ec)) {
        if (it->is_directory(ec) && should_skip_dir(it->path())) {
          it.disable_recursion_pending();
          continue;
        }
        if (!it->is_regular_file(ec)) {
          continue;
        }
        auto rel = relative_posix(it->path(), root);
        if (std::regex_match(rel, matcher) ||
            std::regex_match(it->path().filename().generic_string(), matcher)) {
          results.push_back(std::move(rel));
          if (static_cast<int>(results.size()) >= limit) {
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
      if (static_cast<int>(results.size()) >= limit) {
        out << "\n\n[" << limit << " results limit reached]";
      }
      return std::make_shared<TextToolResult>(truncate_head(out.str(), limit,
                                                            kMaxBytes));
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

class GrepTool final : public BuiltinTool {
public:
  explicit GrepTool(std::filesystem::path cwd)
      : BuiltinTool(
            "grep",
            "Search file contents for a pattern. Returns matching lines with "
            "file paths and line numbers.",
            R"json({"type":"object","properties":{"pattern":{"type":"string","description":"Search pattern (regex or literal string)"},"path":{"type":"string","description":"Directory or file to search (default: current directory)"},"glob":{"type":"string","description":"Filter files by glob pattern, e.g. '*.cpp'"},"ignoreCase":{"type":"boolean","description":"Case-insensitive search (default: false)"},"literal":{"type":"boolean","description":"Treat pattern as literal string instead of regex (default: false)"},"context":{"type":"number","description":"Number of lines to show before and after each match (default: 0)"},"limit":{"type":"number","description":"Maximum number of matches to return (default: 100)"}},"required":["pattern"],"additionalProperties":false})json",
            std::move(cwd)) {}

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
      const auto flags = ignore_case ? std::regex::icase : std::regex::ECMAScript;
      const std::regex matcher(literal ? std::regex_replace(
                                            pattern,
                                            std::regex(R"([.^$|()\\[\]{}*+?])"),
                                            R"(\$&)")
                                      : pattern,
                                  flags);
      const std::optional<std::regex> glob_matcher =
          glob.empty() ? std::nullopt
                       : std::optional<std::regex>(glob_to_regex(glob));

      std::vector<std::filesystem::path> files;
      std::error_code ec;
      if (std::filesystem::is_regular_file(root, ec)) {
        files.push_back(root);
      } else if (std::filesystem::is_directory(root, ec)) {
        for (std::filesystem::recursive_directory_iterator it(
                 root,
                 std::filesystem::directory_options::skip_permission_denied,
                 ec),
             end;
             it != end && !ec; it.increment(ec)) {
          if (it->is_directory(ec) && should_skip_dir(it->path())) {
            it.disable_recursion_pending();
            continue;
          }
          if (!it->is_regular_file(ec)) {
            continue;
          }
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
          const auto start = i > static_cast<std::size_t>(context)
                                 ? i - static_cast<std::size_t>(context)
                                 : 0;
          const auto end = std::min(lines.size() - 1,
                                    i + static_cast<std::size_t>(context));
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
  explicit BashTool(std::filesystem::path cwd)
      : BuiltinTool(
            "bash",
            "Execute a bash command in the current working directory. Returns "
            "stdout and stderr. Optionally provide timeout in seconds.",
            R"json({"type":"object","properties":{"command":{"type":"string","description":"Bash command to execute"},"timeout":{"type":"number","description":"Timeout in seconds (optional)"}},"required":["command"],"additionalProperties":false})json",
            std::move(cwd)) {}

  std::shared_ptr<ToolResult> execute(std::string_view, std::string_view args,
                                      std::stop_token,
                                      ToolUpdateCallback) const override {
    try {
      const auto json = parse_args(args);
      const auto command = json.value("command", std::string{});
      if (command.empty()) {
        throw std::runtime_error("command must not be empty");
      }
      std::string wrapped = "cd " + shell_quote(cwd().string()) + " && ";
      if (json.contains("timeout")) {
        wrapped += "timeout " +
                   std::to_string(std::max(1, json.value("timeout", 0))) + " ";
      }
      wrapped += command + " 2>&1";
      FILE *pipe = ::popen(wrapped.c_str(), "r");
      if (pipe == nullptr) {
        throw std::runtime_error("Failed to start bash command");
      }
      std::array<char, 4096> buffer{};
      std::string output;
      while (std::fgets(buffer.data(), static_cast<int>(buffer.size()),
                        pipe) != nullptr) {
        output += buffer.data();
      }
      const int status = ::pclose(pipe);
      if (output.empty()) {
        output = "(no output)";
      }
      output = truncate_head(output, kMaxBytes, kMaxBytes);
      if (status != 0) {
        if (WIFEXITED(status)) {
          output += "\n\nCommand exited with code " +
                    std::to_string(WEXITSTATUS(status));
        }
        return std::make_shared<TextToolResult>(output, true);
      }
      return std::make_shared<TextToolResult>(output);
    } catch (const std::exception &err) {
      return error_result(err);
    }
  }
};

} // namespace

std::vector<std::shared_ptr<const ToolDefinition>>
create_coding_tools(std::filesystem::path cwd) {
  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  tools.push_back(std::make_shared<ReadTool>(cwd));
  tools.push_back(std::make_shared<BashTool>(cwd));
  tools.push_back(std::make_shared<EditTool>(cwd));
  tools.push_back(std::make_shared<WriteTool>(cwd));
  return tools;
}

std::vector<std::shared_ptr<const ToolDefinition>>
create_read_only_tools(std::filesystem::path cwd) {
  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  tools.push_back(std::make_shared<ReadTool>(cwd));
  tools.push_back(std::make_shared<GrepTool>(cwd));
  tools.push_back(std::make_shared<FindTool>(cwd));
  tools.push_back(std::make_shared<LsTool>(cwd));
  return tools;
}

std::vector<std::shared_ptr<const ToolDefinition>>
create_all_tools(std::filesystem::path cwd) {
  auto tools = create_coding_tools(cwd);
  tools.push_back(std::make_shared<GrepTool>(cwd));
  tools.push_back(std::make_shared<FindTool>(cwd));
  tools.push_back(std::make_shared<LsTool>(cwd));
  return tools;
}

} // namespace pi::core
