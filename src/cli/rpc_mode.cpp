#include "cli/rpc_mode.h"

#include "core/event_types.h"
#include "core/event_json.h"
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
#include <variant>
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

RpcMode::RpcMode(core::AgentSession &session, Output output)
    : session_(session), output_(std::move(output)) {}

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
      session_.agent().abort();
      response(command, true);
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
  session_.agent().abort();
  wait_for_idle();
}

void RpcMode::wait_for_idle() {
  if (run_thread_.joinable())
    run_thread_.join();
}

int run_rpc_mode(core::AgentSession &session, std::istream &input,
                 std::ostream &output) {
  RpcMode mode(session, [&output](const nlohmann::json &event) {
    output << event.dump() << '\n' << std::flush;
  });
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
