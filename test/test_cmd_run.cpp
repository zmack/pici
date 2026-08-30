// Characterization tests for cmd_run() (src/main.cpp), taken before
// refactoring it. cmd_run() has no direct test coverage today and
// everything in main.cpp lives in one anonymous namespace with internal
// linkage, so it cannot be linked into a test binary and called in-process.
// These tests instead drive the real, compiled pi-cli binary as a
// subprocess -- true end-to-end characterization of cmd_run()'s externally
// visible behavior (stdout/stderr/exit code), covering the bootstrap phase
// (model resolution, session/tool/hook setup, --list-tools/--list-addons
// early exits, and the validation error paths) that accounts for most of
// cmd_run()'s branch count. The interactive REPL loop and --mode rpc are
// not covered here: both need a PTY/stdin driver this file doesn't build,
// and the refactor is expected to leave them as thin, already-tested
// delegations (cli::run_rpc_mode is covered by test_rpc_mode.cpp).

#include <gtest/gtest.h>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef PI_CLI_BINARY
#error "PI_CLI_BINARY must be defined to the built pi-cli executable path"
#endif

namespace {

constexpr const char *kBinaryPath = PI_CLI_BINARY;

// Unique, self-cleaning directory used as both $HOME and the isolated
// --session-dir for one test, so a run can never read or write the
// developer's real config/session/auth files.
struct TempDir {
  std::filesystem::path path;

  TempDir() {
    path = std::filesystem::temp_directory_path() /
           ("pici-cmd-run-test-" + std::to_string(::getpid()) + "-" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
  }
  ~TempDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  TempDir(const TempDir &) = delete;
  TempDir &operator=(const TempDir &) = delete;

  std::filesystem::path operator/(std::string_view leaf) const {
    return path / leaf;
  }
};

// argv/env plumbing shared by the blocking and background spawn paths.
[[noreturn]] void exec_cli_child(const std::vector<std::string> &args,
                                 const std::filesystem::path &home_dir) {
  ::setenv("HOME", home_dir.c_str(), 1);
  ::setenv("XDG_CONFIG_HOME", (home_dir / ".config").c_str(), 1);
  ::unsetenv("PICI_CONFIG");
  std::vector<std::string> argv_storage{kBinaryPath};
  argv_storage.insert(argv_storage.end(), args.begin(), args.end());
  std::vector<char *> argv;
  argv.reserve(argv_storage.size() + 1);
  for (auto &s : argv_storage)
    argv.push_back(s.data());
  argv.push_back(nullptr);
  ::execv(kBinaryPath, argv.data());
  ::_exit(127); // execv only returns on failure
}

struct RunResult {
  bool exited_normally = false;
  int exit_code = -1;
  std::string out;
  std::string err;
};

// Runs pi-cli to completion (with a generous timeout as a safety net against
// a test case that accidentally reaches an interactive/blocking code path),
// capturing stdout/stderr.
RunResult run_cli(const std::vector<std::string> &args,
                  const std::filesystem::path &home_dir,
                  std::chrono::milliseconds timeout = std::chrono::seconds(15)) {
  std::array<int, 2> out_pipe{-1, -1};
  std::array<int, 2> err_pipe{-1, -1};
  if (::pipe(out_pipe.data()) != 0 || ::pipe(err_pipe.data()) != 0)
    throw std::runtime_error("pipe() failed");

  const pid_t pid = ::fork();
  if (pid < 0)
    throw std::runtime_error("fork() failed");
  if (pid == 0) {
    const int devnull_in = ::open("/dev/null", O_RDONLY);
    if (devnull_in >= 0) {
      ::dup2(devnull_in, STDIN_FILENO);
      if (devnull_in != STDIN_FILENO)
        ::close(devnull_in);
    }
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);
    exec_cli_child(args, home_dir);
  }

  ::close(out_pipe[1]);
  ::close(err_pipe[1]);
  ::fcntl(out_pipe[0], F_SETFL, O_NONBLOCK);
  ::fcntl(err_pipe[0], F_SETFL, O_NONBLOCK);

  RunResult result;
  bool out_eof = false;
  bool err_eof = false;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::array<char, 4096> buf{};
  bool timed_out = false;
  while (!out_eof || !err_eof) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining <= std::chrono::milliseconds::zero()) {
      timed_out = true;
      break;
    }
    std::array<pollfd, 2> fds{{
        {.fd = out_pipe[0], .events = POLLIN, .revents = 0},
        {.fd = err_pipe[0], .events = POLLIN, .revents = 0},
    }};
    const int n = ::poll(fds.data(), 2, static_cast<int>(remaining.count()));
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (!out_eof && (fds[0].revents & (POLLIN | POLLHUP)) != 0) {
      const auto r = ::read(out_pipe[0], buf.data(), buf.size());
      if (r > 0)
        result.out.append(buf.data(), static_cast<std::size_t>(r));
      else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        out_eof = true;
    }
    if (!err_eof && (fds[1].revents & (POLLIN | POLLHUP)) != 0) {
      const auto r = ::read(err_pipe[0], buf.data(), buf.size());
      if (r > 0)
        result.err.append(buf.data(), static_cast<std::size_t>(r));
      else if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
        err_eof = true;
    }
  }
  ::close(out_pipe[0]);
  ::close(err_pipe[0]);

  if (timed_out)
    ::kill(pid, SIGKILL);
  int status = 0;
  ::waitpid(pid, &status, 0);
  if (WIFEXITED(status)) {
    result.exited_normally = true;
    result.exit_code = WEXITSTATUS(status);
  }
  return result;
}

// Starts pi-cli in the background (stdio discarded) for tests that need to
// interact with it while it runs, e.g. over the --faux-control socket.
// Caller is responsible for reaping it (wait_for_background_exit) so the
// process doesn't outlive the test on failure.
pid_t spawn_cli_background(const std::vector<std::string> &args,
                           const std::filesystem::path &home_dir) {
  const pid_t pid = ::fork();
  if (pid < 0)
    throw std::runtime_error("fork() failed");
  if (pid == 0) {
    const int devnull = ::open("/dev/null", O_RDWR);
    if (devnull >= 0) {
      ::dup2(devnull, STDIN_FILENO);
      ::dup2(devnull, STDOUT_FILENO);
      ::dup2(devnull, STDERR_FILENO);
      if (devnull > STDERR_FILENO)
        ::close(devnull);
    }
    exec_cli_child(args, home_dir);
  }
  return pid;
}

// Polls (non-blocking) for the background process to exit, killing it if it
// hasn't by the deadline. Returns true iff it exited on its own with the
// expected code.
bool wait_for_background_exit(pid_t pid, int expected_exit_code,
                              std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto waited = ::waitpid(pid, &status, WNOHANG);
    if (waited == pid)
      return WIFEXITED(status) && WEXITSTATUS(status) == expected_exit_code;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  ::kill(pid, SIGKILL);
  ::waitpid(pid, &status, 0);
  return false;
}

int connect_unix_socket(const std::string &path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<const sockaddr *>(&address), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
                sizeof(address)) == 0)
    return fd;
  ::close(fd);
  return -1;
}

int connect_unix_socket_retrying(const std::string &path, int attempts = 200) {
  for (int attempt = 0; attempt < attempts; ++attempt) {
    const int fd = connect_unix_socket(path);
    if (fd >= 0)
      return fd;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return -1;
}

bool send_all(int fd, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const auto count =
        ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (count <= 0)
      return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

void write_file(const std::filesystem::path &path, std::string_view content) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream f(path);
  f << content;
}

std::vector<std::string> isolation_args(const TempDir &home,
                                        std::vector<std::string> extra = {}) {
  std::vector<std::string> args = {
      "--session-dir", (home / "sessions").string(),
      "--config", (home / "no-such-config.toml").string(),
      "--sandbox", "disabled",
      "--no-context-files",
      "--no-skills",
  };
  args.insert(args.end(), extra.begin(), extra.end());
  return args;
}

} // namespace

TEST(CmdRun, ListTools_DefaultBuiltinToolsPrinted) {
  TempDir home;
  auto result = run_cli(isolation_args(home, {"--list-tools"}), home.path);
  ASSERT_TRUE(result.exited_normally);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.out.find("read\n"), std::string::npos) << result.out;
  EXPECT_NE(result.out.find("bash\n"), std::string::npos) << result.out;
  EXPECT_NE(result.out.find("edit\n"), std::string::npos) << result.out;
  EXPECT_NE(result.out.find("write\n"), std::string::npos) << result.out;
  EXPECT_NE(result.out.find("grep\n"), std::string::npos) << result.out;
}

TEST(CmdRun, ListTools_AllowlistFiltersToRequestedTools) {
  TempDir home;
  auto result = run_cli(
      isolation_args(home, {"--list-tools", "--tools", "read,bash"}),
      home.path);
  ASSERT_TRUE(result.exited_normally);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.out.find("read\n"), std::string::npos) << result.out;
  EXPECT_NE(result.out.find("bash\n"), std::string::npos) << result.out;
  EXPECT_EQ(result.out.find("edit\n"), std::string::npos) << result.out;
  EXPECT_EQ(result.out.find("write\n"), std::string::npos) << result.out;
  EXPECT_EQ(result.out.find("grep\n"), std::string::npos) << result.out;
}

TEST(CmdRun, ListTools_NoToolsPrintsEmptyMessage) {
  TempDir home;
  auto result =
      run_cli(isolation_args(home, {"--list-tools", "--no-tools"}), home.path);
  ASSERT_TRUE(result.exited_normally);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.out.find("(no tools loaded)"), std::string::npos)
      << result.out;
}

TEST(CmdRun, ListTools_ToolsDirLoadsLuaTool) {
  TempDir home;
  const auto tools_dir = home / "lua-tools";
  write_file(tools_dir / "greet.lua", R"lua(
return {
  name = "greet",
  description = "Returns a greeting",
  schema = '{"type":"object","properties":{},"additionalProperties":false}',
  execute = function(args)
    return "hi"
  end
}
)lua");
  auto result = run_cli(
      isolation_args(home, {"--list-tools", "--tools-dir", tools_dir.string()}),
      home.path);
  ASSERT_TRUE(result.exited_normally);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.out.find("greet\n"), std::string::npos) << result.out;
  // Builtin tools still load alongside the Lua one.
  EXPECT_NE(result.out.find("read\n"), std::string::npos) << result.out;
}

TEST(CmdRun, ListAddons_NoHooksPrintsEmptyMessage) {
  TempDir home;
  auto result = run_cli(isolation_args(home, {"--list-addons"}), home.path);
  ASSERT_TRUE(result.exited_normally);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.out.find("(no add-ons loaded)"), std::string::npos)
      << result.out;
}

TEST(CmdRun, ListAddons_HooksFileShowsRegisteredCommand) {
  TempDir home;
  const auto hooks_file = home / "greet_hook.lua";
  write_file(hooks_file, R"lua(
return {
  commands = {
    { name = "greet", description = "says hi", args_hint = "[name]" },
  },
}
)lua");
  auto result = run_cli(
      isolation_args(home, {"--list-addons", "--hooks-file",
                           hooks_file.string()}),
      home.path);
  ASSERT_TRUE(result.exited_normally);
  EXPECT_EQ(result.exit_code, 0);
  EXPECT_NE(result.out.find("/greet"), std::string::npos) << result.out;
  EXPECT_NE(result.out.find("says hi"), std::string::npos) << result.out;
}

TEST(CmdRun, OpenaiCodexWithApiKey_Rejected) {
  TempDir home;
  write_file(home / "config.toml", R"toml(
[providers.openai-codex]
api = "openai-codex-responses"
base_url = "http://openai-codex.test"
auth = "oauth"

[[providers.openai-codex.models]]
id = "test-model"
)toml");
  std::vector<std::string> args = {
      "--session-dir", (home / "sessions").string(),
      "--config", (home / "config.toml").string(),
      "--sandbox", "disabled",
      "--no-context-files",
      "--no-skills",
      "--provider", "openai-codex",
      "--api-key", "sk-test",
      "--list-tools",
  };
  auto result = run_cli(args, home.path);
  ASSERT_TRUE(result.exited_normally);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.err.find("--api-key cannot be used with openai-codex"),
           std::string::npos)
      << result.err;
}

TEST(CmdRun, StreamTraceUnwritablePath_Errors) {
  TempDir home;
  auto result = run_cli(
      isolation_args(home, {"--list-tools", "--stream-trace",
                           (home / "no/such/dir/trace.jsonl").string()}),
      home.path);
  ASSERT_TRUE(result.exited_normally);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.err.find("error:"), std::string::npos) << result.err;
  EXPECT_NE(result.err.find("stream trace"), std::string::npos) << result.err;
}

TEST(CmdRun, ContinueWithNoPreviousSession_Errors) {
  TempDir home;
  auto result =
      run_cli(isolation_args(home, {"--continue", "--list-tools"}), home.path);
  ASSERT_TRUE(result.exited_normally);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.err.find("no previous session found"), std::string::npos)
      << result.err;
}

TEST(CmdRun, InvalidSandboxMode_Errors) {
  TempDir home;
  std::vector<std::string> args = {
      "--session-dir", (home / "sessions").string(),
      "--config", (home / "no-such-config.toml").string(),
      "--no-context-files",
      "--no-skills",
      "--sandbox", "bogus-mode",
      "--list-tools",
  };
  auto result = run_cli(args, home.path);
  ASSERT_TRUE(result.exited_normally);
  EXPECT_EQ(result.exit_code, 1);
  EXPECT_NE(result.err.find("invalid sandbox mode"), std::string::npos)
      << result.err;
}

// End-to-end smoke test for the --faux-control dispatch branch: unlike the
// cases above, this path skips builtin/Lua tool loading entirely (gated on
// faux_control_socket.empty()) and instead registers a scripted LLM client,
// then hands off to cli::run_faux_control_socket -- exercised here through
// the real compiled binary rather than in-process (see
// test_faux_control_mode.cpp for direct coverage of the socket protocol
// itself; this test only checks that cmd_run() reaches and returns from it
// cleanly).
TEST(CmdRun, FauxControlSocket_BootstrapsAndAcceptsQuit) {
  TempDir home;
  const auto socket_path = home / "faux.sock";
  const pid_t pid = spawn_cli_background(
      isolation_args(home, {"--faux-control", socket_path.string()}),
      home.path);

  const int fd = connect_unix_socket_retrying(socket_path.string());
  ASSERT_GE(fd, 0);
  ASSERT_TRUE(send_all(fd, R"({"type":"quit","id":"quit"})""\n"));
  ::close(fd);

  EXPECT_TRUE(wait_for_background_exit(pid, 0, std::chrono::seconds(10)));
  EXPECT_FALSE(std::filesystem::exists(socket_path));
}
