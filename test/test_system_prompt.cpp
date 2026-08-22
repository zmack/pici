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

  tests::run("skills index injected when catalog provided", [] {
    pi::core::SkillCatalog catalog;
    catalog.skills.push_back({"release-checklist",
                              "Steps to cut a release: version bump, "
                              "changelog, tag, smoke test.",
                              "/ws/skills/release-checklist/SKILL.md",
                              "/ws/skills/release-checklist", "project"});
    catalog.skills.push_back({"pdf-extraction",
                              "Extract tables/text from PDFs.",
                              "/ws/skills/pdf-extraction/SKILL.md",
                              "/ws/skills/pdf-extraction", "project"});

    const auto prompt = pi::cli::build_system_prompt(
        {}, {}, {}, {"read", "bash"}, "/tmp/pici", &catalog);

    CHECK(prompt.find("# Skills") != std::string::npos);
    CHECK(prompt.find("- release-checklist: Steps to cut a release") !=
          std::string::npos);
    CHECK(prompt.find("- pdf-extraction: Extract tables/text") !=
          std::string::npos);
    CHECK(prompt.find("call the `skill` tool with its name") !=
          std::string::npos);
  });

  tests::run("empty skills catalog injects nothing", [] {
    pi::core::SkillCatalog empty;
    const auto prompt = pi::cli::build_system_prompt({}, {}, {}, {"read"},
                                                    "/tmp/pici", &empty);
    CHECK(prompt.find("# Skills") == std::string::npos);

    const auto null_prompt = pi::cli::build_system_prompt({}, {}, {}, {"read"},
                                                         "/tmp/pici", nullptr);
    CHECK(null_prompt.find("# Skills") == std::string::npos);
  });

  tests::run("skills index truncated at cap with overflow notice", [] {
    pi::core::SkillCatalog big;
    for (int i = 0; i < 60; ++i) {
      const std::string name =
          "skill-" + (i < 10 ? std::string("0") : std::string()) +
          std::to_string(i);
      big.skills.push_back(
          {name, "desc " + name, "/tmp/" + name + "/SKILL.md",
           "/tmp/" + name, "project"});
    }
    const auto prompt =
        pi::cli::build_system_prompt({}, {}, {}, {"read"}, "/tmp/pici", &big);

    const auto first = prompt.find("- skill-00:");
    CHECK(first != std::string::npos);
    // skill-48..59 exist but must not be listed; the overflow line is.
    CHECK(prompt.find("- skill-48:") == std::string::npos);
    CHECK(prompt.find("...and 12 more") != std::string::npos);
    CHECK(prompt.find("accepts exact names only") != std::string::npos);
  });

  std::cout << "Tests: " << tests::passed << " passed, " << tests::failed
            << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
