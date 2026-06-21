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

struct Message {
  std::string role; // "user" | "agent"
  std::vector<MessagePart> parts;
};

void to_json(nlohmann::json &j, const Message &m);
void from_json(const nlohmann::json &j, Message &m);

// ────────────────────────────────────────────────────────��

enum class RunMode { sync, stream };

struct RunCreateRequest {
  std::string agent_name;
  std::vector<Message> input;
  std::optional<std::string> session_id;
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
  std::string agent_name;
  RunStatus status{RunStatus::created};
  std::vector<Message> output;
  std::optional<std::string> error;
  std::optional<std::string> session_id;
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
