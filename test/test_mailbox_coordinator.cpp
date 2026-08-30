#include "core/agent_task.h"
#include "core/mailbox/mailbox_coordinator.h"

#include "core/providers/faux.h"
#include "core/session/agent_session.h"
#include "support/gtest_helpers.h"
#include <gtest/gtest.h>

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

using namespace pi::core;

MailboxErrorCode error_code(auto &&call) {
  try {
    call();
  } catch (const MailboxError &error) {
    return error.code();
  }
  return MailboxErrorCode::internal;
}

// Lexicon "Claim, delivery, acknowledgement" state diagram:
//   enqueue -> available -> claim (leased) -> route -> accept -> acknowledge
//                            ^                   |
//                            +---- redeliver ----+  if lease/ack fails
//   acknowledged -> retain -> cleanup
// This file exercises enqueue/claim/route/accept/acknowledge and the
// lease-expiry redelivery path below (search "accept_delivery = false" and
// "idle-redelivery"). It does not exercise the retain -> cleanup tail
// (retention-window expiry / MailboxCoordinator's cleanup_interval); that
// remains an open Phase 1 gap — see plans/session-runtime-migration.md
// Phase 1 item 3.

class MailboxCoordinatorTest : public testing::Test {
protected:
  pi::test::TemporaryDirectory directory{"pici-coordinator"};
  TimestampMs now{1'000};

  void SetUp() override {
    std::filesystem::permissions(directory.path(),
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
  }

  MailboxCoordinatorOptions options(std::string process_id = "process-1",
                                    std::string root_agent_id = "root-agent") {
    MailboxCoordinatorOptions result;
    result.store.path = directory.path() / "mailbox.sqlite3";
    result.store.workspace_id = "workspace";
    result.store.workspace_path = directory.path().string();
    result.store.claim_lease_ms = 100;
    result.store.clock = [&] { return now; };
    result.store.id_generator = [&] { return "generated"; };
    result.process_id = std::move(process_id);
    result.root_agent_id = std::move(root_agent_id);
    result.provider = "test";
    result.model_id = "model-a";
    result.heartbeat_interval = std::chrono::hours(1);
    result.stale_after = std::chrono::hours(2);
    result.cleanup_interval = std::chrono::hours(2);
    return result;
  }
};

TEST_F(MailboxCoordinatorTest, TracksRootAndSubagentLifecycle) {
  MailboxCoordinator coordinator(options());
  bool observed = false;
  auto callbacks = fan_out_agent_task_callbacks({
      [](const AgentTaskEvent &) { throw std::runtime_error("observer"); },
      [&](const AgentTaskEvent &) { observed = true; },
  });
  callbacks(AgentTaskClosedEvent{.id = "ignored"});
  EXPECT_TRUE(observed);

  EXPECT_TRUE(std::filesystem::exists(directory.path() / "mailbox.sqlite3"));
  const auto root_a = coordinator.activate_root("session-a", "first");
  EXPECT_TRUE(coordinator.status().root_active);
  EXPECT_EQ(coordinator.status().session_id.value(), std::string("session-a"));
  coordinator.set_session_name("renamed");
  EXPECT_EQ(coordinator.status().session_name.value(), std::string("renamed"));
  EXPECT_EQ(coordinator.self(root_a).session_name.value(),
            std::string("renamed"));
  EXPECT_EQ(coordinator.self(root_a).agent_id, root_a.agent_id);
  EXPECT_EQ(coordinator.self(root_a).session_id, root_a.session_id);
  EXPECT_EQ(coordinator.self(root_a).lease_expires_at_ms,
            now + std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::hours(2))
                      .count());
  coordinator.set_root_running(true);
  coordinator.set_model("test", "model-b");
  EXPECT_TRUE(coordinator.status().root_running);

  coordinator.register_subagent("agent_1", "/root/child", "root");
  coordinator.observe_task_event(
      AgentTaskStatusChangedEvent{.id = "agent_1",
                                  .previous = AgentTaskStatusKind::pending_init,
                                  .current = AgentTaskStatusKind::completed});
  auto children = coordinator.store().list_agents(AgentQuery{
      .session_id = "session-a", .include_closed = true, .now_ms = now});
  EXPECT_EQ(children.size(), std::size_t{2});
  EXPECT_TRUE(std::ranges::any_of(children, [](const auto &child) {
    return child.task_id == "agent_1" && child.status == "completed";
  }));
  const auto first_child = std::ranges::find_if(
      children, [](const auto &child) { return child.task_id == "agent_1"; });
  ASSERT_NE(first_child, children.end());
  const auto first_endpoint = first_child->agent_id;
  coordinator.register_subagent("agent_2", "/root/child/nested", "agent_1");
  coordinator.register_subagent("agent_3", "/root/child/nested/deeper",
                                "agent_2");
  coordinator.observe_task_event(AgentTaskClosedEvent{.id = "agent_1"});
  EXPECT_TRUE(coordinator.store()
                  .list_agents(AgentQuery{.agent_id = first_endpoint,
                                          .include_closed = true,
                                          .now_ms = now})
                  .front()
                  .closed_at_ms.has_value());

  const auto root_b = coordinator.activate_root("session-b", "second");
  EXPECT_EQ(coordinator.status().session_id.value(), std::string("session-b"));
  EXPECT_EQ(coordinator.store()
                .list_agents(AgentQuery{.include_closed = true, .now_ms = now})
                .size(),
            std::size_t{5});
  const auto old_agents = coordinator.store().list_agents(AgentQuery{
      .session_id = "session-a", .include_closed = true, .now_ms = now});
  EXPECT_EQ(old_agents.size(), std::size_t{4});
  EXPECT_TRUE(std::ranges::all_of(old_agents, [](const auto &agent) {
    return agent.closed_at_ms.has_value();
  }));
  const auto current_agents = coordinator.store().list_agents(AgentQuery{
      .session_id = "session-b", .include_closed = true, .now_ms = now});
  EXPECT_EQ(current_agents.size(), std::size_t{1});
  EXPECT_TRUE(!current_agents.front().closed_at_ms.has_value());
}

TEST_F(MailboxCoordinatorTest, DeliversAndAcknowledgesRootMessages) {
  MailboxCoordinator coordinator(options());
  const auto root_b = coordinator.activate_root("session-b", "second");
  std::vector<AgentInput> delivered;
  std::vector<AgentInput> root_queue;
  bool accept_delivery = true;
  auto delivery = std::make_shared<MailboxDeliveryTargets>();
  delivery->root = [&](std::vector<AgentInput> messages) {
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
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "steer-1",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::steer,
      .body = MailboxPayload{.text = "inspect this"},
      .created_at_ms = now});
  coordinator.pump_inbox();
  EXPECT_TRUE(delivered.empty());
  coordinator.set_root_running(true);
  coordinator.pump_inbox();
  EXPECT_EQ(delivered.size(), std::size_t{1});
  EXPECT_TRUE(std::holds_alternative<UserMessage>(delivered.front().message));
  EXPECT_TRUE(
      std::get<TextContent>(
          std::get<UserMessage>(delivered.front().message).content.front())
          .text == "[pici mailbox message]\n"
                   "message_id=steer-1\n"
                   "kind=steer\n"
                   "sender_session_id=sender-session\n"
                   "sender_agent_id=sender-agent\n"
                   "recipient_session_id=session-b\n"
                   "recipient_agent_id=(session root)\n"
                   "\ninspect this");
  EXPECT_TRUE(delivered.front().presentation.source ==
              InputProvenance::Source::mailbox);
  EXPECT_EQ(delivered.front().presentation.message_id.value(), "steer-1");
  EXPECT_EQ(delivered.front().presentation.message_kind.value(), "steer");
  EXPECT_EQ(delivered.front().presentation.sender_agent_id.value(),
            "sender-agent");
  EXPECT_EQ(delivered.front().presentation.sender_session_id.value(),
            "sender-session");
  delivered.front().on_accepted();
  root_queue.clear();
  EXPECT_TRUE(coordinator.store()
                  .inspect(InboxQuery{.session_id = "session-b",
                                      .workspace_id = "workspace",
                                      .now_ms = now})
                  .empty());

  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "note-1",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::note,
      .body = MailboxPayload{.text = "do not steer"},
      .created_at_ms = now});
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "reply-1",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::reply,
      .body = MailboxPayload{.text = "do not steer"},
      .created_at_ms = now});
  coordinator.pump_inbox();
  EXPECT_EQ(delivered.size(), std::size_t{1});
  EXPECT_EQ(coordinator.store()
                .inspect(InboxQuery{.session_id = "session-b",
                                    .workspace_id = "workspace",
                                    .now_ms = now})
                .size(),
            std::size_t{2});
  for (const auto entry_id : {"steer-1", "note-1", "reply-1"}) {
    EXPECT_TRUE(error_code([&] {
                  static_cast<void>(coordinator.reply(
                      root_b, entry_id, MailboxPayload{.text = "invalid"}));
                }) == MailboxErrorCode::invalid_message);
  }

  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "steer-2",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::steer,
      .body = MailboxPayload{.text = "retry me"},
      .created_at_ms = now});
  accept_delivery = false;
  coordinator.pump_inbox();
  EXPECT_EQ(delivered.size(), std::size_t{1});
  now += 101;
  accept_delivery = true;
  coordinator.pump_inbox();
  EXPECT_EQ(delivered.size(), std::size_t{2});
  delivered.back().on_accepted();
}

TEST_F(MailboxCoordinatorTest, RoutesMessagesByExactEndpoint) {
  MailboxCoordinator coordinator(options());
  coordinator.activate_root("session-a", "first");
  coordinator.register_subagent("agent_1", "/root/child", "root");
  coordinator.register_subagent("agent_2", "/root/child/nested", "agent_1");
  coordinator.register_subagent("agent_3", "/root/child/nested/deeper",
                                "agent_2");
  const auto root_b = coordinator.activate_root("session-b", "second");
  std::vector<AgentInput> delivered;
  std::vector<AgentInput> root_queue;
  bool accept_delivery = true;
  auto delivery = std::make_shared<MailboxDeliveryTargets>();
  delivery->root = [&](std::vector<AgentInput> messages) {
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
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "steer-1",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::steer,
      .body = MailboxPayload{.text = "inspect this"},
      .created_at_ms = now});
  coordinator.pump_inbox();
  EXPECT_TRUE(delivered.empty());
  coordinator.set_root_running(true);
  coordinator.pump_inbox();
  EXPECT_EQ(delivered.size(), std::size_t{1});
  EXPECT_TRUE(std::holds_alternative<UserMessage>(delivered.front().message));
  EXPECT_TRUE(
      std::get<TextContent>(
          std::get<UserMessage>(delivered.front().message).content.front())
          .text == "[pici mailbox message]\n"
                   "message_id=steer-1\n"
                   "kind=steer\n"
                   "sender_session_id=sender-session\n"
                   "sender_agent_id=sender-agent\n"
                   "recipient_session_id=session-b\n"
                   "recipient_agent_id=(session root)\n"
                   "\ninspect this");
  EXPECT_TRUE(delivered.front().presentation.source ==
              InputProvenance::Source::mailbox);
  EXPECT_EQ(delivered.front().presentation.message_id.value(), "steer-1");
  EXPECT_EQ(delivered.front().presentation.message_kind.value(), "steer");
  EXPECT_EQ(delivered.front().presentation.sender_agent_id.value(),
            "sender-agent");
  EXPECT_EQ(delivered.front().presentation.sender_session_id.value(),
            "sender-session");
  delivered.front().on_accepted();
  root_queue.clear();
  EXPECT_TRUE(coordinator.store()
                  .inspect(InboxQuery{.session_id = "session-b",
                                      .workspace_id = "workspace",
                                      .now_ms = now})
                  .empty());

  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "note-1",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::note,
      .body = MailboxPayload{.text = "do not steer"},
      .created_at_ms = now});
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "reply-1",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::reply,
      .body = MailboxPayload{.text = "do not steer"},
      .created_at_ms = now});
  coordinator.pump_inbox();
  EXPECT_EQ(delivered.size(), std::size_t{1});
  EXPECT_EQ(coordinator.store()
                .inspect(InboxQuery{.session_id = "session-b",
                                    .workspace_id = "workspace",
                                    .now_ms = now})
                .size(),
            std::size_t{2});
  for (const auto entry_id : {"steer-1", "note-1", "reply-1"}) {
    EXPECT_TRUE(error_code([&] {
                  static_cast<void>(coordinator.reply(
                      root_b, entry_id, MailboxPayload{.text = "invalid"}));
                }) == MailboxErrorCode::invalid_message);
  }

  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "steer-2",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::steer,
      .body = MailboxPayload{.text = "retry me"},
      .created_at_ms = now});
  accept_delivery = false;
  coordinator.pump_inbox();
  EXPECT_EQ(delivered.size(), std::size_t{1});
  now += 101;
  accept_delivery = true;
  coordinator.pump_inbox();
  EXPECT_EQ(delivered.size(), std::size_t{2});
  delivered.back().on_accepted();

  std::vector<AgentInput> subagent_delivered;
  delivery->subagent = [&](std::string, std::string task_id,
                           std::vector<AgentInput> messages) {
    EXPECT_EQ(task_id, std::string("agent_4"));
    for (auto &message : messages)
      subagent_delivered.push_back(std::move(message));
    return true;
  };
  coordinator.set_root_running(false);
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "root-only-session",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::steer,
      .body = MailboxPayload{.text = "root only"},
      .created_at_ms = now});
  const auto child4 =
      coordinator.register_subagent("agent_4", "/root/child-4", "root");
  coordinator.observe_task_event(
      AgentTaskStatusChangedEvent{.id = "agent_4",
                                  .previous = AgentTaskStatusKind::pending_init,
                                  .current = AgentTaskStatusKind::completed});
  EXPECT_TRUE(coordinator
                  .inspect(child4, InboxQuery{.entry_id = "root-only-session",
                                              .limit = 1})
                  .empty());
  bool child_reply_rejected = false;
  try {
    static_cast<void>(coordinator.reply(child4, "root-only-session",
                                        MailboxPayload{.text = "wrong child"}));
  } catch (const MailboxError &error) {
    child_reply_rejected = error.code() == MailboxErrorCode::not_found;
  }
  EXPECT_TRUE(child_reply_rejected);
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "exact-child-note",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.agent_id = child4.agent_id},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::note,
      .body = MailboxPayload{.text = "child only"},
      .created_at_ms = now});
  EXPECT_TRUE(coordinator
                  .inspect(root_b, InboxQuery{.entry_id = "exact-child-note",
                                              .limit = 1})
                  .empty());
  const auto child5 =
      coordinator.register_subagent("agent_5", "/root/child-5", "root");
  EXPECT_TRUE(coordinator
                  .inspect(child5, InboxQuery{.entry_id = "exact-child-note",
                                              .limit = 1})
                  .empty());
  EXPECT_EQ(coordinator
                .inspect(child4,
                         InboxQuery{.entry_id = "exact-child-note", .limit = 1})
                .size(),
            std::size_t{1});
  bool root_reply_rejected = false;
  try {
    static_cast<void>(coordinator.reply(root_b, "exact-child-note",
                                        MailboxPayload{.text = "wrong root"}));
  } catch (const MailboxError &error) {
    root_reply_rejected = error.code() == MailboxErrorCode::not_found;
  }
  EXPECT_TRUE(root_reply_rejected);
  bool sibling_reply_rejected = false;
  try {
    static_cast<void>(coordinator.reply(
        child5, "exact-child-note", MailboxPayload{.text = "wrong sibling"}));
  } catch (const MailboxError &error) {
    sibling_reply_rejected = error.code() == MailboxErrorCode::not_found;
  }
  EXPECT_TRUE(sibling_reply_rejected);
  coordinator.pump_inbox();
  EXPECT_TRUE(subagent_delivered.empty());
  coordinator.set_root_running(true);
  coordinator.pump_inbox();
  EXPECT_EQ(delivered.size(), std::size_t{3});
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
  ASSERT_NE(endpoint, child_agents.end());
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "sub-steer-1",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.agent_id = endpoint->agent_id},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::steer,
      .body = MailboxPayload{.text = "reactivate child"},
      .created_at_ms = now});
  coordinator.pump_inbox();
  EXPECT_EQ(subagent_delivered.size(), std::size_t{1});
  subagent_delivered.front().on_accepted();
  EXPECT_TRUE(coordinator.store()
                  .inspect(InboxQuery{.session_id = "session-b",
                                      .workspace_id = "workspace",
                                      .agent_id = endpoint->agent_id,
                                      .agent_kind = "subagent",
                                      .entry_id = "sub-steer-1",
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
  ASSERT_NE(second_child, old_agents_after.end());
  EXPECT_TRUE(coordinator.store()
                  .list_agents(AgentQuery{.agent_id = second_child->agent_id,
                                          .include_closed = true,
                                          .now_ms = now})
                  .front()
                  .status == "closed");
}

TEST_F(MailboxCoordinatorTest, ClosesTaskEndpointsOnShutdown) {
  MailboxCoordinator coordinator(options());
  const auto root_b = coordinator.activate_root("session-b", "second");
  Model task_model;
  task_model.id = "task-model";
  task_model.api = "faux";
  task_model.provider = "faux";
  LLMClientRegistry::instance().register_client("faux", [] {
    return std::make_shared<FauxClient>(std::vector<FauxClient::Script>{});
  });
  Agent::Options task_options;
  task_options.model = task_model;
  SessionRuntime task_root({.agent_options = task_options});
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
  ASSERT_NE(shutdown_endpoint, shutdown_agents.end());
  const auto shutdown_agent = coordinator.store().list_agents(
      AgentQuery{.agent_id = shutdown_endpoint->agent_id,
                 .include_closed = true,
                 .now_ms = now});
  EXPECT_EQ(shutdown_agent.size(), std::size_t{1});
  EXPECT_TRUE(shutdown_agent.front().closed_at_ms.has_value());

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
  EXPECT_EQ(same_endpoints.size(), std::size_t{2});
  EXPECT_TRUE(same_endpoints[0] != same_endpoints[1]);
  second_coordinator.stop();
}

TEST_F(MailboxCoordinatorTest, MaintainsIdleRootWorkAndRedelivery) {
  MailboxCoordinator coordinator(options());
  const auto root_b = coordinator.activate_root("session-b", "second");
  std::vector<AgentInput> delivered;
  std::vector<AgentInput> root_queue;
  auto delivery = std::make_shared<MailboxDeliveryTargets>();
  delivery->root = [&](std::vector<AgentInput> messages) {
    for (auto &message : messages) {
      root_queue.push_back(message);
      delivered.push_back(std::move(message));
    }
    return true;
  };
  delivery->drop_root_queued = [&root_queue] { root_queue.clear(); };
  coordinator.attach_delivery(delivery);
  coordinator.set_root_running(false);
  std::size_t root_wakes = 0;
  delivery->root_wake = [&root_wakes] { ++root_wakes; };
  for (const auto kind : {MailboxEntryKind::note, MailboxEntryKind::reply}) {
    coordinator.store().send(EnqueueMailboxEntryRequest{
        .entry_id = kind == MailboxEntryKind::note ? "idle-note" : "idle-reply",
        .sender_agent_id = "sender-agent",
        .sender_session_id = "sender-session",
        .target = MailboxTarget{.session_id = "session-b"},
        .workspace_id = "workspace",
        .kind = kind,
        .body = MailboxPayload{.text = "inbox only"},
        .created_at_ms = now});
  }
  coordinator.maintenance_tick();
  EXPECT_TRUE(!coordinator.idle_root_work_pending());
  EXPECT_EQ(root_wakes, std::size_t{0});
  const auto probe_root = coordinator.self(root_b).agent_id;
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "a-leased",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::request,
      .body = MailboxPayload{.text = "leased first"},
      .created_at_ms = now});
  const auto leased = coordinator.store().claim(
      ClaimRequest{.session_id = "session-b",
                   .agent_id = probe_root,
                   .workspace_id = "workspace",
                   .kinds = {MailboxEntryKind::request},
                   .limit = 1,
                   .now_ms = now,
                   .lease_ms = 100});
  EXPECT_EQ(leased.messages.size(), std::size_t{1});
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "b-claimable",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::request,
      .body = MailboxPayload{.text = "claimable later"},
      .created_at_ms = now});
  EXPECT_TRUE(coordinator.idle_root_work_pending());
  const auto claimable_probe = coordinator.claim_idle_root_turn(1);
  EXPECT_EQ(claimable_probe.size(), std::size_t{1});
  claimable_probe.front().on_accepted();
  coordinator.set_root_running(false);
  coordinator.store().acknowledge(
      AcknowledgeRequest{.entry_id = leased.messages.front().entry_id,
                         .agent_id = probe_root,
                         .claim_token = *leased.messages.front().claim_token,
                         .workspace_id = "workspace",
                         .now_ms = now});
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "idle-request-1",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::request,
      .body = MailboxPayload{.text = "answer this while idle"},
      .created_at_ms = now});
  for (std::size_t index = 0; index < 16; ++index) {
    const auto entry_id = "idle-steer-" + std::to_string(index);
    coordinator.store().send(EnqueueMailboxEntryRequest{
        .entry_id = entry_id,
        .sender_agent_id = "sender-agent",
        .sender_session_id = "sender-session",
        .target = MailboxTarget{.session_id = "session-b"},
        .workspace_id = "workspace",
        .kind = MailboxEntryKind::steer,
        .body = MailboxPayload{.text = entry_id},
        .created_at_ms = now});
  }
  EXPECT_TRUE(coordinator.idle_root_work_pending());
  coordinator.maintenance_tick();
  EXPECT_TRUE(root_wakes > 0);
  // delivered_at_ms commits at delivery (claim -> AgentInput conversion
  // inside claim_idle_root_turn()), not at the raw store-level claim()
  // check above: "b-claimable"/"idle-request-1" was already claimed once
  // (probe_root) without going through delivery, so it must still read
  // back unset here, and only becomes set once claim_idle_root_turn()
  // below actually converts it.
  {
    const auto before_delivery =
        coordinator.store().inspect(InboxQuery{.session_id = "session-b",
                                               .workspace_id = "workspace",
                                               .entry_id = "idle-request-1",
                                               .include_acknowledged = true,
                                               .now_ms = now});
    EXPECT_EQ(before_delivery.size(), std::size_t{1});
    EXPECT_TRUE(!before_delivery.front().delivered_at_ms.has_value());
  }
  const auto idle_messages = coordinator.claim_idle_root_turn();
  EXPECT_EQ(idle_messages.size(), std::size_t{16});
  {
    const auto after_delivery =
        coordinator.store().inspect(InboxQuery{.session_id = "session-b",
                                               .workspace_id = "workspace",
                                               .entry_id = "idle-request-1",
                                               .include_acknowledged = true,
                                               .now_ms = now});
    EXPECT_EQ(after_delivery.size(), std::size_t{1});
    EXPECT_TRUE(after_delivery.front().delivered_at_ms.has_value());
  }
  EXPECT_TRUE(coordinator.status().root_running);
  EXPECT_TRUE(
      std::get<TextContent>(
          std::get<UserMessage>(idle_messages.front().message).content.front())
          .text.find(
              "Reply with agents_reply(message_id=\"idle-request-1\")") !=
      std::string::npos);
  EXPECT_TRUE(coordinator.claim_idle_root_turn().empty());
  for (const auto &message : idle_messages)
    message.on_accepted();
  EXPECT_TRUE(idle_messages.front().presentation.source ==
              InputProvenance::Source::mailbox);
  EXPECT_TRUE(idle_messages.front().presentation.message_id.has_value());
  EXPECT_TRUE(idle_messages.front().presentation.message_kind.has_value());
  coordinator.set_root_running(false);
  EXPECT_TRUE(coordinator.idle_root_work_pending());
  const auto remaining_idle_messages = coordinator.claim_idle_root_turn();
  EXPECT_EQ(remaining_idle_messages.size(), std::size_t{1});
  remaining_idle_messages.front().on_accepted();
  coordinator.set_root_running(false);
  EXPECT_TRUE(!coordinator.idle_root_work_pending());
  EXPECT_TRUE(coordinator.claim_idle_root_turn().empty());
  MailboxAutonomousTurnBudget budget;
  for (std::size_t index = 0;
       index < MailboxAutonomousTurnBudget::max_consecutive_turns; ++index) {
    EXPECT_TRUE(budget.can_run());
    budget.record();
  }
  EXPECT_TRUE(budget.exhausted());
  budget.reset();
  EXPECT_TRUE(budget.can_run());
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "idle-redelivery",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.session_id = "session-b"},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::request,
      .body = MailboxPayload{.text = "retry after acceptance failure"},
      .created_at_ms = now});
  const auto unaccepted = coordinator.claim_idle_root_turn();
  EXPECT_EQ(unaccepted.size(), std::size_t{1});
  coordinator.set_root_running(false);
  EXPECT_TRUE(!coordinator.idle_root_work_pending());
  now += 101;
  EXPECT_TRUE(coordinator.idle_root_work_pending());
  const auto redelivery = coordinator.claim_idle_root_turn();
  EXPECT_EQ(redelivery.size(), std::size_t{1});
  redelivery.front().on_accepted();
  coordinator.set_root_running(false);
  EXPECT_TRUE(
      coordinator.store()
          .inspect(InboxQuery{.session_id = "session-b",
                              .workspace_id = "workspace",
                              .agent_id = coordinator.self(root_b).agent_id,
                              .agent_kind = "root",
                              .entry_id = "idle-request-1",
                              .now_ms = now})
          .empty());

  coordinator.set_root_running(true);
  coordinator.maintenance_tick();
  const auto delivered_after_tick = delivered.size();
  const auto root_endpoint = coordinator.self(root_b).agent_id;
  now += 1;
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "cadence-1",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.agent_id = root_endpoint},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::steer,
      .body = MailboxPayload{.text = "cadence"},
      .created_at_ms = now});
  coordinator.maintenance_tick();
  EXPECT_EQ(delivered.size(), delivered_after_tick);
  now += 249;
  coordinator.maintenance_tick();
  EXPECT_TRUE(delivered.size() > delivered_after_tick);
  EXPECT_TRUE(!root_queue.empty());
  coordinator.set_root_running(false);
  EXPECT_TRUE(root_queue.empty());
  now += 101;
  coordinator.set_root_running(true);
  coordinator.pump_inbox();
  EXPECT_TRUE(
      std::get<TextContent>(
          std::get<UserMessage>(delivered.back().message).content.front())
          .text.find("cadence-1") != std::string::npos);
  delivered.back().on_accepted();
}

TEST_F(MailboxCoordinatorTest, RejectsStaleRootAcceptance) {
  MailboxCoordinator coordinator(options());
  const auto root_b = coordinator.activate_root("session-b", "second");
  std::vector<AgentInput> delivered;
  auto delivery = std::make_shared<MailboxDeliveryTargets>();
  delivery->root = [&](std::vector<AgentInput> messages) {
    for (auto &message : messages)
      delivered.push_back(std::move(message));
    return true;
  };
  coordinator.attach_delivery(delivery);
  coordinator.set_root_running(true);
  const auto root_endpoint = coordinator.self(root_b).agent_id;
  coordinator.store().send(EnqueueMailboxEntryRequest{
      .entry_id = "stop-race",
      .sender_agent_id = "sender-agent",
      .sender_session_id = "sender-session",
      .target = MailboxTarget{.agent_id = root_endpoint},
      .workspace_id = "workspace",
      .kind = MailboxEntryKind::steer,
      .body = MailboxPayload{.text = "stop"},
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
  EXPECT_TRUE(stale_rejected);
  coordinator.stop();
  acceptance.join();
  coordinator.stop();
  EXPECT_TRUE(!coordinator.status().root_active);
}
