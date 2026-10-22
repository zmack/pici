#include "core/agent_task.h"
#include "core/builtin_tools.h"
#include "core/child_completion.h"
#include "core/llm_client.h"
#include "core/lua_tool.h"
#include "core/providers/faux.h"
#include "core/session/session_runtime.h"
#include "core/task_event_json.h"
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

// Mirrors SessionRuntime::activate()'s production factory exactly (children
// get agent_options/tools only, never a durable session_store), so these
// direct TaskTree constructions behave exactly as they did when TaskTree
// built child SessionRuntimes itself, pre-Phase-7.
ChildSessionFactory default_child_factory() {
  return [](ChildSessionSpec spec) {
    return std::make_unique<SessionRuntime>(SessionRuntime::Config{
        .agent_options = std::move(spec.agent_options),
        .tools = std::move(spec.tools),
        .session_store = std::move(spec.session_store),
    });
  };
}

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

AgentTaskSnapshot wait_terminal(TaskTree &manager, AgentTaskSnapshot current) {
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

class TaskTreeShutdown {
public:
  explicit TaskTreeShutdown(TaskTree &manager) : manager_(&manager) {}

  TaskTreeShutdown(const TaskTreeShutdown &) = delete;
  TaskTreeShutdown &operator=(const TaskTreeShutdown &) = delete;

  ~TaskTreeShutdown() {
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
  TaskTree *manager_;
};

// Returns one fixed, non-streamed assistant message (no text deltas) and
// records the transcript each call was handed.
class FixedReplyClient : public LLMClient {
public:
  FixedReplyClient(std::string reply,
                   std::shared_ptr<std::vector<Message>> seen = {})
      : reply_(std::move(reply)), seen_(std::move(seen)) {}

  std::shared_ptr<AssistantMessage>
  stream(const Model &model, const AgentContext &context, const StreamOptions &,
         AssistantEventCallback, std::stop_token) override {
    if (seen_)
      *seen_ = context.messages;
    auto message = std::make_shared<AssistantMessage>();
    message->api = model.api;
    message->provider = model.provider;
    message->model = model.id;
    message->stop_reason = StopReason::stop;
    message->content.emplace_back(TextContent{.text = reply_});
    return message;
  }

  std::string_view provider_name() const override { return "fixed-reply"; }
  std::string_view api_id() const override { return "fixed-reply"; }

private:
  std::string reply_;
  std::shared_ptr<std::vector<Message>> seen_;
};

Agent::Options fixed_reply_options(const std::string &provider,
                                   std::string reply,
                                   std::shared_ptr<std::vector<Message>> seen) {
  LLMClientRegistry::instance().register_client(
      provider, [reply = std::move(reply), seen = std::move(seen)] {
        return std::make_shared<FixedReplyClient>(reply, seen);
      });
  Agent::Options options;
  options.model =
      Model{.id = provider + "-model", .api = provider, .provider = provider};
  return options;
}

AssistantMessage tool_call_message(std::vector<std::string> ids) {
  AssistantMessage assistant;
  assistant.stop_reason = StopReason::tool_use;
  for (auto &id : ids)
    assistant.content.emplace_back(ToolCall{.id = std::move(id), .name = "ls"});
  return assistant;
}

ToolResultMessage tool_result_message(std::string id) {
  return ToolResultMessage{.tool_call_id = std::move(id),
                           .tool_name = "ls",
                           .content = {TextContent{.text = "ok"}}};
}

UserMessage user_message(std::string text) {
  return UserMessage{.content = {TextContent{.text = std::move(text)}}};
}
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
  TaskTree identity_manager(identity_root.agent(), default_child_factory(),
                            identity_options);
  TaskTreeShutdown identity_shutdown(identity_manager);
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
  TaskTree write_manager(write_root.agent(), default_child_factory(),
                         identity_options, TaskTree::Limits{}, {},
                         TaskTree::ChildWriteTools::core);
  TaskTreeShutdown write_shutdown(write_manager);
  const auto write_child = write_manager.spawn({.task_name = "writer",
                                                .prompt = "write",
                                                .requested_tools = {"edit"},
                                                .allow_write_tools = true});
  const auto write_result = wait_terminal(write_manager, write_child);
  EXPECT_TRUE(write_result.status == AgentTaskStatusKind::completed);
  EXPECT_TRUE(has_tool("edit"));
  EXPECT_TRUE(!has_tool("write"));
  write_shutdown.shutdown();

  TaskTree rollback_manager(identity_root.agent(), default_child_factory(),
                            identity_options);
  TaskTreeShutdown rollback_shutdown(rollback_manager);
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

  TaskTree construction_manager(identity_root.agent(), default_child_factory(),
                                identity_options);
  TaskTreeShutdown construction_shutdown(construction_manager);
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
  TaskTree concurrency_manager(concurrency_root.agent(),
                               default_child_factory(), identity_options);
  TaskTreeShutdown concurrency_shutdown(concurrency_manager);
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
  TaskTree::Limits capacity_limits;
  capacity_limits.max_direct_children = 2;
  TaskTree capacity_manager(capacity_root.agent(), default_child_factory(),
                            identity_options, capacity_limits);
  TaskTreeShutdown capacity_shutdown(capacity_manager);
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
  TaskTree parent_manager(parent_root.agent(), default_child_factory(),
                          identity_options);
  TaskTreeShutdown parent_shutdown(parent_manager);
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
  TaskTree unregister_manager(unregister_root.agent(), default_child_factory(),
                              identity_options);
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
  TaskTree manager(root.agent(), default_child_factory(), options);
  TaskTreeShutdown manager_shutdown(manager);
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
  TaskTree interrupt_manager(interrupt_root.agent(), default_child_factory(),
                             interrupt_options);
  TaskTreeShutdown interrupt_shutdown(interrupt_manager);
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
  TaskTree usage_manager(usage_root.agent(), default_child_factory(),
                         usage_options);
  TaskTreeShutdown usage_shutdown(usage_manager);
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
  // TaskTree's own snapshot/result API, never appended to the
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
  TaskTree splice_manager(splice_root.agent(), default_child_factory(),
                          splice_options);
  TaskTreeShutdown splice_shutdown(splice_manager);
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

TEST(AgentTasks, CapsNonStreamedResultAtOutputLimit) {
  auto options =
      fixed_reply_options("cap-nonstream", std::string(1000, 'x'), {});
  SessionRuntime root({.agent_options = options});
  TaskTree::Limits limits;
  limits.max_output_bytes = 100;
  TaskTree tree(root.agent(), default_child_factory(), options, limits);
  TaskTreeShutdown shutdown(tree);
  const auto done = wait_terminal(
      tree, tree.spawn({.task_name = "big", .prompt = "produce output"}));
  EXPECT_TRUE(done.status == AgentTaskStatusKind::completed);
  ASSERT_TRUE(done.result.has_value());
  EXPECT_LE(done.result->text.size(), limits.max_output_bytes);
  EXPECT_TRUE(done.result->truncated);
  shutdown.shutdown();
}

TEST(AgentTasks, DoesNotTruncateNonStreamedResultWithinLimit) {
  auto options = fixed_reply_options("cap-nonstream-ok", "short", {});
  SessionRuntime root({.agent_options = options});
  TaskTree::Limits limits;
  limits.max_output_bytes = 100;
  TaskTree tree(root.agent(), default_child_factory(), options, limits);
  TaskTreeShutdown shutdown(tree);
  const auto done = wait_terminal(
      tree, tree.spawn({.task_name = "small", .prompt = "produce output"}));
  ASSERT_TRUE(done.result.has_value());
  EXPECT_EQ(done.result->text, "short");
  EXPECT_FALSE(done.result->truncated);
  shutdown.shutdown();
}

TEST(AgentTasks, InheritedContextDropsPartialMultiToolExchange) {
  auto seen = std::make_shared<std::vector<Message>>();
  auto options = fixed_reply_options("ctx-multitool", "done", seen);
  SessionRuntime root({.agent_options = options});
  root.agent().state().set_messages({
      user_message("first"),
      tool_call_message({"call_a", "call_b"}),
      tool_result_message("call_a"),
      tool_result_message("call_b"),
      user_message("second"),
  });
  TaskTree tree(root.agent(), default_child_factory(), options);
  TaskTreeShutdown shutdown(tree);
  // Cutting after the first of two results leaves call_b unanswered.
  const auto done = wait_terminal(
      tree,
      tree.spawn({.task_name = "cut",
                  .prompt = "go",
                  .context = {.mode = ContextInheritanceMode::through_message,
                              .through = 3}}));
  EXPECT_TRUE(done.status == AgentTaskStatusKind::completed);
  for (const auto &message : *seen) {
    EXPECT_FALSE(std::holds_alternative<ToolResultMessage>(message));
    if (const auto *assistant = std::get_if<AssistantMessage>(&message))
      for (const auto &block : assistant->content)
        EXPECT_FALSE(std::holds_alternative<ToolCall>(block));
  }
  shutdown.shutdown();
}

TEST(AgentTasks, InheritedContextKeepsCompleteMultiToolExchange) {
  auto seen = std::make_shared<std::vector<Message>>();
  auto options = fixed_reply_options("ctx-multitool-ok", "done", seen);
  SessionRuntime root({.agent_options = options});
  root.agent().state().set_messages({
      user_message("first"),
      tool_call_message({"call_a", "call_b"}),
      tool_result_message("call_a"),
      tool_result_message("call_b"),
      user_message("second"),
  });
  TaskTree tree(root.agent(), default_child_factory(), options);
  TaskTreeShutdown shutdown(tree);
  const auto done = wait_terminal(
      tree,
      tree.spawn({.task_name = "whole",
                  .prompt = "go",
                  .context = {.mode = ContextInheritanceMode::through_message,
                              .through = 4}}));
  EXPECT_TRUE(done.status == AgentTaskStatusKind::completed);
  // user, assistant(2 calls), 2 results, then the spawn prompt.
  EXPECT_EQ(seen->size(), std::size_t{5});
  shutdown.shutdown();
}

TEST(AgentTasks, RootFollowUpIsRejectedWithoutReservingExecution) {
  auto options = fixed_reply_options("root-followup", "ok", {});
  SessionRuntime root({.agent_options = options});
  TaskTree::Limits limits;
  limits.max_active_executions = 1;
  TaskTree tree(root.agent(), default_child_factory(), options, limits);
  TaskTreeShutdown shutdown(tree);
  try {
    tree.follow_up("root", user_message("again"));
    ADD_FAILURE() << "follow_up on the root pseudo-task must be rejected";
  } catch (const AgentTaskError &error) {
    EXPECT_TRUE(error.kind() == AgentTaskErrorKind::invalid_state);
  }
  const auto root_snapshot = tree.get("root");
  ASSERT_TRUE(root_snapshot.has_value());
  EXPECT_TRUE(root_snapshot->status == AgentTaskStatusKind::idle);
  EXPECT_EQ(root_snapshot->queued_message_count, std::size_t{0});
  // The failed follow-up must not have consumed the only execution slot.
  const auto done = wait_terminal(
      tree, tree.spawn({.task_name = "child", .prompt = "still runs"}));
  EXPECT_TRUE(done.status == AgentTaskStatusKind::completed);
  shutdown.shutdown();
}

TEST(AgentTasks, RootStatusTracksOwningAgent) {
  auto entered = std::make_shared<std::atomic<bool>>(false);
  auto release = std::make_shared<std::atomic<bool>>(false);
  auto client = std::make_shared<BlockingUsageClient>(entered, release);
  LLMClientRegistry::instance().register_client("root-status",
                                                [client] { return client; });
  Agent::Options options;
  options.model = Model{.id = "root-status-model",
                        .api = "root-status",
                        .provider = "root-status"};
  SessionRuntime root({.agent_options = options});
  TaskTree tree(root.agent(), default_child_factory(), options);
  TaskTreeShutdown shutdown(tree);
  ASSERT_TRUE(tree.get("root")->status == AgentTaskStatusKind::idle);

  // BlockingUsageClient returns at once on its first call; burn that one so
  // the next root turn blocks.
  root.agent().prompt("warm up").wait();
  // Stream completion precedes the worker's idle-state publication.
  root.agent().wait_for_idle();
  auto stream = root.agent().prompt("block");
  ASSERT_TRUE(pi::test::wait_until([&] { return entered->load(); },
                                   std::chrono::seconds(2),
                                   "root turn to block"));
  EXPECT_TRUE(tree.get("root")->status == AgentTaskStatusKind::running);
  release->store(true);
  stream.wait();
  ASSERT_TRUE(pi::test::wait_until(
      [&] { return tree.get("root")->status == AgentTaskStatusKind::idle; },
      std::chrono::seconds(2), "root to return to idle"));
  shutdown.shutdown();
}

TEST(AgentTasks, RejectsUnsupportedAllowSubagents) {
  auto options = fixed_reply_options("allow-subagents", "ok", {});
  SessionRuntime root({.agent_options = options});
  TaskTree tree(root.agent(), default_child_factory(), options);
  TaskTreeShutdown shutdown(tree);
  try {
    tree.spawn(
        {.task_name = "nested", .prompt = "go", .allow_subagents = true});
    ADD_FAILURE() << "allow_subagents is unsupported and must be rejected";
  } catch (const AgentTaskError &error) {
    EXPECT_TRUE(error.kind() == AgentTaskErrorKind::invalid_state);
  }
  EXPECT_TRUE(tree.list().size() == std::size_t{1}); // root only
  shutdown.shutdown();
}

TEST(AgentTasks, ChildLimitDiagnosticSaysCloseNotWaitFreesSlot) {
  auto options = fixed_reply_options("slot-guidance", "ok", {});
  SessionRuntime root({.agent_options = options});
  TaskTree::Limits limits;
  limits.max_direct_children = 1;
  TaskTree tree(root.agent(), default_child_factory(), options, limits);
  TaskTreeShutdown shutdown(tree);
  const auto first =
      wait_terminal(tree, tree.spawn({.task_name = "one", .prompt = "go"}));
  ASSERT_TRUE(first.status == AgentTaskStatusKind::completed);
  try {
    tree.spawn({.task_name = "two", .prompt = "go"});
    ADD_FAILURE() << "second direct child must exceed the cap";
  } catch (const AgentTaskError &error) {
    const std::string message = error.what();
    EXPECT_NE(message.find("close_agent"), std::string::npos);
    EXPECT_EQ(message.find("wait_agent or close_agent"), std::string::npos);
  }
  // Waiting does not release the slot; closing does.
  AgentWaitRequest wait_request;
  wait_request.targets = {first.id};
  wait_request.timeout = std::chrono::milliseconds(0);
  static_cast<void>(tree.wait(wait_request));
  EXPECT_THROW(tree.spawn({.task_name = "two", .prompt = "go"}),
               AgentTaskError);
  tree.close(first.id);
  EXPECT_NO_THROW(tree.spawn({.task_name = "two", .prompt = "go"}));
  shutdown.shutdown();
}

namespace {

ChildCompletion completion_for(std::string id, std::uint64_t generation,
                               std::string text = "done") {
  return ChildCompletion{.task_id = std::move(id),
                         .task_path = "/root/child",
                         .task_name = "child",
                         .run_generation = generation,
                         .text = std::move(text)};
}

std::string input_text(const AgentInput &input) {
  const auto *user = std::get_if<UserMessage>(&input.message);
  if (user == nullptr || user->content.empty())
    return {};
  const auto *text = std::get_if<TextContent>(&user->content.front());
  return text != nullptr ? text->text : std::string{};
}

struct EventLog {
  std::mutex mutex;
  std::vector<AgentTaskEvent> events;

  TaskTree::EventCallback callback() {
    return [this](const AgentTaskEvent &event) {
      std::scoped_lock lock(mutex);
      events.push_back(event);
    };
  }

  std::size_t completions() {
    std::scoped_lock lock(mutex);
    return static_cast<std::size_t>(std::ranges::count_if(
        events, [](const AgentTaskEvent &event) {
          return std::holds_alternative<AgentTaskCompletionEvent>(event);
        }));
  }
};

TaskTree::Limits delivering_limits() {
  TaskTree::Limits limits;
  limits.deliver_completions = true;
  return limits;
}

bool wait_pending(Agent &agent, std::size_t count) {
  return pi::test::wait_until(
      [&] { return agent.pending_child_completions() == count; },
      std::chrono::seconds(2), "pending child completions");
}

} // namespace

TEST(ChildCompletionInbox, DeduplicatesAndRemembersDrainedKeys) {
  ChildCompletionInbox inbox;
  using A = ChildCompletionInbox::Admission;
  EXPECT_TRUE(inbox.admit(completion_for("a", 1)) == A::admitted);
  EXPECT_TRUE(inbox.admit(completion_for("a", 1)) == A::duplicate);
  EXPECT_TRUE(inbox.admit(completion_for("a", 2)) == A::admitted);
  EXPECT_EQ(inbox.drain().completions.size(), std::size_t{2});
  // A late replay of an already-delivered key is still a duplicate.
  EXPECT_TRUE(inbox.admit(completion_for("a", 2)) == A::duplicate);
  EXPECT_EQ(inbox.pending(), std::size_t{0});
}

TEST(ChildCompletionInbox, BoundsPendingAndCountsDrops) {
  ChildCompletionInbox inbox;
  using A = ChildCompletionInbox::Admission;
  for (std::size_t i = 0; i < ChildCompletionInbox::kMaxPending; ++i)
    EXPECT_TRUE(inbox.admit(completion_for("t" + std::to_string(i), 1)) ==
                A::admitted);
  EXPECT_TRUE(inbox.admit(completion_for("extra", 1)) == A::overflow);
  EXPECT_TRUE(inbox.admit(completion_for("extra2", 1)) == A::overflow);
  const auto batch = inbox.drain();
  EXPECT_EQ(batch.completions.size(), ChildCompletionInbox::kMaxPending);
  EXPECT_EQ(batch.dropped, std::size_t{2});
  EXPECT_EQ(inbox.drain().dropped, std::size_t{0});
}

TEST(ChildCompletionInbox, CancelDropsPendingAndLateArrivals) {
  ChildCompletionInbox inbox;
  using A = ChildCompletionInbox::Admission;
  ASSERT_TRUE(inbox.admit(completion_for("a", 1)) == A::admitted);
  ASSERT_TRUE(inbox.admit(completion_for("b", 1)) == A::admitted);
  EXPECT_EQ(inbox.cancel("a"), std::size_t{1});
  EXPECT_TRUE(inbox.admit(completion_for("a", 2)) == A::cancelled);
  EXPECT_EQ(inbox.drain().completions.size(), std::size_t{1});
  inbox.close();
  EXPECT_TRUE(inbox.admit(completion_for("c", 1)) == A::closed);
  inbox.clear();
  EXPECT_TRUE(inbox.admit(completion_for("a", 1)) == A::admitted);
}

TEST(ChildCompletionInbox, TruncatesOnUtf8Boundary) {
  std::string text = "ab\xC3\xA9"
                     "cd"; // "abécd": é is two bytes at 2..3
  EXPECT_TRUE(truncate_utf8(text, 3));
  EXPECT_EQ(text, "ab");
  std::string short_text = "abc";
  EXPECT_FALSE(truncate_utf8(short_text, 3));
  EXPECT_EQ(short_text, "abc");
}

TEST(AgentChildCompletions, ClaimRendersAttributedInputAndDroppedNote) {
  Agent agent;
  ASSERT_TRUE(agent.admit_child_completion(completion_for("t1", 3, "result")));
  const auto claimed = agent.claim_child_completions();
  ASSERT_EQ(claimed.size(), std::size_t{1});
  const auto &input = claimed.front();
  EXPECT_TRUE(input.presentation.source ==
              InputProvenance::Source::child_completion);
  EXPECT_EQ(input.presentation.message_id.value_or(""), "t1#3");
  EXPECT_EQ(input.presentation.sender_task_path.value_or(""), "/root/child");
  EXPECT_NE(input_text(input).find("run_generation=3"), std::string::npos);
  EXPECT_NE(input_text(input).find("result"), std::string::npos);
  EXPECT_TRUE(agent.claim_child_completions().empty());

  for (std::size_t i = 0; i <= ChildCompletionInbox::kMaxPending; ++i)
    agent.admit_child_completion(completion_for("m" + std::to_string(i), 1));
  const auto overflowed = agent.claim_child_completions();
  ASSERT_EQ(overflowed.size(), ChildCompletionInbox::kMaxPending + 1);
  EXPECT_NE(input_text(overflowed.back()).find("dropped=1"),
            std::string::npos);
}

TEST(AgentChildCompletions, SurvivesUserInterruptButNotResetOrShutdown) {
  Agent agent;
  ASSERT_TRUE(agent.admit_child_completion(completion_for("a", 1)));
  agent.interrupt(TurnAbortReason::user_interrupt);
  EXPECT_EQ(agent.pending_child_completions(), std::size_t{1});
  agent.reset();
  EXPECT_EQ(agent.pending_child_completions(), std::size_t{0});
  ASSERT_TRUE(agent.admit_child_completion(completion_for("a", 1)));
  agent.interrupt(TurnAbortReason::shutdown);
  EXPECT_FALSE(agent.admit_child_completion(completion_for("b", 1)));
}

TEST(AgentTasks, CompletionDeliveryIsOffByDefault) {
  auto options = fixed_reply_options("completion-off", "child said hi", {});
  SessionRuntime root({.agent_options = options});
  EventLog log;
  TaskTree tree(root.agent(), default_child_factory(), options, {},
                log.callback());
  TaskTreeShutdown shutdown(tree);
  const auto done = wait_terminal(
      tree, tree.spawn({.task_name = "quiet", .prompt = "go"}));
  EXPECT_TRUE(done.status == AgentTaskStatusKind::completed);
  EXPECT_EQ(root.agent().pending_child_completions(), std::size_t{0});
  EXPECT_EQ(log.completions(), std::size_t{0});
  shutdown.shutdown();
}

TEST(AgentTasks, DeliversCompletionToIdleParentWithoutSplicing) {
  auto options = fixed_reply_options("completion-idle", "child said hi", {});
  SessionRuntime root({.agent_options = options});
  EventLog log;
  TaskTree tree(root.agent(), default_child_factory(), options,
                delivering_limits(), log.callback());
  TaskTreeShutdown shutdown(tree);
  const auto done = wait_terminal(
      tree, tree.spawn({.task_name = "worker", .prompt = "go"}));
  ASSERT_TRUE(done.status == AgentTaskStatusKind::completed);
  EXPECT_EQ(done.run_generation, std::uint64_t{1});
  ASSERT_TRUE(wait_pending(root.agent(), 1));
  EXPECT_EQ(log.completions(), std::size_t{1});
  // Admission alone never writes the parent transcript or starts a turn.
  EXPECT_EQ(root.agent().state().messages().size(), std::size_t{0});
  EXPECT_FALSE(root.agent().is_streaming());

  const auto claimed = root.agent().claim_child_completions();
  ASSERT_EQ(claimed.size(), std::size_t{1});
  const auto text = input_text(claimed.front());
  EXPECT_NE(text.find("task_id=" + done.id), std::string::npos);
  EXPECT_NE(text.find("run_generation=1"), std::string::npos);
  EXPECT_NE(text.find("child said hi"), std::string::npos);
  // No wake loop: a claim drains, and nothing re-admits the same run.
  EXPECT_TRUE(root.agent().claim_child_completions().empty());
  EXPECT_EQ(root.agent().state().messages().size(), std::size_t{0});
  shutdown.shutdown();
}

TEST(AgentTasks, CompletionContentIsBoundedAndRunsAreCorrelated) {
  auto options = fixed_reply_options("completion-bound", std::string(100, 'x'),
                                     {});
  SessionRuntime root({.agent_options = options});
  auto limits = delivering_limits();
  limits.max_completion_bytes = 10;
  TaskTree tree(root.agent(), default_child_factory(), options, limits);
  TaskTreeShutdown shutdown(tree);
  auto first = wait_terminal(
      tree, tree.spawn({.task_name = "big", .prompt = "go"}));
  ASSERT_TRUE(wait_pending(root.agent(), 1));
  const auto one = root.agent().claim_child_completions();
  ASSERT_EQ(one.size(), std::size_t{1});
  EXPECT_NE(input_text(one.front()).find("truncated=true"), std::string::npos);
  EXPECT_EQ(input_text(one.front()).find(std::string(11, 'x')),
            std::string::npos);
  // The full result stays readable through the lifecycle authority.
  EXPECT_EQ(tree.get(first.id)->result->text.size(), std::size_t{100});

  auto again = wait_terminal(
      tree, tree.follow_up(first.id, user_message("more")));
  EXPECT_EQ(again.run_generation, std::uint64_t{2});
  ASSERT_TRUE(wait_pending(root.agent(), 1));
  const auto two = root.agent().claim_child_completions();
  ASSERT_EQ(two.size(), std::size_t{1});
  EXPECT_EQ(two.front().presentation.message_id.value_or(""),
            first.id + "#2");
  shutdown.shutdown();
}

TEST(AgentTasks, ParentCausedInterruptIsNotDeliveredButUserInterruptIs) {
  auto calls = std::make_shared<std::atomic<int>>(0);
  LLMClientRegistry::instance().register_client("completion-int", [calls] {
    return std::make_shared<InterruptClient>(calls);
  });
  Agent::Options options;
  options.model = Model{.id = "completion-int-model",
                        .api = "completion-int",
                        .provider = "completion-int"};
  SessionRuntime root({.agent_options = options});
  TaskTree tree(root.agent(), default_child_factory(), options,
                delivering_limits());
  TaskTreeShutdown shutdown(tree);
  auto child = tree.spawn({.task_name = "slow", .prompt = "wait"});
  ASSERT_TRUE(pi::test::wait_until([&] { return calls->load() == 1; },
                                   std::chrono::seconds(2), "child to block"));
  auto interrupted = tree.interrupt(child.id, AgentInterruptReason::parent);
  auto settled = wait_terminal(tree, interrupted);
  EXPECT_TRUE(settled.status == AgentTaskStatusKind::interrupted);
  EXPECT_EQ(root.agent().pending_child_completions(), std::size_t{0});

  // The reused task now completes normally (second call returns "reused").
  auto reused = wait_terminal(
      tree, tree.follow_up(child.id, user_message("again")));
  EXPECT_TRUE(reused.status == AgentTaskStatusKind::completed);
  ASSERT_TRUE(wait_pending(root.agent(), 1));
  const auto claimed = root.agent().claim_child_completions();
  EXPECT_NE(input_text(claimed.front()).find("run_generation=2"),
            std::string::npos);
  shutdown.shutdown();
}

TEST(AgentTasks, UserInterruptedChildIsDelivered) {
  auto calls = std::make_shared<std::atomic<int>>(0);
  LLMClientRegistry::instance().register_client("completion-user", [calls] {
    return std::make_shared<InterruptClient>(calls);
  });
  Agent::Options options;
  options.model = Model{.id = "completion-user-model",
                        .api = "completion-user",
                        .provider = "completion-user"};
  SessionRuntime root({.agent_options = options});
  TaskTree tree(root.agent(), default_child_factory(), options,
                delivering_limits());
  TaskTreeShutdown shutdown(tree);
  auto child = tree.spawn({.task_name = "slow", .prompt = "wait"});
  ASSERT_TRUE(pi::test::wait_until([&] { return calls->load() == 1; },
                                   std::chrono::seconds(2), "child to block"));
  wait_terminal(tree, tree.interrupt(child.id, AgentInterruptReason::user));
  ASSERT_TRUE(wait_pending(root.agent(), 1));
  const auto claimed = root.agent().claim_child_completions();
  EXPECT_EQ(claimed.front().presentation.message_kind.value_or(""),
            "interrupted");
  shutdown.shutdown();
}

TEST(AgentTasks, CloseCancelsPendingAndShutdownDeliversNothing) {
  auto options = fixed_reply_options("completion-close", "ok", {});
  SessionRuntime root({.agent_options = options});
  TaskTree tree(root.agent(), default_child_factory(), options,
                delivering_limits());
  TaskTreeShutdown shutdown(tree);
  auto done = wait_terminal(
      tree, tree.spawn({.task_name = "closer", .prompt = "go"}));
  ASSERT_TRUE(wait_pending(root.agent(), 1));
  tree.close(done.id);
  EXPECT_EQ(root.agent().pending_child_completions(), std::size_t{0});
  // A late arrival for the closed task is dropped.
  EXPECT_FALSE(root.agent().admit_child_completion(
      completion_for(done.id, done.run_generation + 1)));

  auto second = wait_terminal(
      tree, tree.spawn({.task_name = "survivor", .prompt = "go"}));
  ASSERT_TRUE(wait_pending(root.agent(), 1));
  shutdown.shutdown();
  // Shutdown closes every child without producing completions for them.
  EXPECT_EQ(root.agent().pending_child_completions(), std::size_t{0});
  // A shutting-down parent admits nothing further.
  root.agent().interrupt(TurnAbortReason::shutdown);
  EXPECT_FALSE(root.agent().admit_child_completion(completion_for("late", 1)));
  EXPECT_TRUE(second.status == AgentTaskStatusKind::completed);
}

TEST(AgentTasks, BusyParentReceivesCompletionAtTurnBoundary) {
  auto entered = std::make_shared<std::atomic<bool>>(false);
  auto release = std::make_shared<std::atomic<bool>>(false);
  auto client = std::make_shared<BlockingUsageClient>(entered, release);
  LLMClientRegistry::instance().register_client("completion-busy",
                                                [client] { return client; });
  Agent::Options options;
  options.model = Model{.id = "completion-busy-model",
                        .api = "completion-busy",
                        .provider = "completion-busy"};
  SessionRuntime root({.agent_options = options});
  root.agent().prompt("warm up").wait();
  root.agent().wait_for_idle();
  auto stream = root.agent().prompt("block");
  ASSERT_TRUE(pi::test::wait_until([&] { return entered->load(); },
                                   std::chrono::seconds(2),
                                   "root turn to block"));
  ASSERT_TRUE(root.agent().admit_child_completion(
      completion_for("c1", 1, "distinctive-child-result")));
  release->store(true);
  stream.wait();
  root.agent().wait_for_idle();
  EXPECT_EQ(root.agent().pending_child_completions(), std::size_t{0});
  std::size_t attributed = 0;
  for (const auto &message : root.agent().state().messages()) {
    const auto *user = std::get_if<UserMessage>(&message);
    if (user == nullptr || user->content.empty())
      continue;
    const auto *text = std::get_if<TextContent>(&user->content.front());
    if (text != nullptr &&
        text->text.find("[pici child task completion]") == 0 &&
        text->text.find("distinctive-child-result") != std::string::npos)
      ++attributed;
  }
  EXPECT_EQ(attributed, std::size_t{1});
}

TEST(AgentTasks, RootMessageIsRejectedInsteadOfQueuedInUnconsumedMailbox) {
  auto options = fixed_reply_options("completion-root-msg", "ok", {});
  SessionRuntime root({.agent_options = options});
  TaskTree tree(root.agent(), default_child_factory(), options);
  TaskTreeShutdown shutdown(tree);
  try {
    tree.send_message("root", Message{user_message("hello")});
    ADD_FAILURE() << "send_message on the root pseudo-task must be rejected";
  } catch (const AgentTaskError &error) {
    EXPECT_TRUE(error.kind() == AgentTaskErrorKind::invalid_state);
  }
  EXPECT_EQ(tree.get("root")->queued_message_count, std::size_t{0});
  shutdown.shutdown();
}

TEST(AgentTasks, SerializesCompletionEvent) {
  auto completion = completion_for("t9", 4, "payload");
  completion.truncated = true;
  completion.error = "boom";
  const auto json = task_event_to_json(
      AgentTaskCompletionEvent{.completion = std::move(completion)});
  EXPECT_EQ(json.at("type"), "task.completion");
  EXPECT_EQ(json.at("task_id"), "t9");
  EXPECT_EQ(json.at("run_generation"), 4);
  EXPECT_EQ(json.at("status"), "completed");
  EXPECT_EQ(json.at("text"), "payload");
  EXPECT_EQ(json.at("error"), "boom");
  EXPECT_EQ(json.at("truncated"), true);
  EXPECT_EQ(task_event_to_json(AgentTaskClosedEvent{.id = "x"}).at("type"),
            "task.closed");
}
