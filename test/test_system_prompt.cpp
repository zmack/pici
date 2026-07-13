#include "cli/system_prompt.h"

#include <filesystem>
#include <iostream>
#include <source_location>
#include <string>
#include <vector>

namespace tests {

int passed{0};
int failed{0};

bool check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
  if (condition)
    return true;
  ++failed;
  std::cerr << "FAIL " << location.file_name() << ":" << location.line()
            << " - " << expression << "\n";
  return false;
}

void run(std::string_view name, auto test) {
  const auto before = failed;
  test();
  if (failed == before) {
    ++passed;
    std::cout << "PASS " << name << "\n";
  }
}

} // namespace tests

#define CHECK(condition) ::tests::check((condition), #condition)

int main() {
  tests::run("default prompt describes the harness", [] {
    const auto prompt = pi::cli::build_system_prompt(
        {}, {}, {}, {"read", "bash", "edit", "write", "ls"}, "/tmp/pici");

    CHECK(prompt.find("expert coding assistant operating inside pici") !=
          std::string::npos);
    CHECK(prompt.find("- read: Read file contents") != std::string::npos);
    CHECK(prompt.find("- ls: List directory contents") != std::string::npos);
    CHECK(prompt.find("Do not modify files unless") != std::string::npos);
    CHECK(prompt.find("Current working directory: /tmp/pici") !=
          std::string::npos);
  });

  tests::run("custom prompt replaces the default", [] {
    const auto prompt = pi::cli::build_system_prompt(
        "custom system", {"extra instruction"},
        {{"/project/AGENTS.md", "follow these rules"}}, {"read"}, "/tmp/pici");

    CHECK(prompt.find("custom system") != std::string::npos);
    CHECK(prompt.find("extra instruction") != std::string::npos);
    CHECK(prompt.find("follow these rules") != std::string::npos);
    CHECK(prompt.find("expert coding assistant operating inside pici") ==
          std::string::npos);
    CHECK(prompt.find("Current date: ") != std::string::npos);
  });

  std::cout << "Tests: " << tests::passed << " passed, " << tests::failed
            << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
