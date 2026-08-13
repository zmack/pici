#include "core/agent_task.h"
#include "core/llm_client.h"
#include "core/providers/faux.h"
#include "core/session/agent_session.h"

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

using namespace pi::core;

namespace {
int failed = 0;
#define CHECK(value)                                                            \
  do {                                                                          \
    if (!(value)) {                                                             \
      ++failed;                                                                 \
      std::cerr << "FAIL: " << #value << " at " << __LINE__ << "\n";          \
    }                                                                             \
  } while (false)

FauxClient::Script response(std::string text) {
  AssistantMessage partial;
  partial.model = "task-model";
  AssistantMessage final_message;
  final_message.model = "task-model";
  final_message.stop_reason = StopReason::stop;
  final_message.content.emplace_back(TextContent{.text = text});
  return FauxClient::Script{
      .events = {AssistantMessageEvent{AssistantMessageStartEvent{partial}},
                 AssistantMessageEvent{
                     AssistantMessageTextDeltaEvent{0, text, partial}},
                 AssistantMessageEvent{AssistantMessageDoneEvent{
                     StopReason::stop, final_message}}}};
}


class InterruptClient : public LLMClient {
public:
  explicit InterruptClient(std::shared_ptr<std::atomic<int>> calls)
      : calls_(std::move(calls)) {}

  std::shared_ptr<AssistantMessage> stream(
      const Model &model, const AgentContext &, const StreamOptions &,
      AssistantEventCallback, std::stop_token stop_token) override {
    const auto call = calls_->fetch_add(1) + 1;
    auto message = std::make_shared<AssistantMessage>();
    message->api = model.api;
    message->provider = model.provider;
    message->model = model.id;
    if (call == 1) {
      while (!stop_token.stop_requested())
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      message->stop_reason = StopReason::aborted;
      message->error_message = "aborted";
    } else {
      message->stop_reason = StopReason::stop;
      message->content.emplace_back(TextContent{.text = "reused"});
    }
    return message;
  }

  std::string_view provider_name() const override { return "interrupt-test"; }
  std::string_view api_id() const override { return "interrupt-test"; }

private:
  std::shared_ptr<std::atomic<int>> calls_;
};

AgentTaskSnapshot wait_terminal(AgentTaskManager &manager,
                                AgentTaskSnapshot current) {
  for (;;) {
    if (current.status == AgentTaskStatusKind::completed ||
        current.status == AgentTaskStatusKind::errored ||
        current.status == AgentTaskStatusKind::interrupted)
      return current;
    AgentWaitRequest request;
    request.targets = {current.id};
    request.after_generation = current.generation;
    request.timeout = std::chrono::seconds(2);
    auto update = manager.wait(request);
    CHECK(!update.caller_interrupted);
    CHECK(!update.timed_out);
    for (const auto &changed : update.changed)
      if (changed.id == current.id)
        current = changed;
  }
}
} // namespace

int main() {
  auto faux = std::make_shared<FauxClient>(
      std::vector{response("child result"), response("follow-up result"),
                  response("third result")});
  LLMClientRegistry::instance().register_client("faux", [faux] {
    return faux;
  });

  Model model;
  model.id = "task-model";
  model.api = "faux";
  model.provider = "faux";
  Agent::Options options;
  options.model = model;
  AgentSession root({.agent_options = options});
  AgentTaskManager manager(root, options);

  auto child = manager.spawn({.task_name = "review", .prompt = "Review"});
  CHECK(!child.id.empty());
  CHECK(child.task_path == "/root/review");
  auto completed = wait_terminal(manager, child);
  CHECK(completed.status == AgentTaskStatusKind::completed);
  CHECK(completed.result.has_value());
  CHECK(completed.result->text == "child result");

  auto queued = manager.send_message(child.id, UserMessage{
                                              .content = {TextContent{.text = "context"}}});
  CHECK(queued.queued_message_count == 1);
  auto follow = manager.follow_up(child.id, UserMessage{
                                             .content = {TextContent{.text = "Summarize"}}});
  auto second = wait_terminal(manager, follow);
  CHECK(second.status == AgentTaskStatusKind::completed);
  CHECK(second.result.has_value());
  CHECK(second.result->text == "follow-up result");

  int accepted = 0;
  UserMessage mailbox_message;
  mailbox_message.content.emplace_back(TextContent{.text = "mailbox"});
  AgentMessageEnvelope mailbox_envelope{
      .message = Message{std::move(mailbox_message)},
      .on_accepted = [&accepted] { ++accepted; },
      .source = AgentMessageSource::mailbox};
  auto reactivated =
      manager.steer_envelopes(child.id, {std::move(mailbox_envelope)});
  auto third = wait_terminal(manager, reactivated);
  CHECK(third.status == AgentTaskStatusKind::completed);
  CHECK(third.result && third.result->text == "third result");
  CHECK(accepted == 1);

  const auto all = manager.list();
  CHECK(all.size() == 2);
  CHECK(all.front().task_path == "/root");
  CHECK(manager.resident_tasks() == 1);

  const auto closed = manager.close(child.id);
  CHECK(closed.status == AgentTaskStatusKind::shutdown);
  CHECK(!manager.get(child.id).has_value());
  CHECK(manager.resident_tasks() == 0);
  manager.shutdown();

  auto interrupt_calls = std::make_shared<std::atomic<int>>(0);
  LLMClientRegistry::instance().register_client("interrupt-test",
                                                   [interrupt_calls] {
                                                     return std::make_shared<InterruptClient>(interrupt_calls);
                                                   });
  Model interrupt_model;
  interrupt_model.id = "interrupt-model";
  interrupt_model.api = "interrupt-test";
  interrupt_model.provider = "interrupt-test";
  Agent::Options interrupt_options;
  interrupt_options.model = interrupt_model;
  AgentSession interrupt_root({.agent_options = interrupt_options});
  AgentTaskManager interrupt_manager(interrupt_root, interrupt_options);
  auto interrupted = interrupt_manager.spawn(
      {.task_name = "slow", .prompt = "wait"});
  while (interrupt_manager.get(interrupted.id)->status !=
         AgentTaskStatusKind::running)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  auto immediate = interrupt_manager.interrupt(
      interrupted.id, AgentInterruptReason::timeout);
  CHECK(immediate.status == AgentTaskStatusKind::running);
  auto settled = wait_terminal(interrupt_manager, immediate);
  CHECK(settled.status == AgentTaskStatusKind::interrupted);
  CHECK(settled.result.has_value());
  CHECK(settled.result->stop_reason == StopReason::aborted);
  auto reused = interrupt_manager.follow_up(
      interrupted.id, UserMessage{.content = {TextContent{.text = "retry"}}});
  auto reused_result = wait_terminal(interrupt_manager, reused);
  CHECK(reused_result.status == AgentTaskStatusKind::completed);
  CHECK(reused_result.result && reused_result.result->text == "reused");
  interrupt_manager.shutdown();

  if (failed != 0)
    return 1;
  std::cout << "agent tasks: all passed\n";
  return 0;
}
