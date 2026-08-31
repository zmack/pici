#include "cli/mailbox_runtime.h"

#include <gtest/gtest.h>

#include "cli/config.h"
#include "core/agent.h"
#include "core/agent_state.h"
#include "core/agent_task.h"
#include "core/llm_client.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/mailbox/mailbox_types.h"
#include "core/message_types.h"
#include "core/session/session_runtime.h"
#include "core/session/session_id.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <unistd.h>

namespace {

using namespace pi;

// Blocks a spawned child task's single turn until release() is set, so a
// test can deterministically control exactly when the task's
// AgentTaskStatusChangedEvent(running -> completed) fires relative to
// mailbox_runtime's own lifecycle.
class BlockingTaskClient : public core::LLMClient {
public:
  BlockingTaskClient(std::shared_ptr<std::atomic<bool>> entered,
                     std::shared_ptr<std::atomic<bool>> release)
      : entered_(std::move(entered)), release_(std::move(release)) {}

  std::shared_ptr<core::AssistantMessage>
  stream(const core::Model &model, const core::AgentContext &,
         const core::StreamOptions &, core::AssistantEventCallback,
         std::stop_token stop_tok) override {
    entered_->store(true);
    while (!release_->load() && !stop_tok.stop_requested())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto message = std::make_shared<core::AssistantMessage>();
    message->api = model.api;
    message->provider = model.provider;
    message->model = model.id;
    message->stop_reason = core::StopReason::stop;
    message->content.emplace_back(core::TextContent{.text = "done"});
    return message;
  }

  std::string_view provider_name() const override { return "test"; }
  std::string_view api_id() const override {
    return "mailbox-runtime-blocking-test";
  }

private:
  std::shared_ptr<std::atomic<bool>> entered_;
  std::shared_ptr<std::atomic<bool>> release_;
};

core::AgentTaskSnapshot wait_for_task_status(core::AgentTaskManager &manager,
                                             const core::AgentTaskId &id,
                                             core::AgentTaskStatusKind status) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    auto snapshot = manager.get(id);
    if (snapshot && snapshot->status == status)
      return *snapshot;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return manager.get(id).value_or(core::AgentTaskSnapshot{});
}

// Lexicon "Destruction follows inverse dependency order. Child tasks close
// before mailbox observers disconnect..." — src/core/session/mailbox_runtime.h
// documents the required local-declaration order (SessionRuntime, then
// MailboxRuntime, then AgentTaskManager) that produces this at scope exit.
// This test demonstrates *why* the order matters: a child task's
// AgentTaskStatusChangedEvent only reaches the mailbox store while
// core::MailboxRuntime's Observer is still attached (task_event_callback() ->
// Observer::observe() -> MailboxCoordinator::observe_task_event()). Detach
// the observer before the event fires (the wrong order) and the mid-lifecycle
// status update is silently dropped — the mailbox record goes stale even
// though the task itself really did complete. See
// plans/session-runtime-migration.md Phase 1 item 4.
//
// Note: a task's *terminal* close (closed_at_ms) is also recorded through a
// second, independent path — MailboxCoordinator::unregister_subagent(),
// reached via AgentTaskManager's endpoint-unregistration callback rather
// than the Observer — so closed_at_ms alone would not distinguish correct
// from incorrect teardown order. The intermediate "completed" status update
// is Observer-exclusive, which is what this test checks.
TEST(MailboxRuntime, TeardownOrder) {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();

  auto make_coordinator = [&](std::string_view label) {
    const auto root =
        std::filesystem::temp_directory_path() /
        ("pici-mailbox-runtime-teardown-" + std::string(label) + "-" +
         std::to_string(::getpid()) + "-" + std::to_string(suffix));
    std::filesystem::create_directories(root);
    std::filesystem::permissions(root, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
    core::MailboxCoordinatorOptions options;
    options.store.path = root / "mailbox.sqlite3";
    options.store.workspace_id = "workspace";
    options.store.workspace_path = root.string();
    options.store.clock = [] {
      return std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::system_clock::now().time_since_epoch())
          .count();
    };
    options.store.id_generator = [] { return core::generate_session_id(); };
    options.process_id = "process-" + std::string(label);
    options.root_agent_id = "root-agent-" + std::string(label);
    options.provider = "test";
    options.model_id = "model-a";
    options.heartbeat_interval = std::chrono::hours(1);
    options.stale_after = std::chrono::hours(2);
    options.cleanup_interval = std::chrono::hours(2);
    return std::make_shared<core::MailboxCoordinator>(std::move(options));
  };

  core::Agent::Options agent_opts;
  agent_opts.model.id = "child-model";
  agent_opts.model.api = "mailbox-runtime-blocking-test";
  agent_opts.model.provider = "test";

  // Scenario A: documented order — mailbox_runtime stays connected while
  // the child task transitions to "completed".
  std::string status_when_connected;
  {
    auto entered = std::make_shared<std::atomic<bool>>(false);
    auto release = std::make_shared<std::atomic<bool>>(false);
    core::LLMClientRegistry::instance().register_client(
        "mailbox-runtime-blocking-test", [entered, release] {
          return std::make_shared<BlockingTaskClient>(entered, release);
        });

    auto coordinator = make_coordinator("a");
    coordinator->activate_root("session-a", "teardown-a");
    core::SessionRuntime root({.agent_options = agent_opts});
    core::MailboxRuntime mailbox_runtime(coordinator);
    auto tasks = std::make_shared<core::AgentTaskManager>(
        root, agent_opts, core::AgentTaskManager::Limits{},
        mailbox_runtime.task_event_callback());
    mailbox_runtime.connect(root, tasks, [] {});

    const auto child = tasks->spawn({.task_name = "child-a", .prompt = "go"});
    const auto entered_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered->load() &&
           std::chrono::steady_clock::now() < entered_deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(entered->load());

    release->store(true);
    const auto completed = wait_for_task_status(
        *tasks, child.id, core::AgentTaskStatusKind::completed);
    EXPECT_TRUE(completed.status == core::AgentTaskStatusKind::completed);

    // Query the mailbox store before shutdown/close: the child's terminal
    // close would independently set status="closed" and mask what we're
    // testing, so this snapshot must happen while the task is merely
    // "completed" but still resident.
    const auto children = coordinator->store().list_agents(
        core::AgentQuery{.session_id = "session-a",
                         .include_stale = true,
                         .include_closed = true});
    const auto record = std::ranges::find_if(
        children, [&](const auto &agent) { return agent.task_id == child.id; });
    EXPECT_TRUE(record != children.end());
    if (record != children.end())
      status_when_connected = record->status;

    tasks->shutdown();
    mailbox_runtime.shutdown();
  }
  EXPECT_EQ(status_when_connected, std::string("completed"));

  // Scenario B: wrong order — mailbox_runtime is torn down (its Observer
  // detached) *before* the child task transitions to "completed", the way
  // it would if AgentTaskManager were declared/destroyed after MailboxRuntime
  // instead of before it.
  std::string status_when_disconnected;
  {
    auto entered = std::make_shared<std::atomic<bool>>(false);
    auto release = std::make_shared<std::atomic<bool>>(false);
    core::LLMClientRegistry::instance().register_client(
        "mailbox-runtime-blocking-test", [entered, release] {
          return std::make_shared<BlockingTaskClient>(entered, release);
        });

    auto coordinator = make_coordinator("b");
    coordinator->activate_root("session-b", "teardown-b");
    core::SessionRuntime root({.agent_options = agent_opts});
    core::MailboxRuntime mailbox_runtime(coordinator);
    auto tasks = std::make_shared<core::AgentTaskManager>(
        root, agent_opts, core::AgentTaskManager::Limits{},
        mailbox_runtime.task_event_callback());
    mailbox_runtime.connect(root, tasks, [] {});

    const auto child = tasks->spawn({.task_name = "child-b", .prompt = "go"});
    const auto entered_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!entered->load() &&
           std::chrono::steady_clock::now() < entered_deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    EXPECT_TRUE(entered->load());

    // Detach the observer (and stop the coordinator's root/maintenance
    // machinery) *before* releasing the blocked turn — the reordering bug
    // this test exists to catch. `coordinator` is kept alive by our own
    // shared_ptr so the mailbox store stays queryable afterward.
    mailbox_runtime.shutdown();

    release->store(true);
    const auto completed = wait_for_task_status(
        *tasks, child.id, core::AgentTaskStatusKind::completed);
    EXPECT_TRUE(completed.status == core::AgentTaskStatusKind::completed);

    const auto children = coordinator->store().list_agents(
        core::AgentQuery{.session_id = "session-b",
                         .include_stale = true,
                         .include_closed = true});
    const auto record = std::ranges::find_if(
        children, [&](const auto &agent) { return agent.task_id == child.id; });
    EXPECT_TRUE(record != children.end());
    if (record != children.end())
      status_when_disconnected = record->status;

    tasks->shutdown();
  }
  // The task really did complete (proven above via the task manager's own
  // state), but the mailbox record never heard about it: the wrong teardown
  // order silently drops durable observability, exactly the failure mode
  // the declared-order comment in cli/mailbox_runtime.h exists to prevent.
  EXPECT_TRUE(status_when_disconnected != "completed");
}

} // namespace

TEST(MailboxRuntime, LaunchAndDisabledRuntime) {
  using namespace pi;

  const auto workspace =
      std::filesystem::temp_directory_path() / "pici-mailbox-runtime-workspace";
  auto config = std::make_shared<cli::Config>();
  config->mailbox.path = "/config/mailbox.sqlite3";
  config->mailbox.scope = "global";
  config->mailbox.heartbeat_interval_ms = 1234;
  config->mailbox.stale_after_ms = 5678;
  config->mailbox.poll_interval_ms = 90;
  config->mailbox.claim_lease_ms = 4321;
  config->mailbox.retention_days = 12;

  cli::Args args;
  args.config_document = config;
  args.mailbox_path = "/cli/mailbox.sqlite3";
  core::Model model;
  model.provider = "provider-a";
  model.id = "model-a";

  const auto launch =
      cli::resolve_mailbox_launch_options(args, model, workspace);
  EXPECT_EQ(launch.resolved_path,
            std::filesystem::path("/cli/mailbox.sqlite3"));
  EXPECT_EQ(launch.coordinator.store.path, launch.resolved_path);
  EXPECT_EQ(launch.coordinator.store.workspace_path,
            workspace.lexically_normal().string());
  EXPECT_TRUE(launch.coordinator.store.scope == core::MailboxScope::global);
  EXPECT_EQ(launch.coordinator.store.claim_lease_ms, std::int64_t{4321});
  EXPECT_EQ(launch.coordinator.store.retention_days, std::int64_t{12});
  EXPECT_EQ(launch.coordinator.heartbeat_interval,
            std::chrono::milliseconds(1234));
  EXPECT_EQ(launch.coordinator.stale_after, std::chrono::milliseconds(5678));
  EXPECT_EQ(launch.coordinator.poll_interval, std::chrono::milliseconds(90));
  EXPECT_EQ(launch.coordinator.provider, std::string("provider-a"));
  EXPECT_EQ(launch.coordinator.model_id, std::string("model-a"));
  EXPECT_TRUE(!launch.coordinator.process_id.empty());
  EXPECT_TRUE(!launch.coordinator.root_agent_id.empty());
  EXPECT_TRUE(!launch.coordinator.store.workspace_id.empty());

  core::MailboxRuntime disabled;
  EXPECT_TRUE(!disabled.enabled());
  EXPECT_TRUE(!disabled.task_event_callback());
  EXPECT_TRUE(disabled.claim_idle_root_turn().empty());
}
