#include "core/providers/faux_control.h"

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace pi::core;

namespace {

int failed = 0;

#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value)) {                                                            \
      ++failed;                                                                \
      std::cerr << "FAIL: " << #value << " at " << __LINE__ << "\n";           \
    }                                                                          \
  } while (false)

Model make_model() {
  return Model{
      .id = "remote-model", .api = "faux-control", .provider = "faux-control"};
}

} // namespace

int main() {
  {
    ScriptedToolRegistry registry;
    std::string error;
    auto script = compile_round({"type", "round"}, registry, error);
    CHECK(!script.has_value());
    CHECK(!error.empty());
  }

  {
    ScriptedToolRegistry registry;
    std::string error;
    auto script =
        compile_round({{"type", "round"},
                       {"stop_reason", "end_turn"},
                       {"content", {{{"type", "text"}, {"text", "hello"}}}}},
                      registry, error);
    CHECK(script.has_value());
    CHECK(error.empty());
    CHECK(script->events.size() == 5);
    CHECK(
        std::holds_alternative<AssistantMessageStartEvent>(script->events[0]));
    CHECK(std::holds_alternative<AssistantMessageTextStartEvent>(
        script->events[1]));
    CHECK(std::holds_alternative<AssistantMessageTextDeltaEvent>(
        script->events[2]));
    CHECK(std::holds_alternative<AssistantMessageTextEndEvent>(
        script->events[3]));
    const auto *done =
        std::get_if<AssistantMessageDoneEvent>(&script->events[4]);
    CHECK(done != nullptr);
    CHECK(done != nullptr && done->reason == StopReason::stop);
    CHECK(done != nullptr && done->message.content.size() == 1);
    CHECK(done != nullptr &&
          std::get<TextContent>(done->message.content[0]).text == "hello");
  }

  {
    ScriptedToolRegistry registry;
    std::string error;
    auto script =
        compile_round({{"type", "round"},
                       {"stop_reason", "tool_calls"},
                       {"content",
                        {{{"type", "tool_call"},
                          {"call_id", "call-1"},
                          {"name", "bash"},
                          {"args", {{"command", "echo hi"}}},
                          {"updates",
                           {{{"after_ms", 2}, {"partial", "running"}},
                            {{"after_ms", 4}, {"partial", "done"}}}},
                          {"result", {{"content", "hi"}, {"is_error", false}}},
                          {"finish_after_ms", 5}}}}},
                      registry, error);
    CHECK(script.has_value());
    const auto *tool_start =
        std::get_if<AssistantMessageToolCallStartEvent>(&script->events[1]);
    const auto *tool_delta =
        std::get_if<AssistantMessageToolCallDeltaEvent>(&script->events[2]);
    const auto *tool_end =
        std::get_if<AssistantMessageToolCallEndEvent>(&script->events[3]);
    CHECK(tool_start != nullptr && tool_start->partial.content.empty());
    CHECK(tool_delta != nullptr && tool_delta->partial.content.size() == 1);
    CHECK(tool_delta != nullptr &&
          tool_delta->delta == R"({"command":"echo hi"})");
    CHECK(tool_end != nullptr && tool_end->tool_call.id == "call-1");
    const auto *done =
        std::get_if<AssistantMessageDoneEvent>(&script->events.back());
    CHECK(done != nullptr && done->reason == StopReason::tool_use);
    CHECK(done != nullptr && done->message.content.size() == 1);
    CHECK(done != nullptr &&
          std::get<ToolCall>(done->message.content[0]).id == "call-1");
    auto behavior = registry.take_behavior("call-1");
    CHECK(behavior.has_value());
    CHECK(behavior->updates.size() == 2);
    CHECK(behavior->updates[1].partial == "done");
    CHECK(behavior->result_content == "hi");
    CHECK(!behavior->is_error);
    CHECK(behavior->finish_after_ms == 5);
    CHECK(!registry.take_behavior("call-1").has_value());
  }

  {
    ScriptedToolRegistry registry;
    std::string error;
    auto missing_id = compile_round(
        {{"type", "round"},
         {"stop_reason", "tool_calls"},
         {"content", {{{"type", "tool_call"}, {"name", "bash"}}}}},
        registry, error);
    CHECK(!missing_id.has_value());
    CHECK(!error.empty());
    auto unknown_type = compile_round({{"type", "not-round"},
                                       {"stop_reason", "end_turn"},
                                       {"content", nlohmann::json::array()}},
                                      registry, error);
    CHECK(!unknown_type.has_value());
    CHECK(!error.empty());
  }

  {
    {
      auto registry = std::make_shared<ScriptedToolRegistry>();
      ScriptedTool tool("scripted", registry);
      CHECK(tool.name() == "scripted");
      CHECK(tool.schema().serialize() == R"({"type":"object"})");
      CHECK(tool.schema().to_definition().empty());
      ToolArguments arguments = nlohmann::json::object({{"value", 1}});
      CHECK(!tool.schema().validate_arguments(arguments).has_value());

      std::vector<std::string> partials;
      registry->register_behavior(
          "tool-1",
          ScriptedToolBehavior{.updates = {{4, "first"}, {8, "second"}},
                               .result_content = "done",
                               .finish_after_ms = 18});
      const auto started = std::chrono::steady_clock::now();
      auto result = tool.execute(
          "{}",
          ToolExecutionContext{
              .call_id = "tool-1",
              .on_update = [&partials](std::shared_ptr<ToolResult> update) {
                partials.push_back(update->content());
              }});
      const auto elapsed = std::chrono::steady_clock::now() - started;
      CHECK(result != nullptr && !result->is_error());
      CHECK(result != nullptr && result->content() == "done");
      const std::vector<std::string> expected_partials{"first", "second"};
      CHECK(partials == expected_partials);
      CHECK(elapsed >= std::chrono::milliseconds(12));
      CHECK(elapsed < std::chrono::milliseconds(500));

      registry->register_behavior(
          "error",
          ScriptedToolBehavior{.result_content = "failed", .is_error = true});
      auto error_result =
          tool.execute("{}", ToolExecutionContext{.call_id = "error"});
      CHECK(error_result != nullptr && error_result->is_error());
      CHECK(error_result != nullptr && error_result->content() == "failed");

      auto missing_result =
          tool.execute("{}", ToolExecutionContext{.call_id = "missing"});
      CHECK(missing_result != nullptr && missing_result->is_error());
      CHECK(missing_result != nullptr &&
            missing_result->content().find("missing") != std::string::npos);

      registry->register_behavior("cancel",
                                  ScriptedToolBehavior{.finish_after_ms = 250});
      const auto cancel_started = std::chrono::steady_clock::now();
      std::stop_source stop_source;
      std::shared_ptr<ToolResult> cancelled;
      std::thread worker([&] {
        cancelled = tool.execute(
            "{}", ToolExecutionContext{.call_id = "cancel",
                                       .stop_token = stop_source.get_token()});
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      stop_source.request_stop();
      worker.join();
      const auto cancel_elapsed =
          std::chrono::steady_clock::now() - cancel_started;
      CHECK(cancel_elapsed < std::chrono::milliseconds(200));
      CHECK(cancelled != nullptr && cancelled->is_error());
      CHECK(cancelled != nullptr && cancelled->content() == "cancelled");
    }

    RemoteFauxClient client;
    ScriptedToolRegistry registry;
    std::string error;
    auto script =
        compile_round({{"type", "round"},
                       {"stop_reason", "end_turn"},
                       {"content", {{{"type", "text"}, {"text", "queued"}}}}},
                      registry, error);
    CHECK(script.has_value());
    client.push_round(std::move(*script));
    std::vector<AssistantMessageEvent> events;
    auto result = client.stream(
        make_model(), AgentContext{}, StreamOptions{},
        [&events](const AssistantMessageEvent &event) {
          events.push_back(event);
        },
        std::stop_token{});
    CHECK(result != nullptr);
    CHECK(result->model == "remote-model");
    CHECK(result->stop_reason == StopReason::stop);
    CHECK(events.size() == 5);

    std::stop_source stop_source;
    auto delayed_script =
        compile_round({{"type", "round"},
                       {"stop_reason", "end_turn"},
                       {"delay_between_ms", 500},
                       {"content", {{{"type", "text"}, {"text", "delayed"}}}}},
                      registry, error);
    CHECK(delayed_script.has_value());
    client.push_round(std::move(*delayed_script));
    std::stop_source delayed_stop;
    std::thread delayed_canceller([&delayed_stop] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      delayed_stop.request_stop();
    });
    const auto delayed_started = std::chrono::steady_clock::now();
    auto delayed_result =
        client.stream(make_model(), AgentContext{}, StreamOptions{}, {},
                      delayed_stop.get_token());
    const auto delayed_elapsed =
        std::chrono::steady_clock::now() - delayed_started;
    delayed_canceller.join();
    CHECK(delayed_result != nullptr);
    CHECK(delayed_result->stop_reason == StopReason::error);
    CHECK(delayed_elapsed < std::chrono::milliseconds(200));
    std::thread canceller([&stop_source] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      stop_source.request_stop();
    });
    const auto started = std::chrono::steady_clock::now();
    auto cancelled =
        client.stream(make_model(), AgentContext{}, StreamOptions{}, {},
                      stop_source.get_token());
    const auto elapsed = std::chrono::steady_clock::now() - started;
    canceller.join();
    CHECK(cancelled != nullptr);
    CHECK(cancelled->stop_reason == StopReason::error);
    CHECK(cancelled->error_message.has_value());
    CHECK(*cancelled->error_message == "No more faux-control rounds queued");
    CHECK(elapsed < std::chrono::seconds(1));
  }

  std::cout << "faux control: " << (failed == 0 ? "passed" : "failed") << "\n";
  return failed == 0 ? 0 : 1;
}
