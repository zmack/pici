#include "core/apply_patch.h"
#include "support/gtest_helpers.h"

#include <gmock/gmock.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>

TEST(ApplyPatch, AppliesUpdate) {
  pi::test::TemporaryDirectory directory("pici-apply-patch-test");
  const auto &dir = directory.path();
  {
    std::ofstream(dir / "file.txt") << "one\ntwo\n";
    pi::core::ParseDiagnostic error;
    const std::string input =
        "*** Begin Patch" + std::string(1, '\n') +
        "*** Update File: file.txt\n@@\n-one\n+ONE\n*** End Patch\n";
    const auto patch = pi::core::parse_patch(input, true, error);
    ASSERT_TRUE(patch);
    pi::core::apply_patch(dir, *patch);
  }
  std::ifstream in(dir / "file.txt");
  const std::string content{std::istreambuf_iterator<char>(in), {}};
  EXPECT_THAT(content, testing::StrEq("ONE\ntwo\n"));
}

TEST(ApplyPatch, PreservesLeadingContext) {
  // A chunk with a leading pure-context line (" foo", no +/-) must parse
  // under lenient mode: lenient only relaxes directive-marker matching, the
  // leading space on a content line is the diff-line sentinel and must
  // survive.
  pi::test::TemporaryDirectory directory("pici-apply-patch-test");
  const auto &dir = directory.path();
  {
    std::ofstream(dir / "file.txt") << "one\ntwo\nthree\n";
    pi::core::ParseDiagnostic error;
    const std::string input = "*** Begin Patch\n"
                              "*** Update File: file.txt\n"
                              "@@\n"
                              " one\n"
                              "-two\n"
                              "+TWO\n"
                              " three\n"
                              "*** End Patch\n";
    const auto patch = pi::core::parse_patch(input, true, error);
    ASSERT_TRUE(patch);
    pi::core::apply_patch(dir, *patch);
  }
  std::ifstream in2(dir / "file.txt");
  const std::string content2{std::istreambuf_iterator<char>(in2), {}};
  EXPECT_THAT(content2, testing::StrEq("one\nTWO\nthree\n"));
}
