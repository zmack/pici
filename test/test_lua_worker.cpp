#include "core/agent_loop.h"
#include "core/agent_state.h"
#include "core/lua_eval_tool.h"
#include "core/lua_worker_tool.h"
#include "support/gtest_helpers.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <thread>

namespace {
using Json = nlohmann::json;
using namespace pi::core;

#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif
#elif defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
constexpr bool kSanitized = true;
#else
constexpr bool kSanitized = false;
#endif

class LuaWorker : public testing::Test {
protected:
  pi::test::TemporaryDirectory directory{"pici-lua-worker"};
  std::shared_ptr<const LuaWorkerAdapter> adapter =
      std::make_shared<LuaWorkerAdapter>(PI_LUA_WORKER_BINARY);

  void SetUp() override {
    if (access("/usr/bin/bwrap", X_OK) != 0 && access("/bin/bwrap", X_OK) != 0)
      GTEST_SKIP() << "bubblewrap is required for worker integration tests";
    std::filesystem::create_directory(directory.path() / "scratch");
    std::ofstream(directory.path() / "input.txt") << "hello";
  }

  std::shared_ptr<const ToolDefinition>
  tool(bool write = false,
       std::vector<std::string> libraries = {"io", "os", "lfs"}) {
    LuaWorkerPolicy policy{.workspace = directory.path(),
                           .libraries = std::move(libraries)};
    // Sanitizers reserve a large shadow address space. Test the explicit
    // unlimited-address-space profile there, with all other OS limits intact.
    policy.address_space_mb = kSanitized ? 0 : 256;
    if (write)
      policy.write_roots = {"scratch"};
    return make_lua_worker_tool(std::move(policy), adapter);
  }

  Json execute(std::string script, Json libraries = Json::array(),
               bool write = false, Json extra = Json::object()) {
    if (libraries.is_null())
      libraries = Json::array();
    extra["script"] = std::move(script);
    extra["requires"] = std::move(libraries);
    const auto result = tool(write)->execute("test", extra.dump(), {}, {});
    return Json::parse(result->content());
  }
};

TEST_F(LuaWorker, StandardUtilitiesAndJsonAreAvailableWithoutHostAccess) {
  const auto result = execute(R"(
    assert(io == nil and os == nil and lfs == nil and package == nil)
    assert(debug == nil and dofile == nil and loadfile == nil and pici == nil)
    assert(require('json') == json and require('string') == string)
    assert(json.encode(json.decode('[]')) == '[]')
    assert(json.encode(json.array()) == '[]')
    local value = json.decode('{"answer":42,"nil_value":null}')
    assert(value.answer == 42 and value.nil_value == json.null)
    assert(json.encode(value):find('null'))
    local co = coroutine.create(function() return 'done' end)
    return string.match('a12', '%d+'), select(2, coroutine.resume(co)), value
  )");
  ASSERT_TRUE(result["ok"].get<bool>()) << result;
  EXPECT_EQ(result["values"][0], "12");
  EXPECT_EQ(result["values"][1], "done");
  EXPECT_EQ(result["values"][2]["answer"], 42);
}

TEST_F(LuaWorker, SelectedIoReadsButCannotWriteReadOnlyWorkspace) {
  const auto result = execute(R"(
    local io = require('io')
    local f = assert(io.open('input.txt', 'r'))
    local text = f:read('*a'); f:close()
    assert(io.open('forbidden.txt', 'w') == nil)
    assert(io.open('/etc/passwd', 'r') == nil)
    for descriptor = 3, 64 do
      assert(io.open('/proc/self/fd/' .. descriptor, 'r') == nil)
    end
    return text
  )",
                              {"io"});
  ASSERT_TRUE(result["ok"].get<bool>()) << result;
  EXPECT_EQ(result["values"][0], "hello");
  EXPECT_FALSE(std::filesystem::exists(directory.path() / "forbidden.txt"));
}

TEST_F(LuaWorker, ExplicitWriteRootAllowsEditsAndDeniesOtherDirectories) {
  const auto result = execute(R"(
    local io = require('io')
    local f = assert(io.open('scratch/output.txt', 'w')); f:write('changed'); f:close()
    assert(io.open('input.txt', 'w') == nil)
    return 'written'
  )",
                              {"io"}, true);
  ASSERT_TRUE(result["ok"].get<bool>()) << result;
  std::ifstream stream(directory.path() / "scratch/output.txt");
  std::string content;
  stream >> content;
  EXPECT_EQ(content, "changed");
}

TEST_F(LuaWorker, OsCommandsShareReadOnlyMountsAndSanitizedEnvironment) {
  pi::test::ScopedEnvironmentVariable secret("PICI_LUA_TEST_SECRET",
                                             "not-for-worker");
  const auto result = execute(R"(
    local os = require('os')
    assert(os.getenv('PICI_LUA_TEST_SECRET') == nil)
    assert(os.getenv('HOME') == '/tmp/home')
    local ok = os.execute('printf forbidden > /workspace/forbidden.txt')
    assert(not ok)
    assert(os.execute('test ! -e /etc/passwd'))
    return true
  )",
                              {"os"});
  ASSERT_TRUE(result["ok"].get<bool>()) << result;
  EXPECT_FALSE(std::filesystem::exists(directory.path() / "forbidden.txt"));
}

TEST_F(LuaWorker, DirectoryChangesAndGlobalsStayInOneWorker) {
  const auto host_cwd = std::filesystem::current_path();
  const auto first =
      execute("local lfs=require('lfs'); assert(lfs.chdir('scratch')); "
              "leaked=1; return lfs.currentdir()",
              {"lfs"});
  ASSERT_TRUE(first["ok"].get<bool>()) << first;
  EXPECT_EQ(first["values"][0], "/workspace/scratch");
  const auto second = execute("local lfs=require('lfs'); assert(leaked == "
                              "nil); return lfs.currentdir()",
                              {"lfs"});
  ASSERT_TRUE(second["ok"].get<bool>()) << second;
  EXPECT_EQ(second["values"][0], "/workspace");
  EXPECT_EQ(std::filesystem::current_path(), host_cwd);
}

TEST_F(LuaWorker, DeniedOrMalformedManifestExecutesNoEffects) {
  const auto selected = tool(true, {"io"});
  for (const auto &request : std::vector<Json>{
           {{"requires", {"os"}}, {"script", "error('executed')"}},
           {{"requires", {"io", "io"}}, {"script", "error('executed')"}},
           {{"requires", {17}}, {"script", "error('executed')"}},
           {{"requires", Json::array()},
            {"script", "return 1"},
            {"unexpected", true}},
           {{"script", "return 1"}},
           {{"requires", Json::array()},
            {"script", "return 1"},
            {"timeout_ms", 0}}}) {
    const auto result = selected->execute("test", request.dump(), {}, {});
    EXPECT_TRUE(result->is_error()) << result->content();
    EXPECT_EQ(result->content().find("executed"), std::string::npos);
  }
}

TEST_F(LuaWorker, UndeclaredImportFailsEvenWhenSessionAllowsIt) {
  const auto result = execute("return require('io')");
  EXPECT_FALSE(result["ok"].get<bool>()) << result;
  EXPECT_TRUE(result["error"].get<std::string>().contains("not declared"));
}

TEST_F(LuaWorker, CheckCompilesWithoutExecutingOrOpeningLibraries) {
  const auto result =
      execute("assert(io.open('scratch/bad', 'w')); error('executed')", {"io"},
              true, {{"mode", "check"}});
  ASSERT_TRUE(result["ok"].get<bool>()) << result;
  EXPECT_TRUE(result["checked"].get<bool>());
  EXPECT_FALSE(std::filesystem::exists(directory.path() / "scratch/bad"));
  EXPECT_FALSE(
      execute("local =", {}, false, {{"mode", "check"}})["ok"].get<bool>());
}

TEST_F(LuaWorker, TimeoutInterruptsLuaAndBlockingCommands) {
  const auto begin = std::chrono::steady_clock::now();
  const auto loop =
      execute("while true do end", {}, false, {{"timeout_ms", 100}});
  EXPECT_FALSE(loop["ok"].get<bool>()) << loop;
  EXPECT_TRUE(loop["error"].get<std::string>().contains("timed out"));
  const auto command = execute("require('os').execute('sleep 30')", {"os"},
                               false, {{"timeout_ms", 100}});
  EXPECT_FALSE(command["ok"].get<bool>()) << command;
  EXPECT_LT(std::chrono::steady_clock::now() - begin, std::chrono::seconds(5));
}

TEST_F(LuaWorker, OutputFloodAndAbruptExitDoNotKillHost) {
  const auto flood =
      execute("require('io').write(string.rep('x', 600000))", {"io"});
  EXPECT_FALSE(flood["ok"].get<bool>()) << flood;
  EXPECT_TRUE(flood["error"].get<std::string>().contains("512 KiB"));
  const auto exited = execute("require('os').exit(0)", {"os"});
  EXPECT_FALSE(exited["ok"].get<bool>()) << exited;
  EXPECT_TRUE(execute("return 42")["ok"].get<bool>());
}

TEST_F(LuaWorker, UnsupportedCyclicAndBinaryValuesAreHandled) {
  EXPECT_FALSE(execute("local t={}; t.self=t; return t")["ok"].get<bool>());
  EXPECT_FALSE(execute("return function() end")["ok"].get<bool>());
  EXPECT_FALSE(execute("return string.char(255)")["ok"].get<bool>());
  const auto output = execute("print(string.char(255)); return 'ok'");
  EXPECT_TRUE(output["ok"].get<bool>()) << output;
}

TEST_F(LuaWorker, CancellationAndCwdEscapeFailWithoutLaunchEffects) {
  auto selected = tool();
  std::stop_source stop;
  stop.request_stop();
  const auto cancelled = selected->execute(
      "test", R"json({"requires":[],"script":"return 1"})json",
      stop.get_token(), {});
  EXPECT_TRUE(cancelled->is_error());
  EXPECT_FALSE(
      execute("return 1", {}, false, {{"cwd", "../"}})["ok"].get<bool>());
}

TEST_F(LuaWorker, EscapingWorkspaceSymlinkDoesNotExposeHostFile) {
  pi::test::TemporaryDirectory outside{"pici-lua-outside"};
  std::ofstream(outside.path() / "secret") << "secret";
  std::filesystem::create_directory_symlink(outside.path(),
                                            directory.path() / "outside");
  const auto result = execute(
      "assert(require('io').open('outside/secret') == nil); return true",
      {"io"});
  EXPECT_TRUE(result["ok"].get<bool>()) << result;
  LuaWorkerPolicy policy{.workspace = directory.path(),
                         .read_roots = {"outside"}};
  EXPECT_THROW(make_lua_worker_tool(policy, adapter), std::invalid_argument);
}

TEST_F(LuaWorker, MountSourceReplacementCannotWidenPolicy) {
  auto selected = tool(true);
  std::filesystem::rename(directory.path() / "scratch",
                          directory.path() / "original");
  std::filesystem::create_directory_symlink("/tmp",
                                            directory.path() / "scratch");
  const auto result = selected->execute(
      "test",
      R"json({"requires":["io"],"script":"error('should not run')"})json", {},
      {});
  EXPECT_TRUE(result->is_error());
  EXPECT_FALSE(result->content().contains("should not run"));
}

TEST_F(LuaWorker, BackgroundProcessesCannotWriteAfterInvocationReturns) {
  const auto result =
      execute("require('os').execute('(sleep 0.3; echo escaped > scratch/late) "
              ">/dev/null 2>&1 &'); return true",
              {"os"}, true);
  ASSERT_TRUE(result["ok"].get<bool>()) << result;
  // Deliberately outwait the scheduled background effect after namespace exit.
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  EXPECT_FALSE(std::filesystem::exists(directory.path() / "scratch/late"));
}

TEST_F(LuaWorker, SchemaIsStableAcrossPoliciesAndAdmissionSurvivesReplacement) {
  const auto selected = tool();
  const auto restricted = tool(false, {"io"});
  EXPECT_EQ(selected->schema().serialize(), restricted->schema().serialize());
  EXPECT_EQ(selected->description(), restricted->description());
  AgentState state;
  state.restrict_to_tool(selected);
  const auto other_lua =
      make_lua_eval_tool(std::make_shared<LuaEvalBindings>());
  state.add_tool(other_lua);
  state.set_tools({other_lua});
  ASSERT_EQ(state.tools().size(), 1u);
  EXPECT_EQ(state.tools()[0], selected);
  state.reset();
  ASSERT_EQ(state.tools().size(), 1u);
  EXPECT_EQ(state.tools()[0], selected);
  EXPECT_THROW(state.restrict_to_tool(other_lua), std::logic_error);
}

TEST_F(LuaWorker, ParallelInvocationsDoNotShareStateOrCwd) {
  auto selected = tool();
  auto call = [&]() {
    return selected->execute(
        "test",
        R"json({"requires":["lfs"],"script":"local lfs=require('lfs'); assert(lfs.chdir('scratch')); return lfs.currentdir()"})json",
        {}, {});
  };
  auto first = std::async(std::launch::async, call);
  auto second = std::async(std::launch::async, call);
  EXPECT_FALSE(first.get()->is_error());
  EXPECT_FALSE(second.get()->is_error());
}

TEST_F(LuaWorker, ResultChannelIsBoundedAndCannotRequestHostActions) {
  const auto result = execute(
      "local f=assert(require('io').open('/pici/result','w')); "
      "f:write(string.rep('x',1048577)); f:close(); require('os').exit(0)",
      {"io", "os"});
  EXPECT_FALSE(result["ok"].get<bool>()) << result;
}

TEST_F(LuaWorker, HardResourceLimitsAreInstalledBeforeTheWorkerExecutable) {
  const auto script = directory.path() / "alternate-worker";
  std::ofstream(script) << R"sh(#!/bin/sh
if ulimit -Hv unlimited 2>/dev/null; then exit 5; fi
printf '{"ok":true,"values":["%s","%s"]}' "$(ulimit -Hv)" "$(ulimit -Hn)" > /pici/result
)sh";
  std::filesystem::permissions(script, std::filesystem::perms::owner_all);
  auto alternate = std::make_shared<LuaWorkerAdapter>(script);
  LuaWorkerPolicy policy{.workspace = directory.path(),
                         .address_space_mb = 256};
  auto selected = make_lua_worker_tool(policy, alternate);
  const auto result = selected->execute(
      "test", R"json({"requires":[],"script":""})json", {}, {});
  ASSERT_FALSE(result->is_error()) << result->content();
  const auto frame = Json::parse(result->content());
  EXPECT_EQ(frame["values"][0], "262144");
  EXPECT_EQ(frame["values"][1], "128");
}

TEST_F(LuaWorker, WorkerImageIsPinnedAcrossWorkspaceExecutableReplacement) {
  const auto script = directory.path() / "replaceable-worker";
  auto write_script =
      [&](const std::string &value) {
        std::ofstream(script)
            << "#!/bin/sh\nprintf '%s' '{\"ok\":true,\"values\":[\"" << value
            << "\"]}' > /pici/result\n";
        std::filesystem::permissions(script, std::filesystem::perms::owner_all);
      };
  write_script("original");
  auto pinned = std::make_shared<LuaWorkerAdapter>(script);
  LuaWorkerPolicy policy{.workspace = directory.path()};
  auto selected = make_lua_worker_tool(policy, pinned);
  write_script("replacement");
  const auto arguments = R"json({"requires":[],"script":""})json";
  const auto result = selected->execute("test", arguments, {}, {});
  ASSERT_FALSE(result->is_error()) << result->content();
  EXPECT_EQ(Json::parse(result->content())["values"][0], "original");
  auto fresh =
      make_lua_worker_tool(policy, std::make_shared<LuaWorkerAdapter>(script));
  const auto replacement = fresh->execute("test", arguments, {}, {});
  ASSERT_FALSE(replacement->is_error()) << replacement->content();
  EXPECT_EQ(Json::parse(replacement->content())["values"][0], "replacement");
}

TEST_F(LuaWorker,
       HiddenToolsCannotBeDispatchedAndWorkerCallsUseNormalDispatch) {
  AgentState state;
  state.restrict_to_tool(tool());
  AgentContext context{.tools = state.tools()};
  AssistantMessage hidden{
      .content = {
          ToolCall{.id = "hidden",
                   .name = "bash",
                   .arguments = {{"command", "touch scratch/forbidden"}}}}};
  const auto denied =
      execute_tool_calls(context, hidden, {}, [](const AgentEvent &) {});
  ASSERT_EQ(denied.messages.size(), 1u);
  EXPECT_TRUE(denied.messages[0].is_error);
  EXPECT_FALSE(std::filesystem::exists(directory.path() / "scratch/forbidden"));
  AssistantMessage allowed{
      .content = {ToolCall{.id = "allowed",
                           .name = "lua",
                           .arguments = {{"requires", Json::array()},
                                         {"script", "return 42"}}}}};
  const auto completed =
      execute_tool_calls(context, allowed, {}, [](const AgentEvent &) {});
  ASSERT_EQ(completed.messages.size(), 1u);
  EXPECT_FALSE(completed.messages[0].is_error);
  const auto frame =
      Json::parse(std::get<TextContent>(completed.messages[0].content[0]).text);
  EXPECT_EQ(frame["values"][0], 42);
}

TEST_F(LuaWorker, MemoryExhaustionDoesNotAffectTheHost) {
  if (kSanitized)
    GTEST_SKIP() << "RLIMIT_AS is tested in the ordinary build; sanitizer "
                    "shadow memory needs unlimited address space";
  const auto result =
      execute("local t={}; while true do t[#t+1]=string.rep('x', 1048576) end",
              {}, false, {{"timeout_ms", 3000}});
  EXPECT_FALSE(result["ok"].get<bool>()) << result;
  EXPECT_TRUE(execute("return 'still alive'")["ok"].get<bool>());
  const auto integer = execute("return json.decode('18446744073709551615')");
  EXPECT_FALSE(integer["ok"].get<bool>());
}

TEST_F(LuaWorker, CancellationStopsAnAlreadyRunningCommand) {
  auto selected = tool(true);
  std::stop_source stop;
  auto future = std::async(std::launch::async, [&] {
    return selected->execute(
        "test",
        R"json({"requires":["io","os"],"script":"local f=assert(io.open('scratch/ready','w')); f:close(); os.execute('sleep 30')"})json",
        stop.get_token(), {});
  });
  EXPECT_TRUE(pi::test::wait_until(
      [&] {
        return std::filesystem::exists(directory.path() / "scratch/ready");
      },
      std::chrono::seconds(2), "worker entered script"));
  stop.request_stop();
  ASSERT_EQ(future.wait_for(std::chrono::seconds(3)),
            std::future_status::ready);
  EXPECT_TRUE(future.get()->is_error());
}

TEST_F(LuaWorker, CommandsCannotConnectToHostLoopbackListener) {
  if (access("/usr/bin/python3", X_OK) != 0)
    GTEST_SKIP() << "python3 is needed as a network probe";
  const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(listener, 0);
  struct Close {
    int fd;
    ~Close() { close(fd); }
  } close_listener{listener};
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  ASSERT_EQ(
      bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)),
      0);
  ASSERT_EQ(listen(listener, 1), 0);
  socklen_t length = sizeof(address);
  ASSERT_EQ(
      getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length),
      0);
  const auto script = "local ok=os.execute([[python3 -c 'import socket; "
                      "socket.create_connection((\"127.0.0.1\"," +
                      std::to_string(ntohs(address.sin_port)) +
                      "), 0.5)' 2>/dev/null]]); return ok == true";
  const auto result = execute(script, {"os"});
  ASSERT_TRUE(result["ok"].get<bool>()) << result;
  EXPECT_EQ(result["values"][0], false);
}
} // namespace
