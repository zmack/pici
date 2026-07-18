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
