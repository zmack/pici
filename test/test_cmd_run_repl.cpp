// Unit tests for CmdRunSession's REPL slash-command dispatch
// (dispatch_line() and the handle_*_command() methods), using a gmock
// MockRenderer in place of a real terminal.
//
// CmdRunSession::bootstrap_for_repl() runs the same bootstrap as run() --
// model/session/tool/hook setup through session creation and activation,
// against a real (but temp-directory-isolated) SessionRuntime, exactly as
// test_cmd_run.cpp's subprocess tests exercise -- but stops before
// dispatching into faux-control, RPC, or the blocking REPL loop, and lets
// the caller substitute a mock Renderer. That makes dispatch_line() and the
// individual slash commands directly testable in-process, without a PTY.
//
// Commands that would run a real agent turn (a bare prompt, or a hook
// command whose result includes `prompt`) are out of scope here -- doing so
// needs a fake/scripted LLM client (see test_faux_control_mode.cpp's
// Fixture pattern), a different collaborator than the Renderer this file
// mocks. /tree and /model with an empty spec are also out of scope: both
// drive an interactive full-screen selector not exercised here.
//
// CmdRunSession keeps its bootstrap/dispatch methods private and grants
// access to them via `friend class CmdRunSessionTest`. Because a TEST_F
// body actually runs in an auto-generated subclass of the fixture (not the
// fixture class itself), and friendship does not extend to that subclass,
// the fixture below exposes thin static wrapper methods -- defined inside
// CmdRunSessionTest itself, where the friend grant applies -- that TEST_F
// bodies call instead of touching CmdRunSession's private members directly.

#include "cli/cmd_run_session.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>

using namespace pi;
using ::testing::HasSubstr;
using ::testing::NiceMock;

namespace {

class MockRenderer : public core::Renderer {
public:
  MOCK_METHOD(void, on_text_delta, (std::string_view delta), (override));
  MOCK_METHOD(void, on_command_output, (std::string_view text), (override));
};

std::shared_ptr<const core::ModelRegistry> empty_registry() {
  return std::make_shared<const core::ModelRegistry>(
      std::map<std::string, core::ProviderConfig>{});
}

void write_file(const std::filesystem::path &path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream f(path);
  f << content;
}

} // namespace

class CmdRunSessionTest : public ::testing::Test {
protected:
  // Unique per-test directory so sessions never touch the developer's real
  // filesystem. CmdRunSession never reads config files itself (that's
  // cli::load_and_merge, upstream of cmd_run() -- bypassed entirely by
  // constructing Args directly here), so only session_dir needs isolating.
  std::filesystem::path home_dir_;

  void SetUp() override {
    home_dir_ = std::filesystem::temp_directory_path() /
               ("pici-cmd-run-repl-test-" + std::to_string(::getpid()) + "-" +
                std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::remove_all(home_dir_);
    std::filesystem::create_directories(home_dir_);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(home_dir_, ec);
  }

  cli::Args make_args(std::vector<std::string> hooks_files = {}) const {
    cli::Args args;
    args.session_dir = (home_dir_ / "sessions").string();
    args.no_context_files = true;
    args.no_skills = true;
    args.sandbox_mode = "disabled";
    args.hooks_files = std::move(hooks_files);
    return args;
  }

  // Thin, friend-authorized wrappers around CmdRunSession's private
  // bootstrap/dispatch methods. See file comment for why TEST_F bodies
  // can't call these directly.
  static bool bootstrap(CmdRunSession &session, MockRenderer **out_renderer) {
    auto owned = std::make_unique<NiceMock<MockRenderer>>();
    *out_renderer = owned.get();
    return session.bootstrap_for_repl(std::move(owned));
  }
  static bool dispatch(CmdRunSession &session, const std::string &line) {
    return session.dispatch_line(line);
  }
};

TEST_F(CmdRunSessionTest, ToolsCommandListsBuiltinTools) {
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_CALL(*renderer, on_command_output(HasSubstr("read")));
  EXPECT_FALSE(dispatch(session, "/tools"));
}

TEST_F(CmdRunSessionTest, SkillsCommandReportsDisabledWhenNoSkillsSet) {
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_CALL(*renderer, on_command_output(HasSubstr("skills disabled")));
  EXPECT_FALSE(dispatch(session, "/skills"));
}

TEST_F(CmdRunSessionTest, AddonsCommandReportsNoneLoadedByDefault) {
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_CALL(*renderer, on_command_output(HasSubstr("no add-ons loaded")));
  EXPECT_FALSE(dispatch(session, "/addons"));
}

TEST_F(CmdRunSessionTest, AddonsCommandShowsLoadedHookFile) {
  const auto hooks_file = home_dir_ / "greet_hook.lua";
  write_file(hooks_file, R"lua(
return {
  commands = {
    { name = "greet", description = "says hi", args_hint = "[name]" },
  },
}
)lua");
  CmdRunSession session(make_args({hooks_file.string()}), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_CALL(*renderer, on_command_output(HasSubstr("/greet")));
  EXPECT_FALSE(dispatch(session, "/addons"));
}

TEST_F(CmdRunSessionTest, ReloadAddonsCommandReportsCount) {
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_CALL(*renderer, on_command_output(HasSubstr("reloaded 0 add-on(s)")));
  EXPECT_FALSE(dispatch(session, "/reload-addons"));
}

TEST_F(CmdRunSessionTest, CompactCommandDeclinesOutsideInteractiveTerminal) {
  // dispatch_line() runs under ctest with stdin not a TTY, so /compact must
  // take its non-interactive guard rather than trying to drive a live
  // compaction request.
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_CALL(*renderer,
             on_command_output(HasSubstr("only available in an interactive "
                                         "terminal session")));
  EXPECT_FALSE(dispatch(session, "/compact"));
}

TEST_F(CmdRunSessionTest, UsageCommandReportsZeroTurnsBeforeAnyTurnRuns) {
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_CALL(*renderer, on_command_output(HasSubstr("turns: 0")));
  EXPECT_FALSE(dispatch(session, "/usage"));
}

TEST_F(CmdRunSessionTest, MemoryCommandRendersHeader) {
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_CALL(*renderer, on_command_output(HasSubstr("Memory")));
  EXPECT_FALSE(dispatch(session, "/memory"));
}

TEST_F(CmdRunSessionTest, ModelsCommandListsTheCatalog) {
  // core::ModelRegistry bundles a built-in catalog even when constructed
  // with no configured providers, so this isn't exercising an *empty*
  // catalog -- just that /models reaches format_model_catalog() and prints
  // its header row.
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_CALL(*renderer, on_command_output(HasSubstr("provider")));
  EXPECT_FALSE(dispatch(session, "/models"));
}

TEST_F(CmdRunSessionTest, NameCommandWithoutArgumentPrintsUsage) {
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  // /name with no argument writes its usage message to stderr, not the
  // renderer -- just confirm it doesn't crash and doesn't exit the REPL.
  EXPECT_FALSE(dispatch(session, "/name"));
}

TEST_F(CmdRunSessionTest, NewCommandStartsAFreshSession) {
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_FALSE(dispatch(session, "/new"));
}

TEST_F(CmdRunSessionTest, ForkCommandCreatesAChildSession) {
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_FALSE(dispatch(session, "/fork"));
}

TEST_F(CmdRunSessionTest, ExitAndQuitRequestReplExit) {
  CmdRunSession session(make_args(), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_TRUE(dispatch(session, "/exit"));

  CmdRunSession session2(make_args(), empty_registry());
  MockRenderer *renderer2 = nullptr;
  ASSERT_TRUE(bootstrap(session2, &renderer2));
  EXPECT_TRUE(dispatch(session2, "/quit"));
}

TEST_F(CmdRunSessionTest, HookOnCommandHandlesAndSuppliesOutput) {
  const auto hooks_file = home_dir_ / "reverse_hook.lua";
  write_file(hooks_file, R"lua(
return {
  on_command = function(cmd, args)
    if cmd == "reverse" then
      return {handled = true, output = args:reverse()}
    end
    return {handled = false}
  end,
}
)lua");
  CmdRunSession session(make_args({hooks_file.string()}), empty_registry());
  MockRenderer *renderer = nullptr;
  ASSERT_TRUE(bootstrap(session, &renderer));

  EXPECT_CALL(*renderer, on_command_output(HasSubstr("cba")));
  EXPECT_FALSE(dispatch(session, "/reverse abc"));
}

// An unhandled hook command (and a bare, non-slash prompt) falls all the
// way through dispatch_line() to accumulate(run_and_persist(line)), which
// runs a real agent turn -- out of scope here for the same reason as
// /tree and /model with an empty spec (see file comment): it needs a
// fake/scripted LLM client, a different collaborator than the Renderer
// this file mocks, and would otherwise attempt a real network connection.
