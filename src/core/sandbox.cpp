#include "core/sandbox.h"

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace pi::core {

std::optional<SandboxMode> sandbox_mode_from_string(std::string_view value) {
  if (value == "auto")
    return SandboxMode::auto_mode;
  if (value == "required")
    return SandboxMode::required;
  if (value == "disabled" || value == "off")
    return SandboxMode::disabled;
  return std::nullopt;
}

std::string_view sandbox_mode_to_string(SandboxMode mode) {
  switch (mode) {
  case SandboxMode::auto_mode:
    return "auto";
  case SandboxMode::required:
    return "required";
  case SandboxMode::disabled:
    return "disabled";
  }
  return "disabled";
}

namespace {

bool executable(const std::filesystem::path &path) {
  return ::access(path.c_str(), X_OK) == 0;
}

bool executable_in_path(std::string_view name) {
  const char *path_env = std::getenv("PATH"); // NOLINT(concurrency-mt-unsafe)
  if (path_env == nullptr || *path_env == '\0')
    return false;

  std::string path(path_env);
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto end = path.find(':', start);
    const auto directory = path.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    const auto candidate =
        (directory.empty() ? std::filesystem::path(".")
                           : std::filesystem::path(directory)) /
        name;
    if (executable(candidate))
      return true;
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return false;
}

[[noreturn]] void exec_direct(const SandboxCommand &command) {
  if (::chdir(command.cwd.c_str()) != 0)
    ::_exit(126);
  ::execl("/bin/sh", "sh", "-c", std::string(command.command).c_str(), nullptr);
  ::_exit(127);
}

} // namespace

bool SandboxLauncher::bubblewrap_available() {
  return executable_in_path("bwrap");
}

std::vector<std::string>
SandboxLauncher::program_argv(const SandboxProgram &program) {
  std::string binary;
  for (const char *path : {"/usr/bin/bwrap", "/bin/bwrap"}) {
    if (executable(path)) {
      binary = path;
      break;
    }
  }
  if (binary.empty())
    throw std::runtime_error("Lua-only mode requires bubblewrap");
  std::string prlimit;
  for (const char *path : {"/usr/bin/prlimit", "/bin/prlimit"}) {
    if (executable(path)) {
      prlimit = path;
      break;
    }
  }
  if (prlimit.empty())
    throw std::runtime_error("Lua-only mode requires util-linux prlimit");
  if (program.address_space_mb > 65536 ||
      (program.address_space_mb != 0 && program.address_space_mb < 64))
    throw std::invalid_argument("invalid sandbox address-space limit");
  if (program.argv.empty())
    throw std::invalid_argument("sandbox executable is missing");
  std::vector<std::string> args{
      binary,           "--die-with-parent",
      "--new-session",  "--unshare-all",
      "--unshare-user", "--disable-userns",
      "--cap-drop",     "ALL",
      "--clearenv",     "--setenv",
      "PATH",           "/usr/local/bin:/usr/bin:/bin",
      "--setenv",       "HOME",
      "/tmp/home",      "--setenv",
      "TMPDIR",         "/tmp",
      "--setenv",       "LC_ALL",
      "C.UTF-8",        "--proc",
      "/proc",          "--dev",
      "/dev",           "--size",
      "67108864",       "--tmpfs",
      "/tmp",           "--dir",
      "/tmp/home",      "--dir",
      "/workspace",     "--dir",
      "/pici"};
  for (const auto &mount : program.mounts) {
    if (mount.copy_data) {
      args.insert(args.end(),
                  {"--perms", "0500", "--ro-bind-data",
                   std::to_string(mount.source_fd), mount.destination});
      continue;
    }
    args.insert(args.end(),
                {mount.writable ? "--bind-fd" : "--ro-bind-fd",
                 std::to_string(mount.source_fd), mount.destination});
  }
  // Even the scaffolding is read-only. Explicit writable mounts and /tmp are
  // separate mounts and retain their intended permissions.
  args.insert(args.end(), {"--remount-ro", "/", "--chdir", program.cwd, "--"});
  args.insert(args.end(), program.argv.begin(), program.argv.end());
  // Apply hard limits outside the Lua worker, before any model-controlled
  // code or native extension runs. Replacing a development-build worker in
  // an explicitly writable workspace cannot opt out of these limits.
  std::vector<std::string> bounded{prlimit,
                                   "--core=0:0",
                                   "--fsize=268435456:268435456",
                                   "--nofile=128:128",
                                   "--cpu=120:120",
                                   "--nproc=256:256"};
  if (program.address_space_mb != 0) {
    const auto bytes = std::to_string(program.address_space_mb * 1024 * 1024);
    bounded.push_back("--as=" + bytes + ":" + bytes);
  }
  bounded.emplace_back("--");
  bounded.insert(bounded.end(), args.begin(), args.end());
  return bounded;
}

void SandboxLauncher::validate(SandboxMode mode) {
  if (mode == SandboxMode::disabled)
    return;
  if (!bubblewrap_available()) {
    throw std::runtime_error(
        "bubblewrap is unavailable; use --sandbox disabled to run bash "
        "without a sandbox");
  }
}

[[noreturn]] void SandboxLauncher::exec(const SandboxCommand &command) {
  if (command.mode == SandboxMode::disabled)
    exec_direct(command);

  std::vector<std::string> args{
      "bwrap",
      "--die-with-parent",
      "--new-session",
      "--unshare-user",
      "--unshare-pid",
      "--unshare-ipc",
      "--unshare-uts",
      "--unshare-net",
      "--proc",
      "/proc",
      "--dev",
      "/dev",
      "--tmpfs",
      "/tmp",
      "--dir",
      "/tmp/home",
      "--dir",
      "/workspace",
      "--bind",
      command.cwd.string(),
      "/workspace",
      "--ro-bind-try",
      "/usr",
      "/usr",
      "--ro-bind-try",
      "/bin",
      "/bin",
      "--ro-bind-try",
      "/sbin",
      "/sbin",
      "--ro-bind-try",
      "/lib",
      "/lib",
      "--ro-bind-try",
      "/lib64",
      "/lib64",
      "--ro-bind-try",
      "/etc",
      "/etc",
      "--setenv",
      "HOME",
      "/tmp/home",
      "--setenv",
      "PATH",
      "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
      "--setenv",
      "PWD",
      "/workspace",
      "--chdir",
      "/workspace",
      "/bin/sh",
      "-c",
      std::string(command.command)};

  std::vector<char *> argv;
  argv.reserve(args.size() + 1);
  for (auto &arg : args)
    argv.push_back(arg.data());
  argv.push_back(nullptr);
  ::execvp(argv[0], argv.data());
  ::_exit(127);
}

} // namespace pi::core
