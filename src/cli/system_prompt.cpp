#include "cli/system_prompt.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace pi::cli {
namespace {

std::string current_date() {
  const auto now =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm local{};
  if (localtime_r(&now, &local) == nullptr)
    return "unknown";

  std::ostringstream result;
  result << std::put_time(&local, "%Y-%m-%d");
  return result.str();
}

std::string tool_description(std::string_view name) {
  if (name == "read")
    return "Read file contents";
  if (name == "bash")
    return "Execute a shell command";
  if (name == "edit")
    return "Apply an exact edit to a file";
  if (name == "write")
    return "Write a file";
  if (name == "grep")
    return "Search file contents";
  if (name == "find")
    return "Find files by name or pattern";
  if (name == "ls")
    return "List directory contents";
  return {};
}

} // namespace

std::string build_system_prompt(std::string_view custom_prompt,
                                const std::vector<std::string> &append_prompts,
                                const std::vector<ContextFile> &context_files,
                                const std::vector<std::string> &tool_names,
                                const std::filesystem::path &cwd) {
  std::string prompt;
  if (!custom_prompt.empty()) {
    prompt = std::string(custom_prompt);
  } else {
    prompt =
        "You are an expert coding assistant operating inside pici, a coding "
        "agent harness. You help users by reading files, executing commands, "
        "editing code, and writing new files.\n\nAvailable tools:\n";

    if (tool_names.empty()) {
      prompt += "(none)\n";
    } else {
      for (const auto &name : tool_names) {
        prompt += "- " + name;
        if (const auto description = tool_description(name);
            !description.empty()) {
          prompt += ": " + description;
        }
        prompt += "\n";
      }
    }

    prompt +=
        "\nIn addition to the tools above, you may have access to other "
        "custom tools depending on the project.\n\nGuidelines:\n"
        "- Use tools when they help fulfill the user's request.\n"
        "- Do not use tools for a simple greeting or general question when no "
        "workspace context is needed.\n"
        "- Do not modify files unless the user asks for a change or the task "
        "clearly requires it.\n"
        "- Do not re-read a file after editing or writing it to verify the "
        "change: edit/write fail loudly if the operation did not succeed, so "
        "a successful call already confirms the result.\n"
        "- Be concise in your responses.\n"
        "- Show file paths clearly when working with files.";
  }

  for (const auto &append : append_prompts) {
    if (!append.empty())
      prompt += "\n\n" + append;
  }

  if (!context_files.empty()) {
    prompt += "\n\n# Project Context\n\n";
    for (const auto &context : context_files) {
      prompt += "## " + context.path + "\n\n" + context.content + "\n\n";
    }
  }

  prompt += "\nCurrent date: " + current_date();
  prompt += "\nCurrent working directory: " + cwd.string();
  return prompt;
}

} // namespace pi::cli
