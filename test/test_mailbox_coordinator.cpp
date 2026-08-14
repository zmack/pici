#include "core/agent_task.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/providers/faux.h"
#include "core/session/agent_session.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <unistd.h>

namespace tests {
int passed{0};
int failed{0};

bool check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
  if (condition) {
    ++passed;
    return true;
  }
  ++failed;
  std::cout << "FAIL " << location.file_name() << ":" << location.line()
            << " — " << expression << "\n";
  return false;
}
} // namespace tests

#define CHECK(expression) tests::check((expression), #expression)
#define CHECK_EQ(left, right)                                                  \
  tests::check((left) == (right), #left " == " #right)

using namespace pi::core;

MailboxErrorCode error_code(auto &&call) {
  try {
    call();
  } catch (const MailboxError &error) {
    return error.code();
  }
  return MailboxErrorCode::internal;
}

int main() {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("pici-coordinator-" + std::to_string(::getpid()) + "-" +
                     std::to_string(suffix));
  std::filesystem::create_directories(root);
  std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  TimestampMs now = 1'000;
  const auto options = [&] {
    MailboxCoordinatorOptions result;
    result.store.path = root / "mailbox.sqlite3";
    result.store.workspace_id = "workspace";
    result.store.workspace_path = root.string();
    result.store.claim_lease_ms = 100;
    result.store.clock = [&] { return now; };
    result.store.id_generator = [&] { return "generated"; };
    result.process_id = "process-1";
    result.root_agent_id = "root-agent";
    result.provider = "test";
    result.model_id = "model-a";
    result.heartbeat_interval = std::chrono::hours(1);
    result.stale_after = std::chrono::hours(2);
    result.cleanup_interval = std::chrono::hours(2);
    return result;
  };

  try {
    bool observed = false;
    auto callbacks = fan_out_agent_task_callbacks({
        [](const AgentTaskEvent &) { throw std::runtime_error("observer"); },
        [&](const AgentTaskEvent &) { observed = true; },
    });
    callbacks(AgentTaskClosedEvent{.id = "ignored"});
    CHECK(observed);

    MailboxCoordinator coordinator(options());
    CHECK(std::filesystem::exists(root / "mailbox.sqlite3"));
    const auto root_a = coordinator.activate_root("session-a", "first");
    CHECK(coordinator.status().root_active);
    CHECK_EQ(coordinator.status().session_id.value(), std::string("session-a"));
    CHECK_EQ(coordinator.self(root_a).agent_id, root_a.agent_id);
    CHECK_EQ(coordinator.self(root_a).session_id, root_a.session_id);
    CHECK_EQ(coordinator.self(root_a).lease_expires_at_ms,
             now + std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::hours(2))
                       .count());
    coordinator.set_root_running(true);
    coordinator.set_model("test", "model-b");
    CHECK(coordinator.status().root_running);

    coordinator.register_subagent("agent_1", "/root/child", "root");
    coordinator.observe_task_event(AgentTaskStatusChangedEvent{
        .id = "agent_1",
        .previous = AgentTaskStatusKind::pending_init,
        .current = AgentTaskStatusKind::completed});
    auto children = coordinator.store().list_agents(AgentQuery{
        .session_id = "session-a", .include_closed = true, .now_ms = now});
    CHECK_EQ(children.size(), std::size_t{2});
    CHECK(std::ranges::any_of(children, [](const auto &child) {
      return child.task_id == "agent_1" && child.status == "completed";
    }));
    const auto first_child = std::ranges::find_if(
        children, [](const auto &child) { return child.task_id == "agent_1"; });
    CHECK(first_child != children.end());
    const auto first_endpoint = first_child->agent_id;
    coordinator.register_subagent("agent_2", "/root/child/nested", "agent_1");
    coordinator.register_subagent("agent_3", "/root/child/nested/deeper",
                                  "agent_2");
    coordinator.observe_task_event(AgentTaskClosedEvent{.id = "agent_1"});
    CHECK(coordinator.store()
              .list_agents(AgentQuery{.agent_id = first_endpoint,
                                      .include_closed = true,
                                      .now_ms = now})
              .front()
              .closed_at_ms.has_value());

    const auto root_b = coordinator.activate_root("session-b", "second");
    CHECK_EQ(coordinator.status().session_id.value(), std::string("session-b"));
    CHECK_EQ(coordinator.store()
                 .list_agents(AgentQuery{.include_closed = true, .now_ms = now})
                 .size(),
             std::size_t{5});
    const auto old_agents = coordinator.store().list_agents(AgentQuery{
        .session_id = "session-a", .include_closed = true, .now_ms = now});
    CHECK_EQ(old_agents.size(), std::size_t{4});
    CHECK(std::ranges::all_of(old_agents, [](const auto &agent) {
      return agent.closed_at_ms.has_value();
    }));
    const auto current_agents = coordinator.store().list_agents(AgentQuery{
        .session_id = "session-b", .include_closed = true, .now_ms = now});
    CHECK_EQ(current_agents.size(), std::size_t{1});
    CHECK(!current_agents.front().closed_at_ms.has_value());

    std::vector<AgentMessageEnvelope> delivered;
    std::vector<AgentMessageEnvelope> root_queue;
    bool accept_delivery = true;
    auto delivery = std::make_shared<MailboxDeliveryTargets>();
    delivery->root = [&](std::vector<AgentMessageEnvelope> messages) {
      if (!accept_delivery)
        return false;
      for (auto &message : messages) {
        root_queue.push_back(message);
        delivered.push_back(std::move(message));
      }
      return true;
    };
    delivery->drop_root_queued = [&root_queue] { root_queue.clear(); };
    coordinator.attach_delivery(delivery);
    coordinator.store().send(
        SendRequest{.message_id = "steer-1",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.session_id = "session-b"},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::steer,
                    .body = MailboxBody{.text = "inspect this"},
                    .created_at_ms = now});
    coordinator.pump_inbox();
    CHECK(delivered.empty());
    coordinator.set_root_running(true);
    coordinator.pump_inbox();
    CHECK_EQ(delivered.size(), std::size_t{1});
    CHECK(std::holds_alternative<UserMessage>(delivered.front().message));
    CHECK(std::get<TextContent>(
              std::get<UserMessage>(delivered.front().message).content.front())
              .text == "[pici mailbox message]\n"
                       "message_id=steer-1\n"
                       "kind=steer\n"
                       "sender_session_id=sender-session\n"
                       "sender_agent_id=sender-agent\n"
                       "recipient_session_id=session-b\n"
                       "recipient_agent_id=(session root)\n"
                       "\ninspect this");
    delivered.front().on_accepted();
    root_queue.clear();
    CHECK(coordinator.store()
              .inspect(InboxQuery{.session_id = "session-b",
                                  .workspace_id = "workspace",
                                  .now_ms = now})
              .empty());

    coordinator.store().send(
        SendRequest{.message_id = "note-1",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.session_id = "session-b"},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::note,
                    .body = MailboxBody{.text = "do not steer"},
                    .created_at_ms = now});
    coordinator.store().send(
        SendRequest{.message_id = "reply-1",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.session_id = "session-b"},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::reply,
                    .body = MailboxBody{.text = "do not steer"},
                    .created_at_ms = now});
    coordinator.pump_inbox();
    CHECK_EQ(delivered.size(), std::size_t{1});
    CHECK_EQ(coordinator.store()
                 .inspect(InboxQuery{.session_id = "session-b",
                                     .workspace_id = "workspace",
                                     .now_ms = now})
                 .size(),
             std::size_t{2});
    for (const auto message_id : {"steer-1", "note-1", "reply-1"}) {
      CHECK(error_code([&] {
              static_cast<void>(coordinator.reply(
                  root_b, message_id, MailboxBody{.text = "invalid"}));
            }) == MailboxErrorCode::invalid_message);
    }

    coordinator.store().send(
        SendRequest{.message_id = "steer-2",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.session_id = "session-b"},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::steer,
                    .body = MailboxBody{.text = "retry me"},
                    .created_at_ms = now});
    accept_delivery = false;
    coordinator.pump_inbox();
    CHECK_EQ(delivered.size(), std::size_t{1});
    now += 101;
    accept_delivery = true;
    coordinator.pump_inbox();
    CHECK_EQ(delivered.size(), std::size_t{2});
    delivered.back().on_accepted();

    std::vector<AgentMessageEnvelope> subagent_delivered;
    delivery->subagent = [&](std::string, std::string task_id,
                             std::vector<AgentMessageEnvelope> messages) {
      CHECK_EQ(task_id, std::string("agent_4"));
      for (auto &message : messages)
        subagent_delivered.push_back(std::move(message));
      return true;
    };
    coordinator.set_root_running(false);
    coordinator.store().send(
        SendRequest{.message_id = "root-only-session",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.session_id = "session-b"},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::steer,
                    .body = MailboxBody{.text = "root only"},
                    .created_at_ms = now});
    const auto child4 =
        coordinator.register_subagent("agent_4", "/root/child-4", "root");
    coordinator.observe_task_event(AgentTaskStatusChangedEvent{
        .id = "agent_4",
        .previous = AgentTaskStatusKind::pending_init,
        .current = AgentTaskStatusKind::completed});
    CHECK(coordinator
              .inspect(child4, InboxQuery{.message_id = "root-only-session",
                                          .limit = 1})
              .empty());
    bool child_reply_rejected = false;
    try {
      static_cast<void>(coordinator.reply(child4, "root-only-session",
                                          MailboxBody{.text = "wrong child"}));
    } catch (const MailboxError &error) {
      child_reply_rejected = error.code() == MailboxErrorCode::not_found;
    }
    CHECK(child_reply_rejected);
    coordinator.store().send(
        SendRequest{.message_id = "exact-child-note",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.agent_id = child4.agent_id},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::note,
                    .body = MailboxBody{.text = "child only"},
                    .created_at_ms = now});
    CHECK(coordinator
              .inspect(root_b,
                       InboxQuery{.message_id = "exact-child-note", .limit = 1})
              .empty());
    const auto child5 =
        coordinator.register_subagent("agent_5", "/root/child-5", "root");
    CHECK(coordinator
              .inspect(child5,
                       InboxQuery{.message_id = "exact-child-note", .limit = 1})
              .empty());
    CHECK_EQ(coordinator
                 .inspect(child4, InboxQuery{.message_id = "exact-child-note",
                                             .limit = 1})
                 .size(),
             std::size_t{1});
    bool root_reply_rejected = false;
    try {
      static_cast<void>(coordinator.reply(root_b, "exact-child-note",
                                          MailboxBody{.text = "wrong root"}));
    } catch (const MailboxError &error) {
      root_reply_rejected = error.code() == MailboxErrorCode::not_found;
    }
    CHECK(root_reply_rejected);
    bool sibling_reply_rejected = false;
    try {
      static_cast<void>(coordinator.reply(
          child5, "exact-child-note", MailboxBody{.text = "wrong sibling"}));
    } catch (const MailboxError &error) {
      sibling_reply_rejected = error.code() == MailboxErrorCode::not_found;
    }
    CHECK(sibling_reply_rejected);
    coordinator.pump_inbox();
    CHECK(subagent_delivered.empty());
    coordinator.set_root_running(true);
    coordinator.pump_inbox();
    CHECK_EQ(delivered.size(), std::size_t{3});
    delivered.back().on_accepted();
    const auto child_agents =
        coordinator.store().list_agents(AgentQuery{.session_id = "session-b",
                                                   .include_closed = false,
                                                   .limit = 100,
                                                   .now_ms = now});
    const auto endpoint =
        std::ranges::find_if(child_agents, [](const auto &agent) {
          return agent.task_id == "agent_4";
        });
    CHECK(endpoint != child_agents.end());
    coordinator.store().send(
        SendRequest{.message_id = "sub-steer-1",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.agent_id = endpoint->agent_id},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::steer,
                    .body = MailboxBody{.text = "reactivate child"},
                    .created_at_ms = now});
    coordinator.pump_inbox();
    CHECK_EQ(subagent_delivered.size(), std::size_t{1});
    subagent_delivered.front().on_accepted();
    CHECK(coordinator.store()
              .inspect(InboxQuery{.session_id = "session-b",
                                  .workspace_id = "workspace",
                                  .agent_id = endpoint->agent_id,
                                  .agent_kind = "subagent",
                                  .message_id = "sub-steer-1",
                                  .now_ms = now})
              .empty());

    coordinator.observe_task_event(
        AgentTaskStatusChangedEvent{.id = "agent_2",
                                    .previous = AgentTaskStatusKind::running,
                                    .current = AgentTaskStatusKind::completed});
    coordinator.observe_task_event(AgentTaskClosedEvent{.id = "agent_2"});
    const auto old_agents_after = coordinator.store().list_agents(AgentQuery{
        .session_id = "session-a", .include_closed = true, .now_ms = now});
    const auto second_child =
        std::ranges::find_if(old_agents_after, [](const auto &agent) {
          return agent.task_id == "agent_2";
        });
    CHECK(second_child != old_agents_after.end());
    CHECK(coordinator.store()
              .list_agents(AgentQuery{.agent_id = second_child->agent_id,
                                      .include_closed = true,
                                      .now_ms = now})
              .front()
              .status == "closed");

    Model task_model;
    task_model.id = "task-model";
    task_model.api = "faux";
    task_model.provider = "faux";
    LLMClientRegistry::instance().register_client("faux", [] {
      return std::make_shared<FauxClient>(std::vector<FauxClient::Script>{});
    });
    Agent::Options task_options;
    task_options.model = task_model;
    AgentSession task_root({.agent_options = task_options});
    AgentTaskManager task_manager(task_root, task_options,
                                  AgentTaskManager::Limits{},
                                  [&](const AgentTaskEvent &event) {
                                    coordinator.observe_task_event(event);
                                  });
    task_manager.set_endpoint_registration(
        [&](const AgentTaskId &task_id, const std::string &task_path,
            const std::optional<AgentTaskId> &parent_id) {
          return coordinator.register_subagent(task_id, task_path, parent_id);
        },
        [&](const AgentTaskId &task_id) {
          coordinator.unregister_subagent(task_id);
        });
    const auto task =
        task_manager.spawn({.task_name = "shutdown-child", .prompt = "stop"});
    task_manager.shutdown();
    const auto shutdown_agents = coordinator.store().list_agents(AgentQuery{
        .session_id = "session-b", .include_closed = true, .now_ms = now});
    const auto shutdown_endpoint =
        std::ranges::find_if(shutdown_agents, [&](const auto &agent) {
          return agent.task_id == task.id;
        });
    CHECK(shutdown_endpoint != shutdown_agents.end());
    const auto shutdown_agent = coordinator.store().list_agents(
        AgentQuery{.agent_id = shutdown_endpoint->agent_id,
                   .include_closed = true,
                   .now_ms = now});
    CHECK_EQ(shutdown_agent.size(), std::size_t{1});
    CHECK(shutdown_agent.front().closed_at_ms.has_value());

    auto second_options = options();
    second_options.process_id = "process-2";
    second_options.root_agent_id = "root-agent-2";
    MailboxCoordinator second_coordinator(std::move(second_options));
    second_coordinator.activate_root("session-c", "third");
    coordinator.register_subagent("agent_same", "/root/same", "root");
    second_coordinator.register_subagent("agent_same", "/root/same", "root");
    const auto same_agents =
        coordinator.store().list_agents(AgentQuery{.workspace_id = "workspace",
                                                   .include_closed = false,
                                                   .limit = 100,
                                                   .now_ms = now});
    std::vector<std::string> same_endpoints;
    for (const auto &agent : same_agents)
      if (agent.task_id == "agent_same")
        same_endpoints.push_back(agent.agent_id);
    CHECK_EQ(same_endpoints.size(), std::size_t{2});
    CHECK(same_endpoints[0] != same_endpoints[1]);
    second_coordinator.stop();

    coordinator.set_root_running(false);
    std::size_t root_wakes = 0;
    delivery->root_wake = [&root_wakes] { ++root_wakes; };
    for (const auto kind :
         {MailboxMessageKind::note, MailboxMessageKind::reply}) {
      coordinator.store().send(SendRequest{
          .message_id =
              kind == MailboxMessageKind::note ? "idle-note" : "idle-reply",
          .sender_agent_id = "sender-agent",
          .sender_session_id = "sender-session",
          .target = MailboxTarget{.session_id = "session-b"},
          .workspace_id = "workspace",
          .kind = kind,
          .body = MailboxBody{.text = "inbox only"},
          .created_at_ms = now});
    }
    coordinator.maintenance_tick();
    CHECK(!coordinator.idle_root_work_pending());
    CHECK_EQ(root_wakes, std::size_t{0});
    const auto probe_root = coordinator.self(root_b).agent_id;
    coordinator.store().send(
        SendRequest{.message_id = "a-leased",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.session_id = "session-b"},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::request,
                    .body = MailboxBody{.text = "leased first"},
                    .created_at_ms = now});
    const auto leased = coordinator.store().claim(
        ClaimRequest{.session_id = "session-b",
                     .agent_id = probe_root,
                     .workspace_id = "workspace",
                     .kinds = {MailboxMessageKind::request},
                     .limit = 1,
                     .now_ms = now,
                     .lease_ms = 100});
    CHECK_EQ(leased.messages.size(), std::size_t{1});
    coordinator.store().send(
        SendRequest{.message_id = "b-claimable",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.session_id = "session-b"},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::request,
                    .body = MailboxBody{.text = "claimable later"},
                    .created_at_ms = now});
    CHECK(coordinator.idle_root_work_pending());
    const auto claimable_probe = coordinator.claim_idle_root_turn(1);
    CHECK_EQ(claimable_probe.size(), std::size_t{1});
    claimable_probe.front().on_accepted();
    coordinator.set_root_running(false);
    coordinator.store().acknowledge(
        AcknowledgeRequest{.message_id = leased.messages.front().message_id,
                           .agent_id = probe_root,
                           .claim_token = *leased.messages.front().claim_token,
                           .workspace_id = "workspace",
                           .now_ms = now});
    coordinator.store().send(
        SendRequest{.message_id = "idle-request-1",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.session_id = "session-b"},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::request,
                    .body = MailboxBody{.text = "answer this while idle"},
                    .created_at_ms = now});
    for (std::size_t index = 0; index < 16; ++index) {
      const auto message_id = "idle-steer-" + std::to_string(index);
      coordinator.store().send(
          SendRequest{.message_id = message_id,
                      .sender_agent_id = "sender-agent",
                      .sender_session_id = "sender-session",
                      .target = MailboxTarget{.session_id = "session-b"},
                      .workspace_id = "workspace",
                      .kind = MailboxMessageKind::steer,
                      .body = MailboxBody{.text = message_id},
                      .created_at_ms = now});
    }
    CHECK(coordinator.idle_root_work_pending());
    coordinator.maintenance_tick();
    CHECK(root_wakes > 0);
    const auto idle_messages = coordinator.claim_idle_root_turn();
    CHECK_EQ(idle_messages.size(), std::size_t{16});
    CHECK(coordinator.status().root_running);
    CHECK(std::get<TextContent>(
              std::get<UserMessage>(idle_messages.front().message)
                  .content.front())
              .text.find(
                  "Reply with agents_reply(message_id=\"idle-request-1\")") !=
          std::string::npos);
    CHECK(coordinator.claim_idle_root_turn().empty());
    for (const auto &message : idle_messages)
      message.on_accepted();
    coordinator.set_root_running(false);
    CHECK(coordinator.idle_root_work_pending());
    const auto remaining_idle_messages = coordinator.claim_idle_root_turn();
    CHECK_EQ(remaining_idle_messages.size(), std::size_t{1});
    remaining_idle_messages.front().on_accepted();
    coordinator.set_root_running(false);
    CHECK(!coordinator.idle_root_work_pending());
    CHECK(coordinator.claim_idle_root_turn().empty());
    MailboxAutonomousTurnBudget budget;
    for (std::size_t index = 0;
         index < MailboxAutonomousTurnBudget::max_consecutive_turns; ++index) {
      CHECK(budget.can_run());
      budget.record();
    }
    CHECK(budget.exhausted());
    budget.reset();
    CHECK(budget.can_run());
    coordinator.store().send(SendRequest{
        .message_id = "idle-redelivery",
        .sender_agent_id = "sender-agent",
        .sender_session_id = "sender-session",
        .target = MailboxTarget{.session_id = "session-b"},
        .workspace_id = "workspace",
        .kind = MailboxMessageKind::request,
        .body = MailboxBody{.text = "retry after acceptance failure"},
        .created_at_ms = now});
    const auto unaccepted = coordinator.claim_idle_root_turn();
    CHECK_EQ(unaccepted.size(), std::size_t{1});
    coordinator.set_root_running(false);
    CHECK(!coordinator.idle_root_work_pending());
    now += 101;
    CHECK(coordinator.idle_root_work_pending());
    const auto redelivery = coordinator.claim_idle_root_turn();
    CHECK_EQ(redelivery.size(), std::size_t{1});
    redelivery.front().on_accepted();
    coordinator.set_root_running(false);
    CHECK(coordinator.store()
              .inspect(InboxQuery{.session_id = "session-b",
                                  .workspace_id = "workspace",
                                  .agent_id = coordinator.self(root_b).agent_id,
                                  .agent_kind = "root",
                                  .message_id = "idle-request-1",
                                  .now_ms = now})
              .empty());

    coordinator.set_root_running(true);
    coordinator.maintenance_tick();
    const auto delivered_after_tick = delivered.size();
    const auto root_endpoint = coordinator.self(root_b).agent_id;
    now += 1;
    coordinator.store().send(
        SendRequest{.message_id = "cadence-1",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.agent_id = root_endpoint},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::steer,
                    .body = MailboxBody{.text = "cadence"},
                    .created_at_ms = now});
    coordinator.maintenance_tick();
    CHECK_EQ(delivered.size(), delivered_after_tick);
    now += 249;
    coordinator.maintenance_tick();
    CHECK(delivered.size() > delivered_after_tick);
    CHECK(!root_queue.empty());
    coordinator.set_root_running(false);
    CHECK(root_queue.empty());
    now += 101;
    coordinator.set_root_running(true);
    coordinator.pump_inbox();
    CHECK(std::get<TextContent>(
              std::get<UserMessage>(delivered.back().message).content.front())
              .text.find("cadence-1") != std::string::npos);
    delivered.back().on_accepted();
    coordinator.store().send(
        SendRequest{.message_id = "stop-race",
                    .sender_agent_id = "sender-agent",
                    .sender_session_id = "sender-session",
                    .target = MailboxTarget{.agent_id = root_endpoint},
                    .workspace_id = "workspace",
                    .kind = MailboxMessageKind::steer,
                    .body = MailboxBody{.text = "stop"},
                    .created_at_ms = now});
    coordinator.pump_inbox();
    auto stop_race = delivered.back();
    std::thread acceptance([&stop_race] { stop_race.on_accepted(); });
    now += std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::hours(2))
               .count() +
           1;
    bool stale_rejected = false;
    try {
      static_cast<void>(coordinator.self(root_b));
    } catch (const MailboxError &error) {
      stale_rejected = error.code() == MailboxErrorCode::permission_denied;
    }
    CHECK(stale_rejected);
    coordinator.stop();
    acceptance.join();
    coordinator.stop();
    CHECK(!coordinator.status().root_active);
  } catch (const std::exception &error) {
    std::cout << "unexpected exception: " << error.what() << "\n";
    ++tests::failed;
  }

  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::cout << "Tests: " << tests::passed << " passed, " << tests::failed
            << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
