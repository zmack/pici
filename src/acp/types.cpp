#include "acp/types.h"
#include "nlohmann/json_fwd.hpp"

#include <string_view>
#include <vector>

namespace pi::acp {

void to_json(nlohmann::json &j, const MessagePart &p) {
  j = {{"content_type", p.content_type}, {"content", p.content}};
}

void from_json(const nlohmann::json &j, MessagePart &p) {
  p.content_type = j.value("content_type", "text/plain");
  p.content = j.value("content", "");
}

void to_json(nlohmann::json &j, const Message &m) {
  j = {{"role", m.role}, {"parts", m.parts}};
}

void from_json(const nlohmann::json &j, Message &m) {
  m.role = j.value("role", "user");
  m.parts = j.value("parts", std::vector<MessagePart>{});
}

void from_json(const nlohmann::json &j, RunCreateRequest &r) {
  r.agent_name = j.value("agent_name", "");
  r.input = j.value("input", std::vector<Message>{});
  if (j.contains("session_id") && j["session_id"].is_string())
    r.session_id = j["session_id"].get<std::string>();
  auto mode_str = j.value("mode", "stream");
  r.mode = mode_str == "sync" ? RunMode::sync : RunMode::stream;
}

std::string_view run_status_str(RunStatus s) {
  switch (s) {
  case RunStatus::created:
    return "created";
  case RunStatus::in_progress:
    return "in-progress";
  case RunStatus::awaiting:
    return "awaiting";
  case RunStatus::completed:
    return "completed";
  case RunStatus::failed:
    return "failed";
  case RunStatus::cancelled:
    return "cancelled";
  }
  return "unknown";
}

void to_json(nlohmann::json &j, const Run &r) {
  j = {{"run_id", r.run_id},
       {"agent_name", r.agent_name},
       {"status", run_status_str(r.status)},
       {"output", r.output}};
  if (r.error)
    j["error"] = *r.error;
  if (r.session_id)
    j["session_id"] = *r.session_id;
}

void to_json(nlohmann::json &j, const AgentManifest &m) {
  j = {{"name", m.name},
       {"description", m.description},
       {"input_content_types", m.input_content_types},
       {"output_content_types", m.output_content_types},
       {"metadata", m.metadata}};
}

} // namespace pi::acp
