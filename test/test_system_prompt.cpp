#include "cli/system_prompt.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <iostream>
#include <source_location>
#include <string>
#include <vector>

TEST(SystemPrompt, Default_prompt_describes_the_harness) {
  const auto prompt = pi::cli::build_system_prompt(
      {}, {}, {}, {"read", "bash", "edit", "write", "ls"}, "/tmp/pici");

  EXPECT_TRUE(prompt.find("expert coding assistant operating inside pici") !=
              std::string::npos);
  EXPECT_TRUE(prompt.find("- read: Read file contents") != std::string::npos);
  EXPECT_TRUE(prompt.find("- ls: List directory contents") !=
              std::string::npos);
  EXPECT_TRUE(prompt.find("Do not modify files unless") != std::string::npos);
  EXPECT_TRUE(prompt.find("Current working directory: /tmp/pici") !=
              std::string::npos);
}

TEST(SystemPrompt, Custom_prompt_replaces_the_default) {
  const auto prompt = pi::cli::build_system_prompt(
      "custom system", {"extra instruction"},
      {{"/project/AGENTS.md", "follow these rules"}}, {"read"}, "/tmp/pici");

  EXPECT_TRUE(prompt.find("custom system") != std::string::npos);
  EXPECT_TRUE(prompt.find("extra instruction") != std::string::npos);
  EXPECT_TRUE(prompt.find("follow these rules") != std::string::npos);
  EXPECT_TRUE(prompt.find("expert coding assistant operating inside pici") ==
              std::string::npos);
  EXPECT_TRUE(prompt.find("Current date: ") != std::string::npos);
}

TEST(SystemPrompt, Skills_index_injected_when_catalog_provided) {
  pi::core::SkillCatalog catalog;
  catalog.skills.push_back({"release-checklist",
                            "Steps to cut a release: version bump, "
                            "changelog, tag, smoke test.",
                            "/ws/skills/release-checklist/SKILL.md",
                            "/ws/skills/release-checklist", "project"});
  catalog.skills.push_back({"pdf-extraction", "Extract tables/text from PDFs.",
                            "/ws/skills/pdf-extraction/SKILL.md",
                            "/ws/skills/pdf-extraction", "project"});

  const auto prompt = pi::cli::build_system_prompt({}, {}, {}, {"read", "bash"},
                                                   "/tmp/pici", &catalog);

  EXPECT_TRUE(prompt.find("# Skills") != std::string::npos);
  EXPECT_TRUE(prompt.find("- release-checklist: Steps to cut a release") !=
              std::string::npos);
  EXPECT_TRUE(prompt.find("- pdf-extraction: Extract tables/text") !=
              std::string::npos);
  EXPECT_TRUE(prompt.find("call the `skill` tool with its name") !=
              std::string::npos);
}

TEST(SystemPrompt, Empty_skills_catalog_injects_nothing) {
  pi::core::SkillCatalog empty;
  const auto prompt =
      pi::cli::build_system_prompt({}, {}, {}, {"read"}, "/tmp/pici", &empty);
  EXPECT_TRUE(prompt.find("# Skills") == std::string::npos);

  const auto null_prompt =
      pi::cli::build_system_prompt({}, {}, {}, {"read"}, "/tmp/pici", nullptr);
  EXPECT_TRUE(null_prompt.find("# Skills") == std::string::npos);
}

TEST(SystemPrompt, Skills_index_truncated_at_cap_with_overflow_notice) {
  pi::core::SkillCatalog big;
  for (int i = 0; i < 60; ++i) {
    const std::string name = "skill-" +
                             (i < 10 ? std::string("0") : std::string()) +
                             std::to_string(i);
    big.skills.push_back({name, "desc " + name, "/tmp/" + name + "/SKILL.md",
                          "/tmp/" + name, "project"});
  }
  const auto prompt =
      pi::cli::build_system_prompt({}, {}, {}, {"read"}, "/tmp/pici", &big);

  const auto first = prompt.find("- skill-00:");
  EXPECT_TRUE(first != std::string::npos);
  // skill-48..59 exist but must not be listed; the overflow line is.
  EXPECT_TRUE(prompt.find("- skill-48:") == std::string::npos);
  EXPECT_TRUE(prompt.find("...and 12 more") != std::string::npos);
  EXPECT_TRUE(prompt.find("accepts exact names only") != std::string::npos);
}
