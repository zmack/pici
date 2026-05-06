#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

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

  tests::register_test("Edit tool: fuzzy match trailing whitespace", []() {
    const auto root = std::filesystem::temp_directory_path() / "pici-edit-fuzzy-ws";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto tools = create_all_tools(root);
    auto write = find_tool(tools, "write");
    auto edit  = find_tool(tools, "edit");
    auto read  = find_tool(tools, "read");

    // File has trailing spaces on lines; model's oldText won't include them
    write->execute("1", R"({"path":"f.txt","content":"line one   \nline two  \nline three"})");
    auto r = edit->execute("2",
        R"({"path":"f.txt","edits":[{"oldText":"line one\nline two","newText":"LINE ONE\nLINE TWO"}]})");
    CHECK(!r->is_error());
    auto content = read->execute("3", R"({"path":"f.txt"})");
    CHECK(content->content().find("LINE ONE") != std::string::npos);
    std::filesystem::remove_all(root);
  });

  tests::register_test("Edit tool: fuzzy match smart quotes", []() {
    const auto root = std::filesystem::temp_directory_path() / "pici-edit-fuzzy-quotes";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto tools = create_all_tools(root);
    auto write = find_tool(tools, "write");
    auto edit  = find_tool(tools, "edit");
    auto read  = find_tool(tools, "read");

    // File contains smart curly quotes; model sends straight ASCII quotes
    write->execute("1", R"({"path":"q.txt","content":"say “hello” to me"})");
    auto r = edit->execute("2",
        R"({"path":"q.txt","edits":[{"oldText":"say \"hello\" to me","newText":"say \"world\" to me"}]})");
    CHECK(!r->is_error());
    auto content = read->execute("3", R"({"path":"q.txt"})");
    CHECK(content->content().find("world") != std::string::npos);
    std::filesystem::remove_all(root);
  });

  tests::register_test("Edit tool: multiple edits applied to same base", []() {
    const auto root = std::filesystem::temp_directory_path() / "pici-edit-multi";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto tools = create_all_tools(root);
    auto write = find_tool(tools, "write");
    auto edit  = find_tool(tools, "edit");
    auto read  = find_tool(tools, "read");

    write->execute("1", R"({"path":"m.txt","content":"alpha\nbeta\ngamma"})");
    auto r = edit->execute("2", R"({"path":"m.txt","edits":[
      {"oldText":"alpha","newText":"ALPHA"},
      {"oldText":"gamma","newText":"GAMMA"}
    ]})");
    CHECK(!r->is_error());
    auto content = read->execute("3", R"({"path":"m.txt"})");
    CHECK(content->content().find("ALPHA") != std::string::npos);
    CHECK(content->content().find("GAMMA") != std::string::npos);
    CHECK(content->content().find("beta")  != std::string::npos);
    std::filesystem::remove_all(root);
  });

  tests::register_test("Edit tool: overlap detection", []() {
    const auto root = std::filesystem::temp_directory_path() / "pici-edit-overlap";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto tools = create_all_tools(root);
    auto write = find_tool(tools, "write");
    auto edit  = find_tool(tools, "edit");

    write->execute("1", R"({"path":"o.txt","content":"abcdef"})");
    auto r = edit->execute("2", R"({"path":"o.txt","edits":[
      {"oldText":"abcd","newText":"X"},
      {"oldText":"cdef","newText":"Y"}
    ]})");
    CHECK(r->is_error());
    std::filesystem::remove_all(root);
  });
}

void test_truncation_detail() {
  tests::register_test("Truncation detail: line limit message", []() {
    const auto root = std::filesystem::temp_directory_path() / "pici-trunc-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);
    const auto tools = create_all_tools(root);
    auto write = find_tool(tools, "write");
    auto read  = find_tool(tools, "read");

    // Write 2500 lines (exceeds default 2000-line limit)
    std::string content;
    for (int i = 1; i <= 2500; ++i)
      content += "line " + std::to_string(i) + "\n";
    write->execute("1", R"({"path":"big.txt","content":)" +
                         nlohmann::json(content).dump() + "}");

    auto r = read->execute("2", R"({"path":"big.txt"})");
    CHECK(!r->is_error());
    // Read tool shows "N more lines... Use offset=X" for its own line limit
    CHECK(r->content().find("more lines in file") != std::string::npos);
    CHECK(r->content().find("offset=") != std::string::npos);
    // And truncate_head adds detail when byte limit is hit on the assembled output
    // (this file is small enough to not hit byte limit, so just verify line limit works)
    CHECK(r->content().find("line 2500") == std::string::npos); // line 2500 not shown
    std::filesystem::remove_all(root);
  });
}

void test_gitignore() {
  tests::register_test("GitIgnore: find respects .gitignore", []() {
    const auto root = std::filesystem::temp_directory_path() / "pici-gitignore-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "src");
    std::filesystem::create_directories(root / "dist");
    std::filesystem::create_directories(root / "node_modules" / "pkg");
    std::ofstream(root / "src" / "main.cpp") << "int main() {}";
    std::ofstream(root / "dist" / "out.js") << "output";
    std::ofstream(root / "node_modules" / "pkg" / "index.js") << "pkg";
    std::ofstream(root / ".gitignore") << "dist/\nnode_modules/\n";

    const auto tools = create_all_tools(root);
    auto find = find_tool(tools, "find");
    auto r = find->execute("1", R"({"pattern":"*.cpp","path":"."})");
    CHECK(!r->is_error());
    CHECK(r->content().find("main.cpp") != std::string::npos);

    // dist and node_modules should be skipped
    auto r2 = find->execute("2", R"({"pattern":"*.js","path":"."})");
    CHECK(r2->content().find("out.js") == std::string::npos);
    CHECK(r2->content().find("index.js") == std::string::npos);

    std::filesystem::remove_all(root);
  });

  tests::register_test("GitIgnore: grep respects .gitignore", []() {
    const auto root = std::filesystem::temp_directory_path() / "pici-gitignore-grep";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "src");
    std::filesystem::create_directories(root / "vendor");
    std::ofstream(root / "src" / "main.cpp") << "needle";
    std::ofstream(root / "vendor" / "lib.cpp") << "needle";
    std::ofstream(root / ".gitignore") << "vendor/\n";

    const auto tools = create_all_tools(root);
    auto grep = find_tool(tools, "grep");
    auto r = grep->execute("1", R"({"pattern":"needle"})");
    CHECK(r->content().find("src/main.cpp") != std::string::npos);
    CHECK(r->content().find("vendor") == std::string::npos);
    std::filesystem::remove_all(root);
  });
}

void test_image_read() {
  tests::register_test("Read tool: image returns ImageContent block", []() {
    const auto root = std::filesystem::temp_directory_path() / "pici-image-test";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root);

    // Write a tiny valid PNG (1x1 white pixel)
    static const unsigned char kPng[] = {
        0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,
        0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
        0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,
        0xde,0x00,0x00,0x00,0x0c,0x49,0x44,0x41,
        0x54,0x08,0xd7,0x63,0xf8,0xff,0xff,0x3f,
        0x00,0x05,0xfe,0x02,0xfe,0xdc,0xcc,0x59,
        0xe7,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,
        0x44,0xae,0x42,0x60,0x82};
    std::ofstream f(root / "img.png", std::ios::binary);
    f.write(reinterpret_cast<const char*>(kPng), sizeof(kPng));
    f.close();

    const auto tools = create_all_tools(root);
    auto read = find_tool(tools, "read");
    auto r = read->execute("1", R"({"path":"img.png"})");
    CHECK(!r->is_error());
    // Should mention image in text content
    CHECK(r->content().find("image/png") != std::string::npos);
    // Should have two content blocks: text + image
    auto blocks = r->content_blocks();
    CHECK_EQ(blocks.size(), std::size_t(2));
    CHECK(std::holds_alternative<TextContent>(blocks[0]));
    CHECK(std::holds_alternative<ImageContent>(blocks[1]));
    const auto &img = std::get<ImageContent>(blocks[1]);
    CHECK(img.mime_type == "image/png");
    CHECK(!img.data.empty());

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

  tests::register_test("Bash tool: timeout kills command", []() {
    const auto tools = create_all_tools(std::filesystem::temp_directory_path());
    auto bash = find_tool(tools, "bash");
    auto start = std::chrono::steady_clock::now();
    auto result = bash->execute("1", R"({"command":"sleep 60","timeout":1})");
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();
    CHECK(result->is_error());
    CHECK(result->content().find("timed out") != std::string::npos);
    CHECK(elapsed < 5); // must not wait 60s
  });

  tests::register_test("Bash tool: stop_token aborts command", []() {
    const auto tools = create_all_tools(std::filesystem::temp_directory_path());
    auto bash = find_tool(tools, "bash");

    std::stop_source src;
    std::thread killer([&src]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
      src.request_stop();
    });

    auto start = std::chrono::steady_clock::now();
    auto result = bash->execute("1", R"({"command":"sleep 60"})", src.get_token(), {});
    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start).count();
    killer.join();

    CHECK(result->is_error());
    CHECK(result->content().find("aborted") != std::string::npos);
    CHECK(elapsed < 5);
  });

  tests::register_test("Bash tool: exit code propagated", []() {
    const auto tools = create_all_tools(std::filesystem::temp_directory_path());
    auto bash = find_tool(tools, "bash");
    auto result = bash->execute("1", R"({"command":"exit 42"})");
    CHECK(result->is_error());
    CHECK(result->content().find("42") != std::string::npos);
  });
}

int main() {
  test_tool_factories();
  test_file_tools();
  test_truncation_detail();
  test_gitignore();
  test_image_read();
  test_discovery_tools();
  test_bash_tool();

  tests::print_summary();
  return tests::failed == 0 ? 0 : 1;
}
