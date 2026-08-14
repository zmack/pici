#pragma once

#include <optional>
#include <string>

namespace pi::core {

// The endpoint identity for one live root or subagent activation.  The
// durable session identifies a conversation; agent_id identifies this
// activation within that conversation.
struct AgentRuntimeIdentity {
  std::string agent_id;
  std::string session_id;
  std::string kind;
  std::optional<std::string> task_id;
  std::optional<std::string> task_path;
  std::optional<std::string> owner_agent_id;

  bool is_root() const noexcept { return kind == "root"; }
};

} // namespace pi::core
