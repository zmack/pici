#include "core/providers/faux_control.h"

#include <gtest/gtest.h>

#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace pi::core;

namespace {

Model make_model() {
  return Model{
      .id = "remote-model", .api = "faux-control", .provider = "faux-control"};
}

} // namespace

TEST(FauxControl, ScriptCompilationAndStreaming) {
  {
    ScriptedToolRegistry registry;
    std::string error;
    auto script = compile_round({"type", "round"}, registry, error);
    EXPECT_TRUE(!script.has_value());
    EXPECT_TRUE(!error.empty());
  }

  {
    ScriptedToolRegistry registry;
    std::string error;
    auto script =
        compile_round({{"type", "round"},
                       {"stop_reason", "end_turn"},
                       {"content", {{{"type", "text"}, {"text", "hello"}}}}},
                      registry, error);
    EXPECT_TRUE(script.has_value());
    EXPECT_TRUE(error.empty());
    EXPECT_TRUE(script->events.size() == 5);
    EXPECT_TRUE(
        std::holds_alternative<AssistantMessageStartEvent>(script->events[0]));
    EXPECT_TRUE(std::holds_alternative<AssistantMessageTextStartEvent>(
        script->events[1]));
    EXPECT_TRUE(std::holds_alternative<AssistantMessageTextDeltaEvent>(
        script->events[2]));
    EXPECT_TRUE(std::holds_alternative<AssistantMessageTextEndEvent>(
        script->events[3]));
    const auto *done =
        std::get_if<AssistantMessageDoneEvent>(&script->events[4]);
    EXPECT_TRUE(done != nullptr);
    EXPECT_TRUE(done != nullptr && done->reason == StopReason::stop);
    EXPECT_TRUE(done != nullptr && done->message.content.size() == 1);
    EXPECT_TRUE(done != nullptr &&
                std::get<TextContent>(done->message.content[0]).text ==
                    "hello");
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
                          {"presentation",
                           {{"kind", "mailbox_reply_queued"},
                            {"request_message_id", "request-1"},
                            {"recipient_session_id", "session-b"},
                            {"recipient_agent_id", "agent-b"},
                            {"text", "hello"}}},
                          {"finish_after_ms", 5}}}}},
                      registry, error);
    EXPECT_TRUE(script.has_value());
    const auto *tool_start =
        std::get_if<AssistantMessageToolCallStartEvent>(&script->events[1]);
    const auto *tool_delta =
        std::get_if<AssistantMessageToolCallDeltaEvent>(&script->events[2]);
    const auto *tool_end =
        std::get_if<AssistantMessageToolCallEndEvent>(&script->events[3]);
    EXPECT_TRUE(tool_start != nullptr && tool_start->partial.content.empty());
    EXPECT_TRUE(tool_delta != nullptr &&
                tool_delta->partial.content.size() == 1);
    EXPECT_TRUE(tool_delta != nullptr &&
                tool_delta->delta == R"({"command":"echo hi"})");
    EXPECT_TRUE(tool_end != nullptr && tool_end->tool_call.id == "call-1");
    const auto *done =
        std::get_if<AssistantMessageDoneEvent>(&script->events.back());
    EXPECT_TRUE(done != nullptr && done->reason == StopReason::tool_use);
    EXPECT_TRUE(done != nullptr && done->message.content.size() == 1);
    EXPECT_TRUE(done != nullptr &&
                std::get<ToolCall>(done->message.content[0]).id == "call-1");
    auto behavior = registry.take_behavior("call-1");
    EXPECT_TRUE(behavior.has_value());
    EXPECT_TRUE(behavior->updates.size() == 2);
    EXPECT_TRUE(behavior->updates[1].partial == "done");
    EXPECT_TRUE(behavior->result_content == "hi");
    EXPECT_TRUE(!behavior->is_error);
    EXPECT_TRUE(behavior->presentation_notice.has_value());
    EXPECT_TRUE(behavior->presentation_notice->reply_text == "hello");
    EXPECT_TRUE(behavior->finish_after_ms == 5);
    EXPECT_TRUE(!registry.take_behavior("call-1").has_value());
  }

  {
    ScriptedToolRegistry registry;
    std::string error;
    auto missing_id = compile_round(
        {{"type", "round"},
         {"stop_reason", "tool_calls"},
         {"content", {{{"type", "tool_call"}, {"name", "bash"}}}}},
        registry, error);
    EXPECT_TRUE(!missing_id.has_value());
    EXPECT_TRUE(!error.empty());
    auto unknown_type = compile_round({{"type", "not-round"},
                                       {"stop_reason", "end_turn"},
                                       {"content", nlohmann::json::array()}},
                                      registry, error);
    EXPECT_TRUE(!unknown_type.has_value());
    EXPECT_TRUE(!error.empty());
  }

  {
    {
      auto registry = std::make_shared<ScriptedToolRegistry>();
      ScriptedTool tool("scripted", registry);
      EXPECT_TRUE(tool.name() == "scripted");
      EXPECT_TRUE(tool.schema().serialize() == R"({"type":"object"})");
      EXPECT_TRUE(tool.schema().to_definition().empty());
      ToolArguments arguments = nlohmann::json::object({{"value", 1}});
      EXPECT_TRUE(!tool.schema().validate_arguments(arguments).has_value());

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
      EXPECT_TRUE(result != nullptr && !result->is_error());
      EXPECT_TRUE(result != nullptr && result->content() == "done");
      const std::vector<std::string> expected_partials{"first", "second"};
      EXPECT_TRUE(partials == expected_partials);
      EXPECT_TRUE(elapsed >= std::chrono::milliseconds(12));
      EXPECT_TRUE(elapsed < std::chrono::milliseconds(500));

      std::optional<MailboxReplyQueuedNotice> notice;
      registry->register_behavior(
          "reply-ok",
          ScriptedToolBehavior{.result_content = "queued",
                               .presentation_notice = MailboxReplyQueuedNotice{
                                   .request_message_id = "request-1",
                                   .recipient_session_id = "session-b",
                                   .recipient_agent_id = "agent-b",
                                   .reply_text = "hello"}});
      auto reply_result = tool.execute(
          "{}", ToolExecutionContext{
                    .call_id = "reply-ok",
                    .on_presentation = [&](ToolPresentationNotice value) {
                      notice =
                          std::get<MailboxReplyQueuedNotice>(std::move(value));
                    }});
      EXPECT_TRUE(reply_result != nullptr && !reply_result->is_error());

      std::optional<MailboxReplyQueuedNotice> error_notice;
      registry->register_behavior(
          "reply-error",
          ScriptedToolBehavior{.result_content = "failed",
                               .is_error = true,
                               .presentation_notice = MailboxReplyQueuedNotice{
                                   .request_message_id = "request-2",
                                   .recipient_session_id = "session-b",
                                   .reply_text = "must not paint"}});
      auto failed_reply = tool.execute(
          "{}", ToolExecutionContext{
                    .call_id = "reply-error",
                    .on_presentation = [&](ToolPresentationNotice value) {
                      error_notice =
                          std::get<MailboxReplyQueuedNotice>(std::move(value));
                    }});
      EXPECT_TRUE(failed_reply != nullptr && failed_reply->is_error());
      EXPECT_TRUE(!error_notice.has_value());
      EXPECT_TRUE(notice.has_value() &&
                  notice->request_message_id == "request-1");
      EXPECT_TRUE(notice.has_value() &&
                  notice->recipient_agent_id == "agent-b");

      registry->register_behavior(
          "error",
          ScriptedToolBehavior{.result_content = "failed", .is_error = true});
      auto error_result =
          tool.execute("{}", ToolExecutionContext{.call_id = "error"});
      EXPECT_TRUE(error_result != nullptr && error_result->is_error());
      EXPECT_TRUE(error_result != nullptr &&
                  error_result->content() == "failed");

      auto missing_result =
          tool.execute("{}", ToolExecutionContext{.call_id = "missing"});
      EXPECT_TRUE(missing_result != nullptr && missing_result->is_error());
      EXPECT_TRUE(missing_result != nullptr &&
                  missing_result->content().find("missing") !=
                      std::string::npos);

      registry->register_behavior(
          "cancel", ScriptedToolBehavior{
                        .finish_after_ms = 250,
                        .presentation_notice = MailboxReplyQueuedNotice{
                            .request_message_id = "request-3",
                            .recipient_session_id = "session-b",
                            .reply_text = "must not paint while cancelled"}});
      const auto cancel_started = std::chrono::steady_clock::now();
      std::stop_source stop_source;
      std::shared_ptr<ToolResult> cancelled;
      std::optional<MailboxReplyQueuedNotice> cancel_notice;
      std::thread worker([&] {
        cancelled = tool.execute(
            "{}", ToolExecutionContext{
                      .call_id = "cancel",
                      .stop_token = stop_source.get_token(),
                      .on_presentation = [&](ToolPresentationNotice value) {
                        cancel_notice = std::get<MailboxReplyQueuedNotice>(
                            std::move(value));
                      }});
      });
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      stop_source.request_stop();
      worker.join();
      const auto cancel_elapsed =
          std::chrono::steady_clock::now() - cancel_started;
      EXPECT_TRUE(cancel_elapsed < std::chrono::milliseconds(200));
      EXPECT_TRUE(cancelled != nullptr && cancelled->is_error());
      EXPECT_TRUE(cancelled != nullptr && cancelled->content() == "cancelled");
      EXPECT_TRUE(!cancel_notice.has_value());
    }

    RemoteFauxClient client;
    ScriptedToolRegistry registry;
    std::string error;
    auto script =
        compile_round({{"type", "round"},
                       {"stop_reason", "end_turn"},
                       {"content", {{{"type", "text"}, {"text", "queued"}}}}},
                      registry, error);
    EXPECT_TRUE(script.has_value());
    client.push_round(std::move(*script));
    std::vector<AssistantMessageEvent> events;
    auto result = client.stream(
        make_model(), AgentContext{}, StreamOptions{},
        [&events](const AssistantMessageEvent &event) {
          events.push_back(event);
        },
        std::stop_token{});
    EXPECT_TRUE(result != nullptr);
    EXPECT_TRUE(result->model == "remote-model");
    EXPECT_TRUE(result->stop_reason == StopReason::stop);
    EXPECT_TRUE(events.size() == 5);

    std::stop_source stop_source;
    auto delayed_script =
        compile_round({{"type", "round"},
                       {"stop_reason", "end_turn"},
                       {"delay_between_ms", 500},
                       {"content", {{{"type", "text"}, {"text", "delayed"}}}}},
                      registry, error);
    EXPECT_TRUE(delayed_script.has_value());
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
    EXPECT_TRUE(delayed_result != nullptr);
    EXPECT_TRUE(delayed_result->stop_reason == StopReason::error);
    EXPECT_TRUE(delayed_elapsed < std::chrono::milliseconds(200));
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
    EXPECT_TRUE(cancelled != nullptr);
    EXPECT_TRUE(cancelled->stop_reason == StopReason::error);
    EXPECT_TRUE(cancelled->error_message.has_value());
    EXPECT_TRUE(*cancelled->error_message ==
                "No more faux-control rounds queued");
    EXPECT_TRUE(elapsed < std::chrono::seconds(1));
  }
}
