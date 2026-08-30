#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace pi::acp {

struct MessagePart {
  std::string content_type{"text/plain"};
  std::string content;
  // content_encoding omitted (always plain for now)
};

void to_json(nlohmann::json &j, const MessagePart &p);
void from_json(const nlohmann::json &j, MessagePart &p);

// Named AcpMessage, not Message, per docs/architecture-lexicon.md: this is
// the ACP wire message shape (role + parts), a distinct type from
// core::TranscriptMessage (aliased as core::Message) -- keeping them both
// spelled "Message" invites exactly the transcript/wire confusion the
// lexicon calls out.
struct AcpMessage {
  std::string role; // "user" | "agent"
  std::vector<MessagePart> parts;
};

void to_json(nlohmann::json &j, const AcpMessage &m);
void from_json(const nlohmann::json &j, AcpMessage &m);

enum class RunMode { sync, stream };

struct RunCreateRequest {
  // ACP agent profile name (which manifest this server advertises under),
  // not an activation or session identity -- see ServerConfig::agent_name.
  std::string agent_name;
  std::vector<AcpMessage> input;
  std::optional<std::string> session_id;
  std::optional<std::string> provider;
  std::optional<std::string> model;
  RunMode mode{RunMode::stream};
};

void from_json(const nlohmann::json &j, RunCreateRequest &r);

enum class RunStatus {
  created,
  in_progress,
  awaiting,
  completed,
  failed,
  cancelled
};

std::string_view run_status_str(RunStatus s);

struct Run {
  std::string run_id;
  // ACP agent profile name, echoed from the request -- see
  // RunCreateRequest::agent_name.
  std::string agent_name;
  RunStatus status{RunStatus::created};
  std::vector<AcpMessage> output;
  std::optional<std::string> error;
  std::optional<std::string> session_id;
  std::optional<std::string> provider;
  std::optional<std::string> model;
};

void to_json(nlohmann::json &j, const Run &r);

struct AgentManifest {
  std::string name;
  std::string description;
  std::vector<std::string> input_content_types{"text/plain"};
  std::vector<std::string> output_content_types{"text/plain"};
  nlohmann::json metadata{nlohmann::json::object()};
};

void to_json(nlohmann::json &j, const AgentManifest &m);

} // namespace pi::acp
