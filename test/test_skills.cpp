#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

#include <unistd.h>

#include "core/skills.h"

#include <gtest/gtest.h>
using namespace pi::core;

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

class TempDir {
public:
  TempDir() {
    static int counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("pici-skills-test-" + std::to_string(getpid()) + "-" +
             std::to_string(counter++));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TempDir() { std::filesystem::remove_all(path_); }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

std::string read_file(const std::filesystem::path &p) {
  std::ifstream in(p);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path &p, const std::string &content) {
  std::error_code ec;
  std::filesystem::create_directories(p.parent_path(), ec);
  std::ofstream out(p);
  out << content;
}

struct Fixture {
  std::filesystem::path root;
  std::filesystem::path agent_dir;
};

// Creates a workspace root with a nested subdir; returns both cwd options.
Fixture make_workspace(TempDir &tmp) {
  Fixture f;
  f.root = tmp.path() / "ws";
  f.agent_dir = tmp.path() / "agent";
  std::filesystem::create_directories(f.root);
  return f;
}

const SkillMetadata *find_skill(const SkillCatalog &catalog,
                                const std::string &name) {
  for (const auto &s : catalog.skills)
    if (s.name == name)
      return &s;
  return nullptr;
}

// ---------------------------------------------------------------------------
// Frontmatter parser
// ---------------------------------------------------------------------------

TEST(Frontmatter, ValidMinimal) {
  auto fm =
      parse_skill_frontmatter("---\nname: foo\ndescription: bar\n---\n\nbody");
  ASSERT_TRUE(fm.has_value());
  EXPECT_EQ(fm->name, "foo");
  EXPECT_EQ(fm->description, "bar");
}

TEST(Frontmatter, MissingDescriptionFails) {
  auto fm = parse_skill_frontmatter("---\nname: foo\n---\nbody");
  EXPECT_TRUE(!fm.has_value());
}

TEST(Frontmatter, NoFrontmatterFails) {
  auto fm = parse_skill_frontmatter("just some prose");
  EXPECT_TRUE(!fm.has_value());
}

TEST(Frontmatter, NoClosingDelimiterFails) {
  auto fm = parse_skill_frontmatter("---\nname: foo\ndescription: bar\n");
  EXPECT_TRUE(!fm.has_value());
}

TEST(Frontmatter, ProseWithColonsParses) {
  // Everything after the FIRST ": " is the scalar — no YAML dependency.
  auto fm = parse_skill_frontmatter(
      "---\ndescription: Build for AWS: ECS deploy\n---\n");
  ASSERT_TRUE(fm.has_value());
  EXPECT_EQ(fm->description, "Build for AWS: ECS deploy");
}

TEST(Frontmatter, CRLFTolerated) {
  auto fm = parse_skill_frontmatter(
      "---\r\nname: crlf\r\ndescription: ok\r\n---\r\nbody\r\n");
  ASSERT_TRUE(fm.has_value());
  EXPECT_EQ(fm->name, "crlf");
  EXPECT_EQ(fm->description, "ok");
}

TEST(Frontmatter, BOMTolerated) {
  auto fm = parse_skill_frontmatter(
      "\xEF\xBB\xBF---\nname: bom\ndescription: ok\n---\n");
  ASSERT_TRUE(fm.has_value());
  EXPECT_EQ(fm->name, "bom");
}

TEST(Frontmatter, EmptyBodyIsValid) {
  auto fm = parse_skill_frontmatter("---\nname: e\ndescription: d\n---\n");
  ASSERT_TRUE(fm.has_value());
}

TEST(Frontmatter, OverLongDescriptionRejected) {
  std::string long_desc(kSkillMaxDescriptionLength + 1, 'x');
  auto fm = parse_skill_frontmatter(
      "---\nname: big\ndescription: " + long_desc + "\n---\n");
  EXPECT_TRUE(!fm.has_value());

  auto ok = parse_skill_frontmatter(
      "---\nname: big\ndescription: " +
      std::string(kSkillMaxDescriptionLength, 'x') + "\n---\n");
  ASSERT_TRUE(ok.has_value());
}

TEST(Frontmatter, InvalidNamesRejected) {
  EXPECT_TRUE(
      !parse_skill_frontmatter("---\nname: Bad Name\ndescription: d\n---\n")
           .has_value()); // uppercase + space
  EXPECT_TRUE(
      parse_skill_frontmatter("---\nname: a-b_c9\ndescription: d\n---\n")
          .has_value());
}

TEST(Frontmatter, FoldedScalarFlattened) {
  auto fm = parse_skill_frontmatter(
      "---\nname: fold\ndescription: >\n  line one\n  line two\n---\n");
  ASSERT_TRUE(fm.has_value());
  EXPECT_EQ(fm->description, "line one line two");
}

TEST(Frontmatter, QuotedScalarUnquoted) {
  auto fm = parse_skill_frontmatter(
      "---\nname: q\ndescription: \"quoted: desc\"\n---\n");
  ASSERT_TRUE(fm.has_value());
  EXPECT_EQ(fm->description, "quoted: desc");
}

TEST(Frontmatter, UnknownKeysIgnored) {
  auto fm =
      parse_skill_frontmatter("---\nname: u\ndescription: d\nallowed-tools: "
                              "[read]\nmetadata:\n  x: 1\n---\n");
  ASSERT_TRUE(fm.has_value());
  EXPECT_EQ(fm->description, "d");
}

TEST(Frontmatter, ShortDescriptionCapped) {
  std::string too_long(kSkillMaxShortDescriptionLength + 1, 'y');
  auto fm = parse_skill_frontmatter(
      "---\nname: s\ndescription: d\nshort-description: " + too_long +
      "\n---\n");
  EXPECT_TRUE(!fm.has_value());
}
TEST(Discovery, RepoFixturesParseAsExpected) {
  // The committed fixtures double as living documentation: one canonical
  // skill, one compat-format skill (no name -> directory default).
#ifdef PI_CPP_SOURCE_DIR
  const auto fixtures =
      std::filesystem::path(PI_CPP_SOURCE_DIR) / "test" / "fixtures" / "skills";
  TempDir tmp;
  const auto ws = tmp.path() / "ws";
  std::filesystem::create_directories(ws / "skills");
  std::filesystem::create_directories(ws / ".claude" / "skills");
  std::filesystem::copy(fixtures / "release-checklist",
                        ws / "skills" / "release-checklist",
                        std::filesystem::copy_options::recursive);
  std::filesystem::copy(fixtures / "pdf-extraction",
                        ws / ".claude" / "skills" / "pdf-extraction",
                        std::filesystem::copy_options::recursive);

  auto catalog = discover_skills(ws, tmp.path() / "agent");
  EXPECT_EQ(catalog.skills.size(), 2u);
  EXPECT_TRUE(catalog.diagnostics.empty());
  const auto *rc = find_skill(catalog, "release-checklist");
  EXPECT_TRUE(rc != nullptr);
  EXPECT_TRUE(rc && rc->scope == "project");
  const auto *pdf = find_skill(catalog, "pdf-extraction");
  EXPECT_TRUE(pdf != nullptr); // dir-name default from frontmatter-less file
  EXPECT_TRUE(pdf && pdf->scope == "compat");
#endif
}

TEST(Discovery, ProjectSkillFound) {
  TempDir tmp;
  auto f = make_workspace(tmp);
  write_file(f.root / "skills" / "alpha" / "SKILL.md",
             "---\nname: alpha\ndescription: Alpha skill\n---\nbody");

  auto catalog = discover_skills(f.root, f.agent_dir);
  EXPECT_EQ(catalog.skills.size(), 1u);
  EXPECT_TRUE(catalog.diagnostics.empty());
  EXPECT_EQ(catalog.skills[0].name, "alpha");
  EXPECT_EQ(catalog.skills[0].scope, "project");
}

TEST(Discovery, PrecedenceUserBeatsProject) {
  TempDir tmp;
  auto f = make_workspace(tmp);
  write_file(f.agent_dir / "skills" / "dup" / "SKILL.md",
             "---\nname: dup\ndescription: from user scope\n---\n");
  write_file(f.root / ".pici" / "skills" / "dup" / "SKILL.md",
             "---\nname: dup\ndescription: from project scope\n---\n");

  auto catalog = discover_skills(f.root, f.agent_dir);
  EXPECT_EQ(catalog.skills.size(), 1u);
  // Plan §1 orders roots "ascending" with project scopes listed after
  // user scope, so the innermost project root outranks user scope.
  EXPECT_EQ(catalog.skills[0].scope, "project");
  EXPECT_EQ(catalog.skills[0].description, "from project scope");
  EXPECT_TRUE(!catalog.diagnostics.empty());
}

TEST(Discovery, InnerAncestorOverridesOuter) {
  TempDir tmp;
  auto f = make_workspace(tmp);
  const auto outer = tmp.path() / "outer-ws";
  std::filesystem::create_directories(outer);
  write_file(outer / "skills" / "dup2" / "SKILL.md",
             "---\nname: dup2\ndescription: outer\n---\n");
  write_file(f.root / "skills" / "dup2" / "SKILL.md",
             "---\nname: dup2\ndescription: inner\n---\n");

  auto catalog = discover_skills(f.root, f.agent_dir);
  EXPECT_EQ(catalog.skills.size(), 1u);
  EXPECT_EQ(catalog.skills[0].description, "inner");
}

TEST(Discovery, CompatRootsScannedLowestPrecedence) {
  TempDir tmp;
  auto f = make_workspace(tmp);
  write_file(f.root / ".claude" / "skills" / "claude-one" / "SKILL.md",
             "---\ndescription: claude format (no name)\n---\n");
  write_file(f.root / ".codex" / "skills" / "codex-one" / "SKILL.md",
             "---\nname: codex-one\ndescription: codex format\n---\n");

  auto catalog = discover_skills(f.root, f.agent_dir);
  EXPECT_EQ(catalog.skills.size(), 2u);
  const auto *c = find_skill(catalog, "claude-one"); // dir-name default
  EXPECT_TRUE(c != nullptr);
  EXPECT_TRUE(c && c->scope == "compat");
  EXPECT_TRUE(find_skill(catalog, "codex-one") != nullptr);
}

TEST(Discovery, UserBeatsCompatOnCollision) {
  TempDir tmp;
  auto f = make_workspace(tmp);
  write_file(f.root / ".claude" / "skills" / "both" / "SKILL.md",
             "---\nname: both\ndescription: claude\n---\n");
  write_file(f.agent_dir / "skills" / "both" / "SKILL.md",
             "---\nname: both\ndescription: user wins\n---\n");
  auto catalog = discover_skills(f.root, f.agent_dir);
  EXPECT_EQ(catalog.skills.size(), 1u);
  EXPECT_EQ(catalog.skills[0].description, "user wins");
}

TEST(Discovery, InvalidSkillsSkippedWithDiagnostic) {
  TempDir tmp;
  auto f = make_workspace(tmp);
  write_file(f.root / "skills" / "bad" / "SKILL.md",
             "# no frontmatter at all\n");
  write_file(f.root / "skills" / "good" / "SKILL.md",
             "---\nname: good\ndescription: fine\n---\n");

  auto catalog = discover_skills(f.root, f.agent_dir);
  EXPECT_EQ(catalog.skills.size(), 1u);
  EXPECT_EQ(catalog.skills[0].name, "good");
  EXPECT_EQ(catalog.diagnostics.size(), 1u);
  EXPECT_TRUE(catalog.diagnostics[0].find("bad") != std::string::npos);
}

TEST(Discovery, EmptyRootsProduceNothing) {
  TempDir tmp;
  auto f = make_workspace(tmp);
  std::filesystem::create_directories(f.root / "skills");
  auto catalog = discover_skills(f.root, f.agent_dir);
  EXPECT_TRUE(catalog.skills.empty());
  EXPECT_TRUE(catalog.diagnostics.empty());
}

TEST(Discovery, SortedByName) {
  TempDir tmp;
  auto f = make_workspace(tmp);
  write_file(f.root / "skills" / "zeta" / "SKILL.md",
             "---\nname: zeta\ndescription: z\n---\n");
  write_file(f.root / "skills" / "alpha" / "SKILL.md",
             "---\nname: alpha\ndescription: a\n---\n");
  auto catalog = discover_skills(f.root, f.agent_dir);
  EXPECT_EQ(catalog.skills.size(), 2u);
  EXPECT_EQ(catalog.skills[0].name, "alpha");
}

TEST(Discovery, SymlinkEscapeRejected) {
  TempDir tmp;
  auto f = make_workspace(tmp);
  const auto outside = tmp.path() / "outside.md";
  write_file(outside, "---\nname: sneaky\ndescription: outside\n---\n");
  std::error_code ec;
  std::filesystem::create_directory(f.root / "skills", ec);
  std::filesystem::create_directory_symlink(outside,
                                            f.root / "skills" / "sneaky");
  auto catalog = discover_skills(f.root, f.agent_dir);
  EXPECT_TRUE(catalog.skills.empty());
  EXPECT_TRUE(!catalog.diagnostics.empty());
}
TEST(Load, BodyReturnedVerbatimWithoutFrontmatter) {
  TempDir tmp;
  const auto dir = tmp.path() / "sk";
  write_file(dir / "SKILL.md",
             "---\nname: lb\ndescription: d\n---\nStep 1\nStep 2\n");
  SkillMetadata meta;
  meta.name = "lb";
  meta.path = (dir / "SKILL.md").string();
  auto body = load_skill_body(meta);
  ASSERT_TRUE(body.has_value());
  EXPECT_TRUE(body && *body == "Step 1\nStep 2\n");
}

TEST(Load, SizeCapEnforcedAsError) {
  TempDir tmp;
  const auto dir = tmp.path() / "big";
  write_file(dir / "SKILL.md", "---\nname: big\ndescription: d\n---\n" +
                                   std::string(kSkillMaxBodyBytes + 1, 'x'));
  SkillMetadata meta;
  meta.name = "big";
  meta.path = (dir / "SKILL.md").string();
  auto body = load_skill_body(meta);
  EXPECT_TRUE(!body.has_value());
}
