#pragma once

#include "core/message_types.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace pi::core {

// Value held by the session's tool binding for its activation. Immutable once
// validated; Lua and protocol output never grant authority.
struct LuaWorkerPolicy {
  std::filesystem::path workspace;
  std::vector<std::string> libraries{"io", "os", "lfs"};
  std::vector<std::filesystem::path> read_roots{"."};
  std::vector<std::filesystem::path> write_roots;
  std::uint64_t address_space_mb{1024}; // 0 explicitly disables this limit
};

// Reusable process-owned Adapter. Each call owns its child process and IO;
// the adapter owns no domain/session state. No host RPC is exposed.
class LuaWorkerAdapter {
public:
  explicit LuaWorkerAdapter(std::filesystem::path executable = {});
  const std::filesystem::path &executable() const { return executable_; }
  void validate() const;
  std::shared_ptr<ToolResult> execute(const LuaWorkerPolicy &policy,
                                      std::string_view arguments,
                                      const std::stop_token &stop) const;

private:
  struct Image;
  std::shared_ptr<const Image> image() const;
  std::filesystem::path executable_;
  mutable std::mutex image_mutex_;
  mutable std::shared_ptr<const Image> image_;
};

std::shared_ptr<const ToolDefinition>
make_lua_worker_tool(LuaWorkerPolicy policy,
                     std::shared_ptr<const LuaWorkerAdapter> adapter);

} // namespace pi::core
