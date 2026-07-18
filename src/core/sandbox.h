#pragma once

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string_view>

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

class SandboxLauncher {
public:
  static bool bubblewrap_available();
  static void validate(SandboxMode mode);

  // Called in the forked child. This function never returns.
  [[noreturn]] static void exec(const SandboxCommand &command);
};

} // namespace pi::core
