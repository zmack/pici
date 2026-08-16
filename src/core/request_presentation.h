#pragma once

#include <optional>
#include <string>

namespace pi::core {

enum class RequestSource { ordinary, mailbox, follow_up };

struct RequestPresentation {
  RequestSource source{RequestSource::ordinary};
  std::optional<std::string> message_id;
  std::optional<std::string> message_kind;
  std::optional<std::string> sender_agent_id;
  std::optional<std::string> sender_session_id;
  std::optional<std::string> sender_task_path;
  std::optional<std::string> sender_session_name;
};

} // namespace pi::core
