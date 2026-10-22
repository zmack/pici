#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pi::core {

enum class SandboxMode { auto_mode, required, disabled };

std::optional<SandboxMode> sandbox_mode_from_string(std::string_view value);
std::string_view sandbox_mode_to_string(SandboxMode mode);

class SandboxPolicy {
public:
  explicit SandboxPolicy(SandboxMode mode = SandboxMode::auto_mode)
      : mode_(mode) {}

  SandboxMode mode() const { return mode_.load(); }
  void set_mode(SandboxMode mode) { mode_.store(mode); }

private:
  std::atomic<SandboxMode> mode_;
};

using SandboxPolicyPtr = std::shared_ptr<SandboxPolicy>;

struct SandboxCommand {
  SandboxMode mode{SandboxMode::disabled};
  std::filesystem::path cwd;
  std::string_view command;
};

// Values assembled by the host before spawning. Source descriptors pin mount
// objects against path replacement; the caller maps them into the child.
struct SandboxMount {
  int source_fd;
  std::string destination;
  bool writable{false};
  bool copy_data{false}; // sealed executable bytes copied to a private mount
};

struct SandboxProgram {
  std::vector<SandboxMount> mounts;
  std::string cwd{"/workspace"};
  std::vector<std::string> argv;
  std::uint64_t address_space_mb{1024};
};

class SandboxLauncher {
public:
  static bool bubblewrap_available();
  static void validate(SandboxMode mode);

  // Host-side argv construction for constrained workers. Never uses a shell,
  // inherits no environment, and has no unsandboxed fallback.
  static std::vector<std::string> program_argv(const SandboxProgram &program);

  // Called in the forked child. This function never returns.
  [[noreturn]] static void exec(const SandboxCommand &command);
};

} // namespace pi::core
