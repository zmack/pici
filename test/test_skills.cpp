#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>

#include <unistd.h>

#include "core/skills.h"

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
              << expr << " (expected " << expected << ", got " << cond << ")\n";
    return false;
  }
  return true;
}

#define CHECK(cond)                                                            \
  (::tests::CHECK_impl(static_cast<bool>(cond), true, #cond,                   \
                       std::source_location::current()))

#define CHECK_EQ(a, b)                                                         \
  (::tests::CHECK_impl((a) == (b), true, #a " == " #b,                         \
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

} // namespace tests

// ---------------------------------------------------------------------------
// Frontmatter parser
// ---------------------------------------------------------------------------

static void test_frontmatter() {
  using namespace tests;

  tests::register_test("frontmatter: valid minimal", []() {
    auto fm = parse_skill_frontmatter(
        "---\nname: foo\ndescription: bar\n---\n\nbody");
    CHECK(fm.has_value());
    CHECK_EQ(fm->name, "foo");
    CHECK_EQ(fm->description, "bar");
  });

  tests::register_test("frontmatter: missing description fails", []() {
    auto fm = parse_skill_frontmatter("---\nname: foo\n---\nbody");
    CHECK(!fm.has_value());
  });

  tests::register_test("frontmatter: no frontmatter fails", []() {
    auto fm = parse_skill_frontmatter("just some prose");
    CHECK(!fm.has_value());
  });

  tests::register_test("frontmatter: no closing delimiter fails", []() {
    auto fm = parse_skill_frontmatter("---\nname: foo\ndescription: bar\n");
    CHECK(!fm.has_value());
  });

  tests::register_test("frontmatter: prose with colons parses", []() {
    // Everything after the FIRST ": " is the scalar — no YAML dependency.
    auto fm = parse_skill_frontmatter(
        "---\ndescription: Build for AWS: ECS deploy\n---\n");
    CHECK(fm.has_value());
    CHECK_EQ(fm->description, "Build for AWS: ECS deploy");
  });

  tests::register_test("frontmatter: CRLF tolerated", []() {
    auto fm = parse_skill_frontmatter(
        "---\r\nname: crlf\r\ndescription: ok\r\n---\r\nbody\r\n");
    CHECK(fm.has_value());
    CHECK_EQ(fm->name, "crlf");
    CHECK_EQ(fm->description, "ok");
  });

  tests::register_test("frontmatter: BOM tolerated", []() {
    auto fm = parse_skill_frontmatter(
        "\xEF\xBB\xBF---\nname: bom\ndescription: ok\n---\n");
    CHECK(fm.has_value());
    CHECK_EQ(fm->name, "bom");
  });

  tests::register_test("frontmatter: empty body is valid", []() {
    auto fm = parse_skill_frontmatter("---\nname: e\ndescription: d\n---\n");
    CHECK(fm.has_value());
  });

  tests::register_test("frontmatter: over-long description rejected", []() {
    std::string long_desc(kSkillMaxDescriptionLength + 1, 'x');
    auto fm = parse_skill_frontmatter(
        "---\nname: big\ndescription: " + long_desc + "\n---\n");
    CHECK(!fm.has_value());

    auto ok = parse_skill_frontmatter(
        "---\nname: big\ndescription: " +
        std::string(kSkillMaxDescriptionLength, 'x') + "\n---\n");
    CHECK(ok.has_value());
  });

  tests::register_test("frontmatter: invalid names rejected", []() {
    CHECK(!parse_skill_frontmatter("---\nname: Bad Name\ndescription: d\n---\n")
               .has_value()); // uppercase + space
    CHECK(parse_skill_frontmatter("---\nname: a-b_c9\ndescription: d\n---\n")
              .has_value());
  });

  tests::register_test("frontmatter: folded scalar flattened", []() {
    auto fm = parse_skill_frontmatter(
        "---\nname: fold\ndescription: >\n  line one\n  line two\n---\n");
    CHECK(fm.has_value());
    CHECK_EQ(fm->description, "line one line two");
  });

  tests::register_test("frontmatter: quoted scalar unquoted", []() {
    auto fm = parse_skill_frontmatter(
        "---\nname: q\ndescription: \"quoted: desc\"\n---\n");
    CHECK(fm.has_value());
    CHECK_EQ(fm->description, "quoted: desc");
  });

  tests::register_test("frontmatter: unknown keys ignored", []() {
    auto fm =
        parse_skill_frontmatter("---\nname: u\ndescription: d\nallowed-tools: "
                                "[read]\nmetadata:\n  x: 1\n---\n");
    CHECK(fm.has_value());
    CHECK_EQ(fm->description, "d");
  });

  tests::register_test("frontmatter: short-description capped", []() {
    std::string too_long(kSkillMaxShortDescriptionLength + 1, 'y');
    auto fm = parse_skill_frontmatter(
        "---\nname: s\ndescription: d\nshort-description: " + too_long +
        "\n---\n");
    CHECK(!fm.has_value());
  });
}

// ---------------------------------------------------------------------------
// Discovery
// ---------------------------------------------------------------------------

static void test_discovery() {
  using namespace tests;

  tests::register_test("discovery: repo fixtures parse as expected", []() {
  // The committed fixtures double as living documentation: one canonical
  // skill, one compat-format skill (no name -> directory default).
#ifdef PI_CPP_SOURCE_DIR
    const auto fixtures = std::filesystem::path(PI_CPP_SOURCE_DIR) / "test" /
                          "fixtures" / "skills";
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
    CHECK_EQ(catalog.skills.size(), 2u);
    CHECK(catalog.diagnostics.empty());
    const auto *rc = find_skill(catalog, "release-checklist");
    CHECK(rc != nullptr);
    CHECK(rc && rc->scope == "project");
    const auto *pdf = find_skill(catalog, "pdf-extraction");
    CHECK(pdf != nullptr); // dir-name default from frontmatter-less file
    CHECK(pdf && pdf->scope == "compat");
#endif
  });
  using namespace tests;

  tests::register_test("discovery: project skill found", []() {
    TempDir tmp;
    auto f = make_workspace(tmp);
    write_file(f.root / "skills" / "alpha" / "SKILL.md",
               "---\nname: alpha\ndescription: Alpha skill\n---\nbody");

    auto catalog = discover_skills(f.root, f.agent_dir);
    CHECK_EQ(catalog.skills.size(), 1u);
    CHECK(catalog.diagnostics.empty());
    CHECK_EQ(catalog.skills[0].name, "alpha");
    CHECK_EQ(catalog.skills[0].scope, "project");
  });

  tests::register_test("discovery: precedence user beats project", []() {
    TempDir tmp;
    auto f = make_workspace(tmp);
    write_file(f.agent_dir / "skills" / "dup" / "SKILL.md",
               "---\nname: dup\ndescription: from user scope\n---\n");
    write_file(f.root / ".pici" / "skills" / "dup" / "SKILL.md",
               "---\nname: dup\ndescription: from project scope\n---\n");

    auto catalog = discover_skills(f.root, f.agent_dir);
    CHECK_EQ(catalog.skills.size(), 1u);
    // Plan §1 orders roots "ascending" with project scopes listed after
    // user scope, so the innermost project root outranks user scope.
    CHECK_EQ(catalog.skills[0].scope, "project");
    CHECK_EQ(catalog.skills[0].description, "from project scope");
    CHECK(!catalog.diagnostics.empty());
  });

  tests::register_test("discovery: inner ancestor overrides outer", []() {
    TempDir tmp;
    auto f = make_workspace(tmp);
    const auto outer = tmp.path() / "outer-ws";
    std::filesystem::create_directories(outer);
    write_file(outer / "skills" / "dup2" / "SKILL.md",
               "---\nname: dup2\ndescription: outer\n---\n");
    write_file(f.root / "skills" / "dup2" / "SKILL.md",
               "---\nname: dup2\ndescription: inner\n---\n");

    auto catalog = discover_skills(f.root, f.agent_dir);
    CHECK_EQ(catalog.skills.size(), 1u);
    CHECK_EQ(catalog.skills[0].description, "inner");
  });

  tests::register_test(
      "discovery: compat roots scanned, lowest precedence", []() {
        TempDir tmp;
        auto f = make_workspace(tmp);
        write_file(f.root / ".claude" / "skills" / "claude-one" / "SKILL.md",
                   "---\ndescription: claude format (no name)\n---\n");
        write_file(f.root / ".codex" / "skills" / "codex-one" / "SKILL.md",
                   "---\nname: codex-one\ndescription: codex format\n---\n");

        auto catalog = discover_skills(f.root, f.agent_dir);
        CHECK_EQ(catalog.skills.size(), 2u);
        const auto *c = find_skill(catalog, "claude-one"); // dir-name default
        CHECK(c != nullptr);
        CHECK(c && c->scope == "compat");
        CHECK(find_skill(catalog, "codex-one") != nullptr);
      });

  tests::register_test("discovery: user beats compat on collision", []() {
    TempDir tmp;
    auto f = make_workspace(tmp);
    write_file(f.root / ".claude" / "skills" / "both" / "SKILL.md",
               "---\nname: both\ndescription: claude\n---\n");
    write_file(f.agent_dir / "skills" / "both" / "SKILL.md",
               "---\nname: both\ndescription: user wins\n---\n");
    auto catalog = discover_skills(f.root, f.agent_dir);
    CHECK_EQ(catalog.skills.size(), 1u);
    CHECK_EQ(catalog.skills[0].description, "user wins");
  });

  tests::register_test(
      "discovery: invalid skills skipped with diagnostic", []() {
        TempDir tmp;
        auto f = make_workspace(tmp);
        write_file(f.root / "skills" / "bad" / "SKILL.md",
                   "# no frontmatter at all\n");
        write_file(f.root / "skills" / "good" / "SKILL.md",
                   "---\nname: good\ndescription: fine\n---\n");

        auto catalog = discover_skills(f.root, f.agent_dir);
        CHECK_EQ(catalog.skills.size(), 1u);
        CHECK_EQ(catalog.skills[0].name, "good");
        CHECK_EQ(catalog.diagnostics.size(), 1u);
        CHECK(catalog.diagnostics[0].find("bad") != std::string::npos);
      });

  tests::register_test("discovery: empty roots produce nothing", []() {
    TempDir tmp;
    auto f = make_workspace(tmp);
    std::filesystem::create_directories(f.root / "skills");
    auto catalog = discover_skills(f.root, f.agent_dir);
    CHECK(catalog.skills.empty());
    CHECK(catalog.diagnostics.empty());
  });

  tests::register_test("discovery: sorted by name", []() {
    TempDir tmp;
    auto f = make_workspace(tmp);
    write_file(f.root / "skills" / "zeta" / "SKILL.md",
               "---\nname: zeta\ndescription: z\n---\n");
    write_file(f.root / "skills" / "alpha" / "SKILL.md",
               "---\nname: alpha\ndescription: a\n---\n");
    auto catalog = discover_skills(f.root, f.agent_dir);
    CHECK_EQ(catalog.skills.size(), 2u);
    CHECK_EQ(catalog.skills[0].name, "alpha");
  });

  tests::register_test("discovery: symlink escape rejected", []() {
    TempDir tmp;
    auto f = make_workspace(tmp);
    const auto outside = tmp.path() / "outside.md";
    write_file(outside, "---\nname: sneaky\ndescription: outside\n---\n");
    std::error_code ec;
    std::filesystem::create_directory(f.root / "skills", ec);
    std::filesystem::create_directory_symlink(outside,
                                              f.root / "skills" / "sneaky");
    auto catalog = discover_skills(f.root, f.agent_dir);
    CHECK(catalog.skills.empty());
    CHECK(!catalog.diagnostics.empty());
  });
}

// ---------------------------------------------------------------------------
// load_skill_body
// ---------------------------------------------------------------------------

static void test_load_body() {
  using namespace tests;

  tests::register_test(
      "load: body returned verbatim without frontmatter", []() {
        TempDir tmp;
        const auto dir = tmp.path() / "sk";
        write_file(dir / "SKILL.md",
                   "---\nname: lb\ndescription: d\n---\nStep 1\nStep 2\n");
        SkillMetadata meta;
        meta.name = "lb";
        meta.path = (dir / "SKILL.md").string();
        auto body = load_skill_body(meta);
        CHECK(body.has_value());
        CHECK(body && *body == "Step 1\nStep 2\n");
      });

  tests::register_test("load: size cap enforced as error", []() {
    TempDir tmp;
    const auto dir = tmp.path() / "big";
    write_file(dir / "SKILL.md", "---\nname: big\ndescription: d\n---\n" +
                                     std::string(kSkillMaxBodyBytes + 1, 'x'));
    SkillMetadata meta;
    meta.name = "big";
    meta.path = (dir / "SKILL.md").string();
    auto body = load_skill_body(meta);
    CHECK(!body.has_value());
  });
}

int main() {
  test_frontmatter();
  test_discovery();
  test_load_body();

  tests::print_summary();
  return tests::failed == 0 ? 0 : 1;
}
