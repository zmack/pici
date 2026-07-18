#include "cli/rpc_mode.h"

#include "core/event_json.h"
#include "core/event_types.h"
#include "core/message_types.h"
#include "core/session/agent_session.h"
#include "core/session/session_id.h"
#include "core/session/session_record.h"

#include <chrono>
#include <exception>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace pi::cli {

namespace {

nlohmann::json as_json( // NOLINT(misc-include-cleaner)
    const core::Message &message) {
  return nlohmann::json::parse(core::json::to_json(message));
}

nlohmann::json as_json(const core::Model &model) {
  return nlohmann::json::parse(core::json::to_json(model));
}

nlohmann::json task_snapshot_json(const core::AgentTaskSnapshot &snapshot) {
  nlohmann::json value = {
      {"id", snapshot.id},
      {"task_path", snapshot.task_path},
      {"task_name", snapshot.task_name},
      {"status", core::agent_task_status_to_string(snapshot.status)},
      {"child_count", snapshot.child_count},
      {"queued_message_count", snapshot.queued_message_count},
      {"generation", snapshot.generation},
  };
  if (snapshot.parent_id)
    value["parent_id"] = *snapshot.parent_id;
  else
    value["parent_id"] = nullptr;
  if (snapshot.result) {
    value["result"] = {
        {"text", snapshot.result->text},
        {"stop_reason", core::stop_reason_to_string(snapshot.result->stop_reason)},
        {"truncated", snapshot.result->truncated},
    };
    if (snapshot.result->error)
      value["result"]["error"] = *snapshot.result->error;
    else
      value["result"]["error"] = nullptr;
  }
  return value;
}

core::Message rpc_message(const nlohmann::json &command) {
  core::UserMessage message;
  message.content.emplace_back(
      core::TextContent{.text = command.value("message", std::string{})});
  return message;
}

core::TurnAbortReason rpc_abort_reason(std::string_view reason) {
  if (reason == "parent")
    return core::TurnAbortReason::parent_interrupt;
  if (reason == "shutdown")
    return core::TurnAbortReason::shutdown;
  if (reason == "timeout")
    return core::TurnAbortReason::timeout;
  if (reason == "budget")
    return core::TurnAbortReason::budget;
  return core::TurnAbortReason::user_interrupt;
}

std::optional<core::ContextInheritanceMode>
rpc_context_mode(std::string_view mode) {
  if (mode == "none")
    return core::ContextInheritanceMode::none;
  if (mode == "full")
    return core::ContextInheritanceMode::full;
  if (mode == "through_message")
    return core::ContextInheritanceMode::through_message;
  if (mode == "recent_messages")
    return core::ContextInheritanceMode::recent_messages;
  return std::nullopt;
}

std::optional<core::AgentInterruptReason>
rpc_interrupt_reason(std::string_view reason) {
  if (reason == "user")
    return core::AgentInterruptReason::user;
  if (reason == "parent")
    return core::AgentInterruptReason::parent;
  if (reason == "shutdown")
    return core::AgentInterruptReason::shutdown;
  if (reason == "timeout")
    return core::AgentInterruptReason::timeout;
  if (reason == "budget")
    return core::AgentInterruptReason::budget;
  if (reason == "replacement_task")
    return core::AgentInterruptReason::replacement_task;
  return std::nullopt;
}

std::optional<core::ThinkingLevel> parse_thinking(std::string_view level) {
  for (const auto candidate :
       {core::ThinkingLevel::off, core::ThinkingLevel::minimal,
        core::ThinkingLevel::low, core::ThinkingLevel::medium,
        core::ThinkingLevel::high, core::ThinkingLevel::xhigh}) {
    if (core::thinking_level_to_string(candidate) == level)
      return candidate;
  }
  return std::nullopt;
}

} // namespace

RpcMode::RpcMode(core::AgentSession &session, Output output,
                 core::AgentTaskManager *task_manager)
    : session_(session), task_manager_(task_manager), output_(std::move(output)) {}

RpcMode::~RpcMode() { stop(); }

void RpcMode::emit(const nlohmann::json &value) const {
  std::scoped_lock lock(output_mutex_);
  output_(value);
}

void RpcMode::response(const nlohmann::json &command, bool success,
                       nlohmann::json data, std::string error) const {
  nlohmann::json result = {{"type", "response"},
                           {"command", command.value("type", "")},
                           {"success", success}};
  if (command.contains("id"))
    result["id"] = command["id"];
  if (success && !data.is_null())
    result["data"] = std::move(data);
  if (!success)
    result["error"] = std::move(error);
  emit(result);
}

void RpcMode::start_prompt(const nlohmann::json &command, std::string message) {
  if (run_active_.exchange(true) || session_.agent().is_streaming()) {
    response(command, false, nullptr,
             "agent is already processing; use steer or follow_up");
    return;
  }
  if (run_thread_.joinable())
    run_thread_.join();
  response(command, true);
  run_thread_ = std::jthread([this, message = std::move(message)] {
    const auto result =
        session_.run_prompt(message, [this](const core::AgentEvent &event) {
          emit(core::event_to_json(event));
        });
    if (result.error)
      emit({{"type", "run.failed"}, {"error", *result.error}});
    else
      emit({{"type", "run.completed"}});
    run_active_ = false;
  });
}

void RpcMode::start_wait(const nlohmann::json &command) {
  if (!task_manager_) {
    response(command, false, nullptr, "agent task manager is unavailable");
    return;
  }
  core::AgentWaitRequest request;
  if (command.contains("targets"))
    request.targets = command.at("targets").get<std::vector<std::string>>();
  request.after_generation = command.value("after_generation", 0ULL);
  request.timeout = std::chrono::milliseconds(
      command.value("timeout_ms", 30000ULL));
  std::scoped_lock lock(wait_mutex_);
  wait_threads_.emplace_back(
      [this, command, request](std::stop_token stop_token) {
        try {
          const auto result = task_manager_->wait(request, stop_token);
          nlohmann::json changed = nlohmann::json::array();
          for (const auto &snapshot : result.changed)
            changed.push_back(task_snapshot_json(snapshot));
          response(command, true,
                   {{"timed_out", result.timed_out},
                    {"caller_interrupted", result.caller_interrupted},
                    {"generation", result.generation},
                    {"changed", std::move(changed)}});
        } catch (const std::exception &error) {
          response(command, false, nullptr, error.what());
        }
      });
}

void RpcMode::handle(const nlohmann::json &command) {
  if (!command.is_object() || !command.contains("type") ||
      !command["type"].is_string()) {
    emit({{"type", "response"},
          {"command", ""},
          {"success", false},
          {"error", "command must be an object with a string type"}});
    return;
  }

  const auto type = command["type"].get<std::string>();
  try {
    if (type == "prompt") {
      if (!command.contains("message") || !command["message"].is_string() ||
          command["message"].get<std::string>().empty()) {
        response(command, false, nullptr,
                 "prompt requires a non-empty string message");
        return;
      }
      start_prompt(command, command["message"].get<std::string>());
    } else if (type == "steer" || type == "follow_up") {
      if (!command.contains("message") || !command["message"].is_string()) {
        response(command, false, nullptr, type + " requires a string message");
        return;
      }
      core::UserMessage message;
      message.content.emplace_back(
          core::TextContent{.text = command["message"].get<std::string>()});
      if (type == "steer")
        session_.agent().steer({std::move(message)});
      else
        session_.agent().follow_up({std::move(message)});
      response(command, true);
    } else if (type == "abort") {
      session_.agent().interrupt(rpc_abort_reason(
          command.value("reason", std::string("user"))));
      response(command, true,
               {{"reason", command.value("reason", std::string("user"))}});
    } else if (type == "spawn_agent") {
      if (!task_manager_) {
        response(command, false, nullptr, "agent task manager is unavailable");
        return;
      }
      core::SpawnAgentRequest request;
      request.parent_id = command.value("parent_id", std::string{});
      request.task_name = command.value("task_name", std::string{});
      request.prompt = command.value("message", std::string{});
      if (command.contains("context")) {
        const auto &context = command.at("context");
        const auto mode = rpc_context_mode(
            context.value("mode", std::string("none")));
        if (!mode)
          throw core::AgentTaskError(core::AgentTaskErrorKind::invalid_context,
                                     "invalid context inheritance mode");
        request.context.mode = *mode;
        if (context.contains("through"))
          request.context.through = context.at("through").get<std::size_t>();
        if (context.contains("recent_count"))
          request.context.recent_count =
              context.at("recent_count").get<std::size_t>();
      }
      if (command.contains("model") && !command.at("model").is_null())
        request.model_spec = command.at("model").get<std::string>();
      if (command.contains("system_prompt") &&
          !command.at("system_prompt").is_null())
        request.system_prompt = command.at("system_prompt").get<std::string>();
      if (command.contains("tools"))
        request.requested_tools =
            command.at("tools").get<std::vector<std::string>>();
      response(command, true,
               task_snapshot_json(task_manager_->spawn(request)));
    } else if (type == "list_agents") {
      if (!task_manager_) {
        response(command, false, nullptr, "agent task manager is unavailable");
        return;
      }
      const auto prefix = command.value("path_prefix", std::string{});
      nlohmann::json agents = nlohmann::json::array();
      for (const auto &snapshot : task_manager_->list(
               prefix.empty() ? std::optional<std::string_view>{}
                              : std::optional<std::string_view>{prefix}))
        agents.push_back(task_snapshot_json(snapshot));
      response(command, true, {{"agents", std::move(agents)}});
    } else if (type == "send_agent" || type == "follow_up_agent") {
      if (!task_manager_) {
        response(command, false, nullptr, "agent task manager is unavailable");
        return;
      }
      const auto target = command.value("target", std::string{});
      auto message = rpc_message(command);
      const auto snapshot = type == "send_agent"
                                ? task_manager_->send_message(target, message)
                                : task_manager_->follow_up(target, message);
      response(command, true, task_snapshot_json(snapshot));
    } else if (type == "wait_agents") {
      start_wait(command);
    } else if (type == "interrupt_agent") {
      if (!task_manager_) {
        response(command, false, nullptr, "agent task manager is unavailable");
        return;
      }
      const auto reason = rpc_interrupt_reason(
          command.value("reason", std::string("user")));
      if (!reason)
        throw core::AgentTaskError(core::AgentTaskErrorKind::invalid_context,
                                   "invalid interrupt reason");
      response(command, true,
               task_snapshot_json(task_manager_->interrupt(
                   command.value("target", std::string{}), *reason)));
    } else if (type == "close_agent") {
      if (!task_manager_) {
        response(command, false, nullptr, "agent task manager is unavailable");
        return;
      }
      response(command, true,
               task_snapshot_json(task_manager_->close(
                   command.value("target", std::string{}))));
    } else if (type == "get_state") {
      const auto messages = session_.agent().state().messages();
      nlohmann::json tools = nlohmann::json::array();
      for (const auto &tool : session_.agent().state().tools())
        tools.push_back(tool->name());
      nlohmann::json state = nlohmann::json::object();
      state["model"] = as_json(session_.agent().state().model());
      state["thinking_level"] = core::thinking_level_to_string(
          session_.agent().state().thinking_level());
      state["is_streaming"] = session_.agent().is_streaming();
      if (const auto &id = session_.active_session_id())
        state["session_id"] = *id;
      else
        state["session_id"] = nullptr;
      if (const auto name = session_.agent().state().session_name())
        state["session_name"] = *name;
      else
        state["session_name"] = nullptr;
      state["message_count"] = messages.size();
      state["tools"] = std::move(tools);
      response(command, true, std::move(state));
    } else if (type == "get_messages") {
      nlohmann::json messages = nlohmann::json::array();
      for (const auto &message : session_.agent().state().messages())
        messages.push_back(as_json(message));
      response(command, true, {{"messages", std::move(messages)}});
    } else if (type == "set_thinking_level") {
      const auto level = command.value("level", std::string{});
      const auto parsed = parse_thinking(level);
      if (!parsed) {
        response(command, false, nullptr, "invalid thinking level");
        return;
      }
      session_.agent().state().set_thinking_level(*parsed);
      response(command, true);
    } else if (type == "new_session" || type == "switch_session" ||
               type == "fork" || type == "set_session_name") {
      if (run_active_ || session_.agent().is_streaming()) {
        response(command, false, nullptr,
                 "session changes require an idle agent");
        return;
      }
      if (type == "new_session") {
        const auto model = session_.agent().state().model();
        core::SessionHeader header{.id = core::generate_session_id(),
                                   .created =
                                       std::chrono::system_clock::to_time_t(
                                           std::chrono::system_clock::now()),
                                   .model = model.id,
                                   .provider = model.provider};
        const auto id = session_.create_session(std::move(header));
        response(command, true, {{"session_id", id}});
      } else if (type == "switch_session") {
        const auto id = command.value("session_id", std::string{});
        if (id.empty() || !session_.activate_session(id)) {
          response(command, false, nullptr, "session not found");
          return;
        }
        if (const auto record = session_.load_session(id);
            record && record->header.name)
          session_.agent().state().set_session_name(*record->header.name);
        response(command, true, {{"session_id", id}});
      } else if (type == "fork") {
        const auto parent = session_.active_session_id();
        if (!parent) {
          response(command, false, nullptr, "no active session to fork");
          return;
        }
        const auto model = session_.agent().state().model();
        core::SessionHeader header{
            .id = core::generate_session_id(),
            .parent_id = parent,
            .parent_offset = session_.agent().state().messages().size(),
            .created = std::chrono::system_clock::to_time_t(
                std::chrono::system_clock::now()),
            .model = model.id,
            .provider = model.provider};
        const auto id = session_.fork_session(std::move(header));
        response(command, true,
                 {{"session_id", id}, {"parent_session_id", *parent}});
      } else {
        const auto name = command.value("name", std::string{});
        const auto id = session_.active_session_id();
        if (name.empty() || !id || session_.session_store() == nullptr) {
          response(
              command, false, nullptr,
              "set_session_name requires an active session and non-empty name");
          return;
        }
        session_.session_store()->set_name(*id, name);
        session_.agent().state().set_session_name(name);
        response(command, true);
      }
    } else {
      response(command, false, nullptr, "unknown command: " + type);
    }
  } catch (const std::exception &e) {
    response(command, false, nullptr, e.what());
  }
}

void RpcMode::stop() {
  session_.agent().interrupt(core::TurnAbortReason::shutdown);
  if (task_manager_)
    task_manager_->shutdown();
  wait_for_idle();
  std::vector<std::jthread> waits;
  {
    std::scoped_lock lock(wait_mutex_);
    waits.swap(wait_threads_);
  }
  for (auto &wait : waits)
    wait.request_stop();
}

void RpcMode::wait_for_idle() {
  if (run_thread_.joinable())
    run_thread_.join();
}

int run_rpc_mode(core::AgentSession &session, std::istream &input,
                 std::ostream &output, core::AgentTaskManager *task_manager) {
  RpcMode mode(session, [&output](const nlohmann::json &event) {
    output << event.dump() << '\n' << std::flush;
  }, task_manager);
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty())
      continue;
    const auto command = nlohmann::json::parse(line, nullptr, false);
    if (command.is_discarded()) {
      output
          << R"({"type":"response","command":"","success":false,"error":"invalid JSON"})"
          << '\n'
          << std::flush;
      continue;
    }
    mode.handle(command);
  }
  return 0;
}

} // namespace pi::cli
