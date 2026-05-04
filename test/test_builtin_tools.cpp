#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>

#include "core/builtin_tools.h"

using namespace pi::core;

namespace tests {

int passed{0};
int failed{0};
int total{0};
int current_failed{0};

bool CHECK_impl(bool cond, bool expected, std::string_view expr,
                std::source_location loc = std::source_location::current()) {
  if (cond != expected) {
    current_failed++;
    std::cerr << "  FAIL " << loc.file_name() << ":" << loc.line() << " - "
              << expr << " (expected " << expected << ", got " << cond
              << ")\n";
    return false;
  }
  return true;
}

#define CHECK(cond)                                                            \
  (::tests::CHECK_impl(static_cast<bool>(cond), true, #cond,                  \
                       std::source_location::current()))

#define CHECK_EQ(a, b)                                                         \
  (::tests::CHECK_impl((a) == (b), true, #a " == " #b,                        \
                       std::source_location::current()))

void register_test(std::string name, std::function<void()> fn) {
  total++;
  current_failed = 0;
  fn();
  if (current_failed == 0) {
    passed++;
    std::cout << "  PASS " << name << "\n";
  } else {
    failed++;
    std::cout << "  FAIL " << name << "\n";
  }
}

void print_summary() {
  std::cout << "\n========================================\n";
  std::cout << "  Tests: " << total << " total, " << passed << " passed, "
            << failed << " failed\n";
  std::cout << "========================================\n";
}

} // namespace tests

static std::shared_ptr<const ToolDefinition>
find_tool(const std::vector<std::shared_ptr<const ToolDefinition>> &tools,
          std::string_view name) {
  for (const auto &tool : tools) {
    if (tool->name() == name) {
      return tool;
    }
  }
  return {};
}

void test_tool_factories() {
  tests::register_test("Builtin tools: factory names", []() {
    const auto tools = create_all_tools();
    CHECK_EQ(tools.size(), std::size_t(7));
    CHECK(find_tool(tools, "read"));
    CHECK(find_tool(tools, "bash"));
    CHECK(find_tool(tools, "edit"));
    CHECK(find_tool(tools, "write"));
    CHECK(find_tool(tools, "grep"));
    CHECK(find_tool(tools, "find"));
    CHECK(find_tool(tools, "ls"));
  });
}

void test_file_tools() {
  tests::register_test("Builtin tools: read write edit", []() {
    const auto root = std::filesystem::temp_directory_path() /
                      "pici-builtin-tools-file-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto tools = create_all_tools(root);

    auto write = find_tool(tools, "write");
    auto read = find_tool(tools, "read");
    auto edit = find_tool(tools, "edit");

    auto write_result =
        write->execute("1", R"({"path":"notes/a.txt","content":"hello\nworld"})");
    CHECK(!write_result->is_error());

    auto read_result = read->execute("2", R"({"path":"notes/a.txt","limit":1})");
    CHECK(!read_result->is_error());
    CHECK(read_result->content().find("hello") != std::string::npos);
    CHECK(read_result->content().find("Use offset=2") != std::string::npos);

    auto edit_result = edit->execute(
        "3",
        R"({"path":"notes/a.txt","edits":[{"oldText":"world","newText":"there"}]})");
    CHECK(!edit_result->is_error());

    auto final_result = read->execute("4", R"({"path":"notes/a.txt"})");
    CHECK(final_result->content().find("there") != std::string::npos);
    std::filesystem::remove_all(root);
  });
}

void test_discovery_tools() {
  tests::register_test("Builtin tools: ls find grep", []() {
    const auto root = std::filesystem::temp_directory_path() /
                      "pici-builtin-tools-discovery-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "src");
    std::ofstream(root / "src" / "main.cpp") << "int main() { return 0; }\n";
    std::ofstream(root / "README.md") << "needle\n";

    const auto tools = create_all_tools(root);
    auto ls = find_tool(tools, "ls");
    auto find = find_tool(tools, "find");
    auto grep = find_tool(tools, "grep");

    auto ls_result = ls->execute("1", R"({"path":"."})");
    CHECK(ls_result->content().find("src/") != std::string::npos);

    auto find_result = find->execute("2", R"({"pattern":"**/*.cpp"})");
    CHECK(find_result->content().find("src/main.cpp") != std::string::npos);

    auto grep_result = grep->execute("3", R"({"pattern":"needle"})");
    CHECK(grep_result->content().find("README.md:1: needle") !=
          std::string::npos);
    std::filesystem::remove_all(root);
  });
}

void test_bash_tool() {
  tests::register_test("Builtin tools: bash", []() {
    const auto root =
        std::filesystem::temp_directory_path() / "pici-builtin-tools-bash-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto tools = create_all_tools(root);
    auto bash = find_tool(tools, "bash");
    auto result = bash->execute("1", R"({"command":"printf ok"})");
    CHECK(!result->is_error());
    CHECK_EQ(result->content(), "ok");
    std::filesystem::remove_all(root);
  });
}

int main() {
  test_tool_factories();
  test_file_tools();
  test_discovery_tools();
  test_bash_tool();

  tests::print_summary();
  return tests::failed == 0 ? 0 : 1;
}
