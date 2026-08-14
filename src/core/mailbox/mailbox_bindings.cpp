#include "core/mailbox/mailbox_bindings.h"

#include "core/agent_runtime_identity.h"
#include "core/lua_tool.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/mailbox/mailbox_types.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pi::core {
namespace {

constexpr std::int64_t kMaxWaitMs = 60'000;
constexpr std::size_t kMaxSendTextBytes = static_cast<std::size_t>(64) * 1024;

nlohmann::json mailbox_error(MailboxErrorCode code, std::string_view message) {
  return nlohmann::json{
      {"error",
       {{"code", mailbox_error_code_to_string(code)}, {"message", message}}}};
}

void require_object(const nlohmann::json &value) {
  if (!value.is_object())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "mailbox arguments must be an object");
}

bool get_bool(const nlohmann::json &value, std::string_view name,
              bool default_value) {
  if (!value.contains(name))
    return default_value;
  if (!value.at(name).is_boolean())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       std::string(name) + " must be a boolean");
  return value.at(name).get<bool>();
}

std::size_t get_limit(const nlohmann::json &value, std::string_view name,
                      std::size_t default_value, std::size_t maximum) {
  if (!value.contains(name))
    return default_value;
  const auto &raw = value.at(name);
  if (!raw.is_number_integer())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       std::string(name) + " must be an integer");
  if (raw.is_number_unsigned()) {
    const auto limit = raw.get<std::uint64_t>();
    if (limit == 0 || limit > maximum)
      throw MailboxError(MailboxErrorCode::invalid_message,
                         std::string(name) + " is out of range");
    return static_cast<std::size_t>(limit);
  }
  const auto limit = raw.get<std::int64_t>();
  if (limit < 1 || std::cmp_greater(limit, maximum))
    throw MailboxError(MailboxErrorCode::invalid_message,
                       std::string(name) + " is out of range");
  return static_cast<std::size_t>(limit);
}

std::int64_t get_timeout(const nlohmann::json &value, std::string_view name,
                         std::int64_t default_value) {
  if (!value.contains(name))
    return default_value;
  const auto &raw = value.at(name);
  if (!raw.is_number_integer())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       std::string(name) + " must be an integer");
  if (raw.is_number_unsigned() &&
      raw.get<std::uint64_t>() >
          static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    throw MailboxError(MailboxErrorCode::invalid_message,
                       std::string(name) + " is out of range");
  const auto timeout = raw.get<std::int64_t>();
  if (timeout < 0)
    throw MailboxError(MailboxErrorCode::invalid_message,
                       std::string(name) + " must not be negative");
  return std::min(timeout, kMaxWaitMs);
}

std::int64_t get_generation(const nlohmann::json &value) {
  if (!value.contains("after_generation"))
    return 0;
  const auto &raw = value.at("after_generation");
  if (!raw.is_number_integer() ||
      (raw.is_number_unsigned() &&
       raw.get<std::uint64_t>() >
           static_cast<std::uint64_t>(
               std::numeric_limits<std::int64_t>::max())) ||
      (!raw.is_number_unsigned() && raw.get<std::int64_t>() < 0))
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "after_generation must be a non-negative integer");
  return raw.get<std::int64_t>();
}

std::string required_string(const nlohmann::json &value,
                            std::string_view name) {
  if (!value.contains(name) || !value.at(name).is_string() ||
      value.at(name).get<std::string>().empty())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       std::string(name) + " must be a non-empty string");
  return value.at(name).get<std::string>();
}

std::string parse_text(const nlohmann::json &value) {
  if (!value.contains("text") || !value.at("text").is_string())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "text must be a non-empty string");
  auto text = value.at("text").get<std::string>();
  if (text.empty() || text.size() > kMaxSendTextBytes)
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "text must be between 1 and 65536 bytes");
  return text;
}

MailboxTarget parse_target(const nlohmann::json &value) {
  if (!value.contains("target") || !value.at("target").is_object())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "target must contain exactly one address");
  const auto &target = value.at("target");
  const bool has_session = target.contains("session_id");
  const bool has_agent = target.contains("agent_id");
  if (has_session == has_agent)
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "target must contain exactly one address");
  const auto *const key = has_session ? "session_id" : "agent_id";
  if (!target.at(key).is_string() || target.at(key).get<std::string>().empty())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       std::string(key) + " must be a non-empty string");
  return MailboxTarget{
      .session_id = has_session
                        ? std::optional<std::string>{target.at("session_id")}
                        : std::nullopt,
      .agent_id = has_agent ? std::optional<std::string>{target.at("agent_id")}
                            : std::nullopt};
}

MailboxMessageKind parse_kind(std::string_view value) {
  const auto kind = mailbox_message_kind_from_string(value);
  if (!kind)
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "unknown mailbox message kind");
  return *kind;
}

MailboxMessageKind parse_kind(const nlohmann::json &value,
                              std::string_view name,
                              std::string_view default_value) {
  if (!value.contains(name))
    return parse_kind(default_value);
  if (!value.at(name).is_string())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       std::string(name) + " must be a string");
  return parse_kind(value.at(name).get<std::string>());
}

std::vector<MailboxMessageKind> parse_kinds(const nlohmann::json &value) {
  if (!value.contains("kinds"))
    return {MailboxMessageKind::steer, MailboxMessageKind::note,
            MailboxMessageKind::request, MailboxMessageKind::reply};
  if (!value.at("kinds").is_array())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "kinds must be an array");
  std::vector<MailboxMessageKind> kinds;
  for (const auto &kind : value.at("kinds")) {
    if (!kind.is_string())
      throw MailboxError(MailboxErrorCode::invalid_message,
                         "kinds must contain strings");
    kinds.push_back(parse_kind(kind.get<std::string>()));
  }
  return kinds;
}

nlohmann::json agent_json(const AgentRecord &agent) {
  nlohmann::json result{{"agent_id", agent.agent_id},
                        {"process_id", agent.process_id},
                        {"kind", agent.kind},
                        {"session_id", agent.session_id},
                        {"workspace_id", agent.workspace_id},
                        {"workspace_path", agent.workspace_path},
                        {"provider", agent.provider},
                        {"model_id", agent.model_id},
                        {"status", agent.status},
                        {"started_at_ms", agent.started_at_ms},
                        {"last_seen_at_ms", agent.last_seen_at_ms},
                        {"lease_expires_at_ms", agent.lease_expires_at_ms}};
  if (agent.owner_agent_id)
    result["owner_agent_id"] = *agent.owner_agent_id;
  if (agent.session_name)
    result["session_name"] = *agent.session_name;
  if (agent.task_id)
    result["task_id"] = *agent.task_id;
  if (agent.task_path)
    result["task_path"] = *agent.task_path;
  if (agent.closed_at_ms)
    result["closed_at_ms"] = *agent.closed_at_ms;
  return result;
}

nlohmann::json message_json(const MailboxMessage &message) {
  nlohmann::json result{{"message_id", message.message_id},
                        {"sender_agent_id", message.sender_agent_id},
                        {"sender_session_id", message.sender_session_id},
                        {"recipient_session_id", message.recipient_session_id},
                        {"workspace_id", message.workspace_id},
                        {"kind", mailbox_message_kind_to_string(message.kind)},
                        {"text", message.body.text},
                        {"created_at_ms", message.created_at_ms},
                        {"available_at_ms", message.available_at_ms}};
  if (message.recipient_agent_id)
    result["recipient_agent_id"] = *message.recipient_agent_id;
  if (message.reply_to_message_id)
    result["reply_to"] = *message.reply_to_message_id;
  if (message.claim_token)
    result["claim_token"] = *message.claim_token;
  if (message.claim_expires_at_ms)
    result["claim_expires_at_ms"] = *message.claim_expires_at_ms;
  if (message.delivered_at_ms)
    result["delivered_at_ms"] = *message.delivered_at_ms;
  if (message.acknowledged_at_ms)
    result["acknowledged_at_ms"] = *message.acknowledged_at_ms;
  return result;
}

} // namespace

LuaHooks::MailboxBindings
make_mailbox_bindings(std::weak_ptr<MailboxCoordinator> coordinator,
                      const MailboxActorProvider &actor_provider) {
  auto mailbox_weak = std::move(coordinator);
  auto mailbox_call = [mailbox_weak,
                       actor_provider](const nlohmann::json &value, auto fn) {
    try {
      require_object(value);
      auto current = mailbox_weak.lock();
      if (!current)
        return mailbox_error(MailboxErrorCode::not_found,
                             "pici.mailbox is not available");
      if (!actor_provider)
        return mailbox_error(MailboxErrorCode::permission_denied,
                             "mailbox actor identity is required");
      const auto actor = actor_provider();
      if (!actor)
        return mailbox_error(MailboxErrorCode::not_found,
                             "mailbox actor identity is not active");
      return fn(*current, *actor);
    } catch (const MailboxError &error) {
      return mailbox_error(error.code(), error.what());
    } catch (const std::exception &error) {
      return mailbox_error(MailboxErrorCode::internal, error.what());
    }
  };

  LuaHooks::MailboxBindings result;
  result.self = [mailbox_call](const nlohmann::json &value,
                               const std::stop_token &) {
    return mailbox_call(value, [](MailboxCoordinator &coordinator,
                                  const AgentRuntimeIdentity &actor) {
      return agent_json(coordinator.self(actor));
    });
  };
  result.list = [mailbox_call](const nlohmann::json &value,
                               const std::stop_token &) {
    return mailbox_call(value, [&](MailboxCoordinator &coordinator,
                                   const AgentRuntimeIdentity &actor) {
      const bool include_self = get_bool(value, "include_self", false);
      const bool include_stale = get_bool(value, "include_stale", false);
      const auto limit = get_limit(value, "limit", 100, 100);
      const auto self = coordinator.self(actor);
      auto agents = coordinator.list_agents(
          actor, AgentQuery{.include_stale = include_stale,
                            .include_closed = false,
                            .limit = limit});
      nlohmann::json output = nlohmann::json::array();
      for (const auto &agent : agents) {
        if (!include_self && agent.agent_id == self.agent_id)
          continue;
        auto entry = agent_json(agent);
        entry["same_session_attached"] = agent.session_id == self.session_id &&
                                         agent.process_id != self.process_id;
        output.push_back(std::move(entry));
      }
      return output;
    });
  };
  result.send = [mailbox_call](const nlohmann::json &value,
                               const std::stop_token &) {
    return mailbox_call(value, [&](MailboxCoordinator &coordinator,
                                   const AgentRuntimeIdentity &actor) {
      auto request = SendRequest{.target = parse_target(value),
                                 .kind = parse_kind(value, "kind", "note"),
                                 .body = {.text = parse_text(value)}};
      if (request.kind != MailboxMessageKind::note &&
          request.kind != MailboxMessageKind::steer)
        throw MailboxError(MailboxErrorCode::invalid_message,
                           "send kind must be note or steer");
      if (value.contains("reply_to") && !value.at("reply_to").is_null())
        request.reply_to_message_id = required_string(value, "reply_to");
      const auto receipt = coordinator.send(actor, std::move(request));
      return nlohmann::json{
          {"message_id", receipt.message_id},
          {"recipient_session_id", receipt.recipient_session_id},
          {"recipient_agent_id",
           receipt.recipient_agent_id
               ? nlohmann::json(*receipt.recipient_agent_id)
               : nlohmann::json(nullptr)},
          {"created_at_ms", receipt.created_at_ms},
          {"state", "queued"}};
    });
  };
  result.request = [mailbox_call](const nlohmann::json &value,
                                  const std::stop_token stop_token) {
    return mailbox_call(value, [&](MailboxCoordinator &coordinator,
                                   const AgentRuntimeIdentity &actor) {
      const auto target = parse_target(value);
      const auto text = parse_text(value);
      const auto timeout = get_timeout(value, "timeout_ms", 30'000);
      if (stop_token.stop_requested())
        return mailbox_error(MailboxErrorCode::internal,
                             "mailbox request cancelled");
      const auto receipt = coordinator.send(
          actor, SendRequest{.target = target,
                             .kind = MailboxMessageKind::request,
                             .body = {.text = text}});
      const auto deadline =
          std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
      auto generation = coordinator.status().mailbox.latest_generation;
      for (;;) {
        if (stop_token.stop_requested())
          return mailbox_error(MailboxErrorCode::internal,
                               "mailbox request cancelled");
        // A successful request intentionally leaves its correlated reply
        // durable and unacknowledged; callers can inspect or acknowledge it.
        for (const auto &message : coordinator.inspect(
                 actor, InboxQuery{.kinds = {MailboxMessageKind::reply},
                                   .limit = 100})) {
          if (message.reply_to_message_id == receipt.message_id) {
            auto output = message_json(message);
            output["state"] = "replied";
            output["request_id"] = receipt.message_id;
            return output;
          }
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero())
          return nlohmann::json{{"state", "pending"},
                                {"request_id", receipt.message_id}};
        const auto change =
            coordinator.wait(WaitRequest{.after_generation = generation,
                                         .timeout_ms = std::min<std::int64_t>(
                                             remaining.count(), 250),
                                         .poll_interval_ms = 25},
                             stop_token);
        generation = std::max(change.generation, generation);
      }
    });
  };
  result.reply = [mailbox_call](const nlohmann::json &value,
                                const std::stop_token &) {
    return mailbox_call(value, [&](MailboxCoordinator &coordinator,
                                   const AgentRuntimeIdentity &actor) {
      const auto message_id = required_string(value, "message_id");
      const auto text = parse_text(value);
      const auto receipt =
          coordinator.reply(actor, message_id, MailboxBody{.text = text});
      return nlohmann::json{
          {"message_id", receipt.message_id},
          {"recipient_session_id", receipt.recipient_session_id},
          {"recipient_agent_id",
           receipt.recipient_agent_id
               ? nlohmann::json(*receipt.recipient_agent_id)
               : nlohmann::json(nullptr)},
          {"created_at_ms", receipt.created_at_ms},
          {"state", "queued"}};
    });
  };
  result.inbox = [mailbox_call](const nlohmann::json &value,
                                const std::stop_token &) {
    return mailbox_call(value, [&](MailboxCoordinator &coordinator,
                                   const AgentRuntimeIdentity &actor) {
      const auto limit = get_limit(value, "limit", 50, 50);
      const auto kinds = parse_kinds(value);
      const bool claim = get_bool(value, "claim", false);
      nlohmann::json output = nlohmann::json::array();
      if (claim) {
        for (const auto &message :
             coordinator
                 .claim(actor, ClaimRequest{.kinds = kinds, .limit = limit})
                 .messages)
          output.push_back(message_json(message));
      } else {
        for (const auto &message : coordinator.inspect(
                 actor, InboxQuery{.kinds = kinds, .limit = limit}))
          output.push_back(message_json(message));
      }
      return output;
    });
  };
  result.ack = [mailbox_call](const nlohmann::json &value,
                              const std::stop_token &) {
    return mailbox_call(value, [&](MailboxCoordinator &coordinator,
                                   const AgentRuntimeIdentity &actor) {
      const auto message_id = required_string(value, "message_id");
      const auto claim_token = required_string(value, "claim_token");
      coordinator.acknowledge(actor,
                              AcknowledgeRequest{.message_id = message_id,
                                                 .claim_token = claim_token});
      return nlohmann::json{{"message_id", message_id},
                            {"state", "acknowledged"}};
    });
  };
  result.wait = [mailbox_call](const nlohmann::json &value,
                               const std::stop_token stop_token) {
    return mailbox_call(value, [&](MailboxCoordinator &coordinator,
                                   const AgentRuntimeIdentity &) {
      const auto timeout = get_timeout(value, "timeout_ms", 30'000);
      const auto generation = get_generation(value);
      const auto result =
          coordinator.wait(WaitRequest{.after_generation = generation,
                                       .timeout_ms = timeout,
                                       .poll_interval_ms = 250},
                           stop_token);
      return nlohmann::json{{"timed_out", result.timed_out},
                            {"generation", result.generation},
                            {"presence_changed", result.presence_changed},
                            {"messages_changed", result.messages_changed}};
    });
  };
  result.status = [mailbox_call](const nlohmann::json &value,
                                 const std::stop_token &) {
    return mailbox_call(value, [](MailboxCoordinator &coordinator,
                                  const AgentRuntimeIdentity &) {
      const auto status = coordinator.status();
      return nlohmann::json{
          {"process_id", status.process_id},
          {"agent_id", status.root_agent_id},
          {"session_id", status.session_id.value_or("")},
          {"status", status.status},
          {"provider", status.provider},
          {"model_id", status.model_id},
          {"root_active", status.root_active},
          {"root_running", status.root_running},
          {"schema_version", status.mailbox.schema_version},
          {"workspace_id", status.mailbox.workspace_id},
          {"live_agents", status.mailbox.live_agents},
          {"unread_messages", status.mailbox.unread_messages},
          {"latest_generation", status.mailbox.latest_generation}};
    });
  };
  return result;
}

} // namespace pi::core
