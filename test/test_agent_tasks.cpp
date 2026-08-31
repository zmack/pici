#include "core/agent_task.h"
#include "core/builtin_tools.h"
#include "core/llm_client.h"
#include "core/lua_tool.h"
#include "core/providers/faux.h"
#include "core/session/session_runtime.h"
#include "support/gtest_helpers.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <iostream>
#include <memory>
#include <mutex>
#include <random>
#include <ranges>
#include <string>
#include <system_error>
#include <thread>
#include <variant>

using namespace pi::core;

namespace {

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

  std::shared_ptr<AssistantMessage>
  stream(const Model &model, const AgentContext &, const StreamOptions &,
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

class IdentityClient : public LLMClient {
public:
  explicit IdentityClient(
      std::shared_ptr<std::optional<AgentRuntimeIdentity>> identity,
      std::shared_ptr<std::vector<std::string>> tools = {})
      : identity_(std::move(identity)), tools_(std::move(tools)) {}

  std::shared_ptr<AssistantMessage>
  stream(const Model &model, const AgentContext &context, const StreamOptions &,
         AssistantEventCallback, std::stop_token) override {
    *identity_ = context.runtime_identity;
    if (tools_) {
      tools_->clear();
      for (const auto &tool : context.tools)
        tools_->emplace_back(tool->name());
    }
    auto message = std::make_shared<AssistantMessage>();
    message->api = model.api;
    message->provider = model.provider;
    message->model = model.id;
    message->stop_reason = StopReason::stop;
    message->content.emplace_back(TextContent{.text = "identity result"});
    return message;
  }

  std::string_view provider_name() const override { return "identity-test"; }
  std::string_view api_id() const override { return "identity-test"; }

private:
  std::shared_ptr<std::optional<AgentRuntimeIdentity>> identity_;
  std::shared_ptr<std::vector<std::string>> tools_;
};

class BlockingUsageClient : public LLMClient {
public:
  explicit BlockingUsageClient(std::shared_ptr<std::atomic<bool>> entered,
                               std::shared_ptr<std::atomic<bool>> release)
      : entered_(std::move(entered)), release_(std::move(release)) {}

  std::shared_ptr<AssistantMessage>
  stream(const Model &model, const AgentContext &, const StreamOptions &,
         AssistantEventCallback, std::stop_token) override {
    auto message = std::make_shared<AssistantMessage>();
    message->api = model.api;
    message->provider = model.provider;
    message->model = model.id;
    if (calls_++ == 0) {
      // First turn: return immediately so its usage is recorded.
      message->usage =
          TokenUsage{.input = 1000, .output = 200, .total_tokens = 1200};
      message->stop_reason = StopReason::stop;
      return message;
    }
    // Second turn: block so the test can observe the first turn's usage and
    // the growing transcript through a concurrent get() while still running.
    entered_->store(true);
    while (!release_->load())
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    message->usage =
        TokenUsage{.input = 2000, .output = 500, .total_tokens = 2500};
    message->stop_reason = StopReason::stop;
    return message;
  }

  std::string_view provider_name() const override { return "blocking-usage"; }
  std::string_view api_id() const override { return "blocking-usage"; }

private:
  std::shared_ptr<std::atomic<bool>> entered_;
  std::shared_ptr<std::atomic<bool>> release_;
  int calls_{0}; // per-instance; share one instance to count across turns
};

AgentTaskSnapshot wait_terminal(AgentTaskManager &manager,
                                AgentTaskSnapshot current) {
  for (int attempts = 0; attempts < 5; ++attempts) {
    if (current.status == AgentTaskStatusKind::completed ||
        current.status == AgentTaskStatusKind::errored ||
        current.status == AgentTaskStatusKind::interrupted)
      return current;
    AgentWaitRequest request;
    request.targets = {current.id};
    request.after_generation = current.generation;
    request.timeout = std::chrono::seconds(2);
    auto update = manager.wait(request);
    EXPECT_TRUE(!update.caller_interrupted);
    if (update.timed_out) {
      EXPECT_TRUE(!update.timed_out);
      const auto snapshot = manager.get(current.id).value_or(current);
      std::cout << "wait timed out for task " << current.id
                << " status=" << agent_task_status_to_string(snapshot.status)
                << " generation=" << snapshot.generation
                << " queued=" << snapshot.queued_message_count << "\n";
      return snapshot;
    }
    for (const auto &changed : update.changed)
      if (changed.id == current.id)
        current = changed;
  }
  EXPECT_TRUE(false);
  return current;
}

void register_scripted_client(std::string provider,
                              std::vector<FauxClient::Script> scripts) {
  auto client = std::make_shared<FauxClient>(std::move(scripts));
  LLMClientRegistry::instance().register_client(std::move(provider),
                                                [client] { return client; });
}

class AgentTaskManagerShutdown {
public:
  explicit AgentTaskManagerShutdown(AgentTaskManager &manager)
      : manager_(&manager) {}

  AgentTaskManagerShutdown(const AgentTaskManagerShutdown &) = delete;
  AgentTaskManagerShutdown &
  operator=(const AgentTaskManagerShutdown &) = delete;

  ~AgentTaskManagerShutdown() {
    if (manager_ != nullptr)
      manager_->shutdown();
  }

  void shutdown() {
    if (manager_ != nullptr) {
      manager_->shutdown();
      manager_ = nullptr;
    }
  }

  void release() noexcept { manager_ = nullptr; }

private:
  AgentTaskManager *manager_;
};
} // namespace

TEST(AgentTasks, PropagatesRuntimeIdentityAndFiltersTools) {
  Model model;
  model.id = "task-model";
  model.api = "identity-test";
  model.provider = "identity-test";
  Agent::Options options;
  options.model = model;
  auto observed_identity =
      std::make_shared<std::optional<AgentRuntimeIdentity>>();
  auto observed_tools = std::make_shared<std::vector<std::string>>();
  LLMClientRegistry::instance().register_client(
      "identity-test", [observed_identity, observed_tools] {
        return std::make_shared<IdentityClient>(observed_identity,
                                                observed_tools);
      });
  Agent::Options identity_options = options;
  identity_options.model = Model{.id = "identity-model",
                                 .api = "identity-test",
                                 .provider = "identity-test"};
  auto identity_tools = create_read_only_tools();
  const auto mailbox_path =
      std::filesystem::path(PI_CPP_SOURCE_DIR) / "addons" / "mailbox.lua";
  auto mailbox_hooks = load_lua_hooks(mailbox_path, true);
  identity_tools.insert(identity_tools.end(),
                        mailbox_hooks->registered_tools.begin(),
                        mailbox_hooks->registered_tools.end());
  const auto addon_path =
      std::filesystem::temp_directory_path() /
      ("pici-agent-task-addon-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()) +
       "-" + std::to_string(std::random_device{}()) + ".lua");
  struct TemporaryAddonCleanup {
    std::filesystem::path path;
    ~TemporaryAddonCleanup() {
      std::error_code error;
      std::filesystem::remove(path, error);
    }
  } addon_cleanup{addon_path};
  {
    std::ofstream addon(addon_path);
    addon << R"lua(
pici.add_tool({
  name = "arbitrary_addon",
  description = "not inherited by child agents",
  schema = '{"type":"object"}',
  execute = function() return "ok" end,
})
return {}
)lua";
  }
  auto addon_hooks = load_lua_hooks(addon_path);
  identity_tools.insert(identity_tools.end(),
                        addon_hooks->registered_tools.begin(),
                        addon_hooks->registered_tools.end());
  SessionRuntime identity_root(
      {.agent_options = identity_options, .tools = std::move(identity_tools)});
  AgentTaskManager identity_manager(identity_root, identity_options);
  AgentTaskManagerShutdown identity_shutdown(identity_manager);
  std::vector<AgentRuntimeIdentity> registered;
  identity_manager.set_endpoint_registration(
      [&](const AgentTaskId &task_id, const std::string &task_path,
          const std::optional<AgentTaskId> &parent_id) {
        auto identity = AgentRuntimeIdentity{
            .agent_id = "endpoint-" + std::to_string(registered.size() + 1),
            .session_id = "session-root",
            .kind = "subagent",
            .task_id = task_id,
            .task_path = task_path,
            .owner_agent_id = "root-endpoint"};
        EXPECT_TRUE(parent_id && *parent_id == "root");
        registered.push_back(identity);
        return identity;
      },
      [&](const AgentTaskId &task_id) {
        std::erase_if(registered, [&](const auto &identity) {
          return identity.task_id && *identity.task_id == task_id;
        });
      });
  const auto identity_child =
      identity_manager.spawn({.task_name = "identity", .prompt = "identify"});
  const auto identity_result = wait_terminal(identity_manager, identity_child);
  EXPECT_TRUE(identity_result.status == AgentTaskStatusKind::completed);
  ASSERT_TRUE(observed_identity->has_value());
  EXPECT_TRUE(observed_identity->value().agent_id == "endpoint-1");
  EXPECT_TRUE(observed_identity->value().session_id == "session-root");
  EXPECT_TRUE(observed_identity->value().kind == "subagent");
  EXPECT_TRUE(observed_identity->value().task_id == identity_child.id);
  EXPECT_TRUE(observed_identity->value().task_path == identity_child.task_path);
  const auto has_tool = [&](std::string_view name) {
    return std::ranges::find(*observed_tools, name) != observed_tools->end();
  };
  for (const auto name :
       {"read", "grep", "find", "ls", "agents_self", "agents_list",
        "agents_send", "agents_request", "agents_reply", "agents_inbox"})
    EXPECT_TRUE(has_tool(name));
  EXPECT_TRUE(!has_tool("agents_close"));
  EXPECT_TRUE(!has_tool("arbitrary_addon"));
  identity_shutdown.shutdown();

  SessionRuntime write_root(
      {.agent_options = identity_options, .tools = create_coding_tools()});
  AgentTaskManager write_manager(write_root, identity_options,
                                 AgentTaskManager::Limits{}, {},
                                 AgentTaskManager::ChildWriteTools::core);
  AgentTaskManagerShutdown write_shutdown(write_manager);
  const auto write_child = write_manager.spawn({.task_name = "writer",
                                                .prompt = "write",
                                                .requested_tools = {"edit"},
                                                .allow_write_tools = true});
  const auto write_result = wait_terminal(write_manager, write_child);
  EXPECT_TRUE(write_result.status == AgentTaskStatusKind::completed);
  EXPECT_TRUE(has_tool("edit"));
  EXPECT_TRUE(!has_tool("write"));
  write_shutdown.shutdown();

  AgentTaskManager rollback_manager(identity_root, identity_options);
  AgentTaskManagerShutdown rollback_shutdown(rollback_manager);
  bool unregister_called = false;
  rollback_manager.set_endpoint_registration(
      [](const AgentTaskId &, const std::string &,
         const std::optional<AgentTaskId> &) -> AgentRuntimeIdentity {
        throw std::runtime_error("registration failed");
      },
      [&](const AgentTaskId &) { unregister_called = true; });
  bool registration_failed = false;
  try {
    static_cast<void>(rollback_manager.spawn(
        {.task_name = "registration-failure", .prompt = "fail"}));
  } catch (const std::runtime_error &) {
    registration_failed = true;
  }
  EXPECT_TRUE(registration_failed);
  EXPECT_TRUE(!unregister_called);
  EXPECT_TRUE(rollback_manager.resident_tasks() == 0);
  EXPECT_TRUE(rollback_manager.active_executions() == 0);
  rollback_shutdown.shutdown();

  AgentTaskManager construction_manager(identity_root, identity_options);
  AgentTaskManagerShutdown construction_shutdown(construction_manager);
  bool construction_unregistered = false;
  construction_manager.set_endpoint_registration(
      [](const AgentTaskId &task_id, const std::string &task_path,
         const std::optional<AgentTaskId> &) {
        return AgentRuntimeIdentity{.agent_id = "construction-" + task_id,
                                    .session_id = "session-root",
                                    .kind = "subagent",
                                    .task_id = task_id,
                                    .task_path = task_path,
                                    .owner_agent_id = "root-endpoint"};
      },
      [&](const AgentTaskId &) { construction_unregistered = true; });
  bool construction_failed = false;
  try {
    static_cast<void>(
        construction_manager.spawn({.task_name = "construction-failure",
                                    .prompt = "fail",
                                    .requested_tools = {"missing-tool"}}));
  } catch (const AgentTaskError &error) {
    construction_failed = error.kind() == AgentTaskErrorKind::invalid_tool;
  }
  EXPECT_TRUE(construction_failed);
  EXPECT_TRUE(construction_unregistered);
  EXPECT_TRUE(construction_manager.resident_tasks() == 0);
  EXPECT_TRUE(construction_manager.active_executions() == 0);
  construction_shutdown.shutdown();
}

TEST(AgentTasks, RejectsDuplicatePendingTaskDuringShutdown) {
  Model model;
  model.id = "identity-model";
  model.api = "identity-test";
  model.provider = "identity-test";
  Agent::Options options;
  options.model = model;
  Agent::Options identity_options = options;
  LLMClientRegistry::instance().register_client("identity-test", [] {
    return std::make_shared<IdentityClient>(
        std::make_shared<std::optional<AgentRuntimeIdentity>>());
  });
  SessionRuntime concurrency_root({.agent_options = identity_options});
  AgentTaskManager concurrency_manager(concurrency_root, identity_options);
  AgentTaskManagerShutdown concurrency_shutdown(concurrency_manager);
  std::mutex registration_mutex;
  std::condition_variable registration_changed;
  bool registration_entered = false;
  bool registration_release = false;
  std::atomic<int> unregister_count{0};
  concurrency_manager.set_endpoint_registration(
      [&](const AgentTaskId &task_id, const std::string &task_path,
          const std::optional<AgentTaskId> &parent_id) {
        {
          std::scoped_lock lock(registration_mutex);
          registration_entered = true;
        }
        registration_changed.notify_all();
        std::unique_lock lock(registration_mutex);
        registration_changed.wait(lock, [&] { return registration_release; });
        return AgentRuntimeIdentity{.agent_id = "blocked-" + task_id,
                                    .session_id = "session-root",
                                    .kind = "subagent",
                                    .task_id = task_id,
                                    .task_path = task_path,
                                    .owner_agent_id = "root-endpoint"};
      },
      [&](const AgentTaskId &) { ++unregister_count; });
  std::atomic<bool> spawn_failed{false};
  std::thread blocked_spawn([&] {
    try {
      static_cast<void>(concurrency_manager.spawn(
          {.task_name = "blocked", .prompt = "blocked"}));
    } catch (const AgentTaskError &) {
      spawn_failed = true;
    }
  });
  {
    std::unique_lock lock(registration_mutex);
    registration_changed.wait(lock, [&] { return registration_entered; });
  }
  bool duplicate_pending_rejected = false;
  try {
    static_cast<void>(concurrency_manager.spawn(
        {.task_name = "blocked", .prompt = "duplicate"}));
  } catch (const AgentTaskError &error) {
    duplicate_pending_rejected =
        error.kind() == AgentTaskErrorKind::duplicate_name;
  }
  EXPECT_TRUE(duplicate_pending_rejected);
  EXPECT_TRUE(concurrency_manager.list().size() == 1);
  std::thread shutting_down([&] { concurrency_shutdown.shutdown(); });
  while (!concurrency_manager.is_shutting_down())
    std::this_thread::yield();
  {
    std::scoped_lock lock(registration_mutex);
    registration_release = true;
  }
  registration_changed.notify_all();
  blocked_spawn.join();
  shutting_down.join();
  concurrency_shutdown.release();
  EXPECT_TRUE(spawn_failed);
  EXPECT_TRUE(concurrency_manager.resident_tasks() == 0);
  EXPECT_TRUE(concurrency_manager.active_executions() == 0);
  EXPECT_TRUE(unregister_count == 1);
}

TEST(AgentTasks, EnforcesDirectChildCapacity) {
  Model model;
  model.id = "identity-model";
  model.api = "identity-test";
  model.provider = "identity-test";
  Agent::Options options;
  options.model = model;
  Agent::Options identity_options = options;
  LLMClientRegistry::instance().register_client("identity-test", [] {
    return std::make_shared<IdentityClient>(
        std::make_shared<std::optional<AgentRuntimeIdentity>>());
  });
  SessionRuntime capacity_root({.agent_options = identity_options});
  AgentTaskManager::Limits capacity_limits;
  capacity_limits.max_direct_children = 2;
  AgentTaskManager capacity_manager(capacity_root, identity_options,
                                    capacity_limits);
  AgentTaskManagerShutdown capacity_shutdown(capacity_manager);
  std::mutex capacity_mutex;
  std::condition_variable capacity_changed;
  std::size_t capacity_entered = 0;
  bool capacity_release = false;
  capacity_manager.set_endpoint_registration(
      [&](const AgentTaskId &task_id, const std::string &task_path,
          const std::optional<AgentTaskId> &) {
        {
          std::scoped_lock lock(capacity_mutex);
          ++capacity_entered;
        }
        capacity_changed.notify_all();
        std::unique_lock lock(capacity_mutex);
        capacity_changed.wait(lock, [&] { return capacity_release; });
        return AgentRuntimeIdentity{.agent_id = "capacity-" + task_id,
                                    .session_id = "session-root",
                                    .kind = "subagent",
                                    .task_id = task_id,
                                    .task_path = task_path,
                                    .owner_agent_id = "root-endpoint"};
      },
      [](const AgentTaskId &) {});
  std::thread capacity_first([&] {
    static_cast<void>(
        capacity_manager.spawn({.task_name = "first", .prompt = "first"}));
  });
  std::thread capacity_second([&] {
    static_cast<void>(
        capacity_manager.spawn({.task_name = "second", .prompt = "second"}));
  });
  {
    std::unique_lock lock(capacity_mutex);
    capacity_changed.wait(lock, [&] { return capacity_entered == 2; });
  }
  bool third_rejected = false;
  try {
    static_cast<void>(
        capacity_manager.spawn({.task_name = "third", .prompt = "third"}));
  } catch (const AgentTaskError &error) {
    third_rejected = error.kind() == AgentTaskErrorKind::residency_limit;
  }
  EXPECT_TRUE(third_rejected);
  EXPECT_TRUE(capacity_manager.list().size() == 1);
  {
    std::scoped_lock lock(capacity_mutex);
    capacity_release = true;
  }
  capacity_changed.notify_all();
  capacity_first.join();
  capacity_second.join();
  EXPECT_TRUE(capacity_manager.resident_tasks() == 2);
  capacity_shutdown.shutdown();
}

TEST(AgentTasks, RejectsNestedSpawnAfterParentClose) {
  Model model;
  model.id = "identity-model";
  model.api = "identity-test";
  model.provider = "identity-test";
  Agent::Options options;
  options.model = model;
  Agent::Options identity_options = options;
  LLMClientRegistry::instance().register_client("identity-test", [] {
    return std::make_shared<IdentityClient>(
        std::make_shared<std::optional<AgentRuntimeIdentity>>());
  });
  SessionRuntime parent_root({.agent_options = identity_options});
  AgentTaskManager parent_manager(parent_root, identity_options);
  AgentTaskManagerShutdown parent_shutdown(parent_manager);
  std::mutex parent_mutex;
  std::condition_variable parent_changed;
  bool nested_entered = false;
  bool nested_release = false;
  std::atomic<int> parent_unregister_count{0};
  parent_manager.set_endpoint_registration(
      [&](const AgentTaskId &task_id, const std::string &task_path,
          const std::optional<AgentTaskId> &parent_id) {
        if (task_path.ends_with("/nested")) {
          {
            std::scoped_lock lock(parent_mutex);
            nested_entered = true;
          }
          parent_changed.notify_all();
          std::unique_lock lock(parent_mutex);
          parent_changed.wait(lock, [&] { return nested_release; });
        }
        return AgentRuntimeIdentity{.agent_id = "parent-" + task_id,
                                    .session_id = "session-root",
                                    .kind = "subagent",
                                    .task_id = task_id,
                                    .task_path = task_path,
                                    .owner_agent_id = "root-endpoint"};
      },
      [&](const AgentTaskId &) { ++parent_unregister_count; });
  const auto parent_child =
      parent_manager.spawn({.task_name = "parent", .prompt = "parent"});
  const auto parent_done = wait_terminal(parent_manager, parent_child);
  EXPECT_TRUE(parent_done.status == AgentTaskStatusKind::completed);
  std::atomic<bool> nested_failed{false};
  std::thread nested_spawn([&] {
    try {
      static_cast<void>(parent_manager.spawn({.parent_id = parent_child.id,
                                              .task_name = "nested",
                                              .prompt = "nested"}));
    } catch (const AgentTaskError &error) {
      nested_failed = error.kind() == AgentTaskErrorKind::invalid_state;
    }
  });
  {
    std::unique_lock lock(parent_mutex);
    parent_changed.wait(lock, [&] { return nested_entered; });
  }
  EXPECT_TRUE(parent_manager.list().size() == 2);
  static_cast<void>(parent_manager.close(parent_child.id));
  {
    std::scoped_lock lock(parent_mutex);
    nested_release = true;
  }
  parent_changed.notify_all();
  nested_spawn.join();
  EXPECT_TRUE(nested_failed);
  EXPECT_TRUE(parent_unregister_count == 2);
  EXPECT_TRUE(parent_manager.resident_tasks() == 0);
  parent_shutdown.shutdown();
}

TEST(AgentTasks, WaitsForUnregisterBeforeShutdown) {
  Model model;
  model.id = "identity-model";
  model.api = "identity-test";
  model.provider = "identity-test";
  Agent::Options options;
  options.model = model;
  Agent::Options identity_options = options;
  LLMClientRegistry::instance().register_client("identity-test", [] {
    return std::make_shared<IdentityClient>(
        std::make_shared<std::optional<AgentRuntimeIdentity>>());
  });
  SessionRuntime unregister_root({.agent_options = identity_options});
  AgentTaskManager unregister_manager(unregister_root, identity_options);
  std::mutex unregister_mutex;
  std::condition_variable unregister_changed;
  bool nested_registration_entered = false;
  bool nested_registration_release = false;
  bool unregister_entered = false;
  bool unregister_release = false;
  bool shutdown_started = false;
  std::string nested_task_id;
  unregister_manager.set_endpoint_registration(
      [&](const AgentTaskId &task_id, const std::string &task_path,
          const std::optional<AgentTaskId> &) {
        if (task_path.ends_with("/nested")) {
          {
            std::scoped_lock lock(unregister_mutex);
            nested_task_id = task_id;
            nested_registration_entered = true;
          }
          unregister_changed.notify_all();
          std::unique_lock wait_lock(unregister_mutex);
          unregister_changed.wait(wait_lock,
                                  [&] { return nested_registration_release; });
        }
        return AgentRuntimeIdentity{.agent_id = "unregister-" + task_id,
                                    .session_id = "session-root",
                                    .kind = "subagent",
                                    .task_id = task_id,
                                    .task_path = task_path,
                                    .owner_agent_id = "root-endpoint"};
      },
      [&](const AgentTaskId &task_id) {
        bool block = false;
        {
          std::scoped_lock lock(unregister_mutex);
          block = task_id == nested_task_id;
        }
        if (!block)
          return;
        {
          std::scoped_lock lock(unregister_mutex);
          unregister_entered = true;
        }
        unregister_changed.notify_all();
        std::unique_lock wait_lock(unregister_mutex);
        unregister_changed.wait(wait_lock, [&] { return unregister_release; });
      });
  const auto unregister_parent =
      unregister_manager.spawn({.task_name = "parent", .prompt = "parent"});
  EXPECT_TRUE(wait_terminal(unregister_manager, unregister_parent).status ==
              AgentTaskStatusKind::completed);
  std::atomic<bool> nested_registration_failed{false};
  std::thread unregister_spawn([&] {
    try {
      static_cast<void>(
          unregister_manager.spawn({.parent_id = unregister_parent.id,
                                    .task_name = "nested",
                                    .prompt = "nested"}));
    } catch (const AgentTaskError &error) {
      nested_registration_failed =
          error.kind() == AgentTaskErrorKind::invalid_state;
    }
  });
  {
    std::unique_lock lock(unregister_mutex);
    unregister_changed.wait(lock, [&] { return nested_registration_entered; });
  }
  static_cast<void>(unregister_manager.close(unregister_parent.id));
  {
    std::scoped_lock lock(unregister_mutex);
    nested_registration_release = true;
  }
  unregister_changed.notify_all();
  {
    std::unique_lock lock(unregister_mutex);
    unregister_changed.wait(lock, [&] { return unregister_entered; });
  }
  std::atomic<bool> unregister_shutdown_done{false};
  std::thread unregister_shutdown([&] {
    {
      std::scoped_lock lock(unregister_mutex);
      shutdown_started = true;
    }
    unregister_changed.notify_all();
    unregister_manager.shutdown();
    unregister_shutdown_done = true;
  });
  {
    std::unique_lock lock(unregister_mutex);
    unregister_changed.wait(lock, [&] { return shutdown_started; });
  }
  while (!unregister_manager.is_shutting_down())
    std::this_thread::yield();
  EXPECT_TRUE(!unregister_shutdown_done.load());
  {
    std::scoped_lock lock(unregister_mutex);
    unregister_release = true;
  }
  unregister_changed.notify_all();
  unregister_spawn.join();
  unregister_shutdown.join();
  EXPECT_TRUE(nested_registration_failed);
  EXPECT_TRUE(unregister_shutdown_done.load());
  EXPECT_TRUE(unregister_manager.resident_tasks() == 0);
}

TEST(AgentTasks, RunsLifecycleFollowUpsAndMailboxInput) {
  register_scripted_client("faux", {response("child result"),
                                    response("follow-up result"),
                                    response("third result")});
  Model model;
  model.id = "task-model";
  model.api = "faux";
  model.provider = "faux";
  Agent::Options options;
  options.model = model;
  SessionRuntime root({.agent_options = options});
  AgentTaskManager manager(root, options);
  AgentTaskManagerShutdown manager_shutdown(manager);
  auto child = manager.spawn({.task_name = "review", .prompt = "Review"});
  EXPECT_TRUE(!child.id.empty());
  EXPECT_TRUE(child.task_path == "/root/review");

  // Context observability: the spawn snapshot carries live context info.
  ASSERT_TRUE(child.context_info.has_value());
  {
    const auto &info = *child.context_info;
    // Seed prompt is queued in work; the transcript fills on MessageEndEvent,
    // so at spawn time it is still empty.
    EXPECT_TRUE(info.message_count == 0);
    EXPECT_TRUE(info.context_bytes == 0);
    EXPECT_TRUE(info.last_input_tokens == 0);
    EXPECT_TRUE(info.total_tokens == 0);
    // Faux model declares no window.
    EXPECT_TRUE(!info.context_window.has_value());
  }

  auto completed = wait_terminal(manager, child);
  EXPECT_TRUE(completed.status == AgentTaskStatusKind::completed);
  ASSERT_TRUE(completed.result.has_value());
  EXPECT_TRUE(completed.result->text == "child result");
  ASSERT_TRUE(completed.context_info.has_value());
  EXPECT_TRUE(completed.context_info->last_input_tokens == 0);
  // FauxClient scripts carry no usage; total falls back to input+output = 0.
  EXPECT_TRUE(completed.context_info->total_tokens == 0);
  EXPECT_TRUE(completed.context_info->message_count >= 2); // prompt + assistant

  auto queued = manager.send_message(
      child.id, UserMessage{.content = {TextContent{.text = "context"}}});
  EXPECT_TRUE(queued.queued_message_count == 1);
  auto follow = manager.follow_up(
      child.id, UserMessage{.content = {TextContent{.text = "Summarize"}}});
  auto second = wait_terminal(manager, follow);
  EXPECT_TRUE(second.status == AgentTaskStatusKind::completed);
  ASSERT_TRUE(second.result.has_value());
  EXPECT_TRUE(second.result->text == "follow-up result");
  // Context grew across the follow-up turn.
  ASSERT_TRUE(second.context_info.has_value());
  EXPECT_TRUE(second.context_info->message_count >
              completed.context_info->message_count);
  EXPECT_TRUE(second.context_info->context_bytes >
              completed.context_info->context_bytes);

  int accepted = 0;
  UserMessage mailbox_message;
  mailbox_message.content.emplace_back(TextContent{.text = "mailbox"});
  AgentInput mailbox_envelope{
      .message = Message{std::move(mailbox_message)},
      .on_accepted = [&accepted] { ++accepted; },
      .presentation = {.source = InputProvenance::Source::mailbox}};
  auto reactivated =
      manager.steer_envelopes(child.id, {std::move(mailbox_envelope)});
  auto third = wait_terminal(manager, reactivated);
  EXPECT_TRUE(third.status == AgentTaskStatusKind::completed);
  EXPECT_TRUE(third.result && third.result->text == "third result");
  EXPECT_TRUE(accepted == 1);

  const auto all = manager.list();
  EXPECT_TRUE(all.size() == 2);
  EXPECT_TRUE(all.front().task_path == "/root");
  EXPECT_TRUE(manager.resident_tasks() == 1);

  const auto closed = manager.close(child.id);
  EXPECT_TRUE(closed.status == AgentTaskStatusKind::shutdown);
  EXPECT_TRUE(!manager.get(child.id).has_value());
  EXPECT_TRUE(manager.resident_tasks() == 0);
  manager_shutdown.shutdown();
}

TEST(AgentTasks, InterruptsAndReusesTask) {
  auto interrupt_calls = std::make_shared<std::atomic<int>>(0);
  LLMClientRegistry::instance().register_client(
      "interrupt-test", [interrupt_calls] {
        return std::make_shared<InterruptClient>(interrupt_calls);
      });
  Model interrupt_model;
  interrupt_model.id = "interrupt-model";
  interrupt_model.api = "interrupt-test";
  interrupt_model.provider = "interrupt-test";
  Agent::Options interrupt_options;
  interrupt_options.model = interrupt_model;
  SessionRuntime interrupt_root({.agent_options = interrupt_options});
  AgentTaskManager interrupt_manager(interrupt_root, interrupt_options);
  AgentTaskManagerShutdown interrupt_shutdown(interrupt_manager);
  auto interrupted =
      interrupt_manager.spawn({.task_name = "slow", .prompt = "wait"});
  ASSERT_TRUE(pi::test::wait_until(
      [&] {
        const auto snapshot = interrupt_manager.get(interrupted.id);
        return snapshot && snapshot->status == AgentTaskStatusKind::running;
      },
      std::chrono::seconds(2), "interrupt task to start"));
  ASSERT_TRUE(pi::test::wait_until([&] { return interrupt_calls->load() == 1; },
                                   std::chrono::seconds(2),
                                   "interrupt client to be called"));
  auto immediate = interrupt_manager.interrupt(interrupted.id,
                                               AgentInterruptReason::timeout);
  EXPECT_TRUE(immediate.status == AgentTaskStatusKind::running);
  auto settled = wait_terminal(interrupt_manager, immediate);
  EXPECT_TRUE(settled.status == AgentTaskStatusKind::interrupted);
  ASSERT_TRUE(settled.result.has_value());
  EXPECT_TRUE(settled.result->stop_reason == StopReason::aborted);
  auto reused = interrupt_manager.follow_up(
      interrupted.id, UserMessage{.content = {TextContent{.text = "retry"}}});
  auto reused_result = wait_terminal(interrupt_manager, reused);
  EXPECT_TRUE(reused_result.status == AgentTaskStatusKind::completed);
  EXPECT_TRUE(reused_result.result && reused_result.result->text == "reused");
  interrupt_shutdown.shutdown();
}

TEST(AgentTasks, ReportsMidTurnUsage) {
  // --- Mid-turn context observability -----------------------------------
  // The child runs two turns: turn 1 returns immediately (usage recorded),
  // turn 2 (queued via follow_up) blocks inside stream() so the test can
  // observe turn 1's usage and the growing transcript through get() while
  // the task is still running.
  auto entered = std::make_shared<std::atomic<bool>>(false);
  auto release = std::make_shared<std::atomic<bool>>(false);
  // One shared instance so calls_ counts across turns.
  auto usage_client = std::make_shared<BlockingUsageClient>(entered, release);
  LLMClientRegistry::instance().register_client(
      "blocking-usage", [usage_client] { return usage_client; });
  Model usage_model;
  usage_model.id = "usage-model";
  usage_model.api = "blocking-usage";
  usage_model.provider = "blocking-usage";
  usage_model.context_window = 100000; // declared window must surface
  Agent::Options usage_options;
  usage_options.model = usage_model;
  SessionRuntime usage_root({.agent_options = usage_options});
  AgentTaskManager usage_manager(usage_root, usage_options);
  AgentTaskManagerShutdown usage_shutdown(usage_manager);
  auto observed_child =
      usage_manager.spawn({.task_name = "observed", .prompt = "observe"});

  // Spawn snapshot: seed prompt still queued in work, not yet in state.
  ASSERT_TRUE(observed_child.context_info.has_value());
  EXPECT_TRUE(observed_child.context_info->message_count == 0);
  EXPECT_TRUE(observed_child.context_info->last_input_tokens == 0);
  // Window resolved from the child's model at spawn time.
  EXPECT_TRUE(observed_child.context_info->context_window.value_or(0) ==
              100000);

  // Queue turn 2 up front: it starts once turn 1 finishes.
  static_cast<void>(usage_manager.follow_up(
      observed_child.id,
      Message{UserMessage{.content = {TextContent{.text = "second"}}}}));

  // Wait until turn 2 is streaming...
  ASSERT_TRUE(pi::test::wait_until([&] { return entered->load(); },
                                   std::chrono::seconds(5),
                                   "usage client to enter second turn"));
  // ...and observe turn-1 usage live through a plain get().
  const auto mid_turn = usage_manager.get(observed_child.id);
  ASSERT_TRUE(mid_turn.has_value());
  EXPECT_TRUE(mid_turn->status == AgentTaskStatusKind::running);
  ASSERT_TRUE(mid_turn->context_info.has_value());
  EXPECT_TRUE(mid_turn->context_info->last_input_tokens == 1000);
  EXPECT_TRUE(mid_turn->context_info->last_output_tokens == 200);
  EXPECT_TRUE(mid_turn->context_info->total_tokens == 1200);
  EXPECT_TRUE(mid_turn->context_info->context_window.value_or(0) == 100000);
  // Turn 1 transcript: prompt + assistant.
  EXPECT_TRUE(mid_turn->context_info->message_count >= 2);
  EXPECT_TRUE(mid_turn->context_info->context_bytes > 0);

  release->store(true);
  const auto usage_done = wait_terminal(usage_manager, observed_child);
  EXPECT_TRUE(usage_done.status == AgentTaskStatusKind::completed);
  ASSERT_TRUE(usage_done.context_info.has_value());
  // Turn-2 usage replaced turn-1 usage; total falls back to input+output.
  EXPECT_TRUE(usage_done.context_info->last_input_tokens == 2000);
  EXPECT_TRUE(usage_done.context_info->total_tokens == 2500);
  EXPECT_TRUE(usage_done.context_info->message_count ==
              mid_turn->context_info->message_count + 1); // + turn-2 assistant
  usage_shutdown.shutdown();
}

TEST(AgentTasks, DoesNotSpliceChildResultIntoParent) {
  // --- Delegated task results never splice into the parent transcript ----
  // Lexicon delegated-task flow step 6: "Results use task APIs or explicit
  // mailbox entries; they never silently splice into the parent transcript."
  // A child's completed result must be visible only through
  // AgentTaskManager's own snapshot/result API, never appended to the
  // parent SessionRuntime's own message history.
  auto splice_client = std::make_shared<FauxClient>(
      std::vector{response("splice-check distinctive child output")});
  LLMClientRegistry::instance().register_client(
      "splice-check", [splice_client] { return splice_client; });
  Model splice_model;
  splice_model.id = "splice-model";
  splice_model.api = "splice-check";
  splice_model.provider = "splice-check";
  Agent::Options splice_options;
  splice_options.model = splice_model;
  SessionRuntime splice_root({.agent_options = splice_options});
  AgentTaskManager splice_manager(splice_root, splice_options);
  AgentTaskManagerShutdown splice_shutdown(splice_manager);
  EXPECT_TRUE(splice_root.agent().state().messages().size() == std::size_t{0});
  const auto splice_child = splice_manager.spawn(
      {.task_name = "splice-child", .prompt = "produce distinctive output"});
  const auto splice_done = wait_terminal(splice_manager, splice_child);
  EXPECT_TRUE(splice_done.status == AgentTaskStatusKind::completed);
  ASSERT_TRUE(splice_done.result.has_value());
  EXPECT_TRUE(splice_done.result->text ==
              "splice-check distinctive child output");
  // The parent's own transcript must be untouched: no messages appended,
  // and the child's distinctive text must not appear anywhere in it.
  EXPECT_TRUE(splice_root.agent().state().messages().size() == std::size_t{0});
  const bool leaked_into_parent = std::ranges::any_of(
      splice_root.agent().state().messages(), [](const Message &message) {
        const auto *assistant = std::get_if<AssistantMessage>(&message);
        if (assistant == nullptr)
          return false;
        for (const auto &block : assistant->content) {
          const auto *text = std::get_if<TextContent>(&block);
          if (text != nullptr &&
              text->text.find("splice-check distinctive child output") !=
                  std::string::npos)
            return true;
        }
        return false;
      });
  EXPECT_TRUE(!leaked_into_parent);
  splice_shutdown.shutdown();
}
