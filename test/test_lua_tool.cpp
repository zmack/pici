#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <source_location>
#include <string>
#include <string_view>

#include "core/lua_tool.h"

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

static std::filesystem::path write_lua(const std::filesystem::path &dir,
                                       const std::string &name,
                                       const std::string &src) {
  auto path = dir / name;
  std::ofstream(path) << src;
  return path;
}

void test_load_and_metadata() {
  tests::register_test("LuaTool: load and metadata", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-meta";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "greet.lua", R"lua(
return {
  name = "greet",
  description = "Returns a greeting",
  schema = '{"type":"object","properties":{"name":{"type":"string"}},"required":["name"],"additionalProperties":false}',
  execute = function(args)
    return "Hello, " .. args.name .. "!"
  end
}
)lua");

    auto tool = load_lua_tool(path);
    CHECK_EQ(std::string(tool->name()), "greet");
    CHECK_EQ(std::string(tool->description()), "Returns a greeting");
    CHECK(tool->schema().serialize().find("\"type\"") != std::string::npos);

    std::filesystem::remove_all(dir);
  });
}

void test_execute_string_return() {
  tests::register_test("LuaTool: execute returns string", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-str";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "echo.lua", R"lua(
return {
  name = "echo",
  description = "Echoes input",
  execute = function(args)
    return args.text
  end
}
)lua");

    auto tool = load_lua_tool(path);
    auto result = tool->execute("1", R"({"text":"hello world"})");
    CHECK(!result->is_error());
    CHECK_EQ(result->content(), "hello world");

    std::filesystem::remove_all(dir);
  });
}

void test_execute_table_return() {
  tests::register_test("LuaTool: execute returns table", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-tbl";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "fail.lua", R"lua(
return {
  name = "fail",
  description = "Always fails with details",
  execute = function(args)
    return { content = "something went wrong", is_error = true, details = "extra info" }
  end
}
)lua");

    auto tool = load_lua_tool(path);
    auto result = tool->execute("1", "{}");
    CHECK(result->is_error());
    CHECK_EQ(result->content(), "something went wrong");
    CHECK(result->details().has_value());
    CHECK_EQ(result->details().value(), "extra info");

    std::filesystem::remove_all(dir);
  });
}

void test_lua_runtime_error() {
  tests::register_test("LuaTool: Lua runtime error returns error result", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-err";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "boom.lua", R"lua(
return {
  name = "boom",
  description = "Crashes",
  execute = function(args)
    error("intentional crash")
  end
}
)lua");

    auto tool = load_lua_tool(path);
    auto result = tool->execute("1", "{}");
    CHECK(result->is_error());
    CHECK(result->content().find("intentional crash") != std::string::npos);

    std::filesystem::remove_all(dir);
  });
}

void test_load_syntax_error() {
  tests::register_test("LuaTool: syntax error throws", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-syn";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "bad.lua", "this is not valid lua !!!@#$");

    bool threw = false;
    try {
      load_lua_tool(path);
    } catch (const std::runtime_error &) {
      threw = true;
    }
    CHECK(threw);

    std::filesystem::remove_all(dir);
  });
}

void test_json_bridge() {
  tests::register_test("LuaTool: json module available in Lua", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-json";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "jsontest.lua", R"lua(
return {
  name = "jsontest",
  description = "Tests json module",
  execute = function(args)
    local t = { x = 1, y = "two" }
    local encoded = json.encode(t)
    local decoded = json.decode(encoded)
    return tostring(decoded.x) .. ":" .. decoded.y
  end
}
)lua");

    auto tool = load_lua_tool(path);
    auto result = tool->execute("1", "{}");
    CHECK(!result->is_error());
    CHECK_EQ(result->content(), "1:two");

    std::filesystem::remove_all(dir);
  });
}

void test_load_lua_tools_directory() {
  tests::register_test("load_lua_tools: loads all .lua files", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-dir";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    write_lua(dir, "tool_a.lua", R"lua(
return { name="tool_a", description="A", execute=function(a) return "a" end }
)lua");
    write_lua(dir, "tool_b.lua", R"lua(
return { name="tool_b", description="B", execute=function(a) return "b" end }
)lua");
    // non-.lua file should be ignored
    std::ofstream(dir / "ignored.txt") << "not a tool";
    // bad lua should be skipped
    write_lua(dir, "bad.lua", "syntax error !!!");

    auto tools = load_lua_tools(dir);
    CHECK_EQ(tools.size(), std::size_t(2));

    std::filesystem::remove_all(dir);
  });
}

int main() {
  std::cout << "=== pi-cpp lua tool tests ===\n\n";

  test_load_and_metadata();
  test_execute_string_return();
  test_execute_table_return();
  test_lua_runtime_error();
  test_load_syntax_error();
  test_json_bridge();
  test_load_lua_tools_directory();

  tests::print_summary();
  return tests::failed == 0 ? 0 : 1;
}
