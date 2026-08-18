#include "core/agent_task.h"
#include "core/agent_loop.h"

#include "core/agent.h"
#include "core/agent_runtime_identity.h"
#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/session/agent_session.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace pi::core {

namespace {

std::string message_text(const Message &message) {
  return std::visit(
      []<typename T>(const T &value) {
        std::string result;
        if constexpr (std::same_as<T, ContextCompactionMessage>) {
          // Opaque server-side content; never surfaced as plain text.
        } else {
          for (const auto &block : value.content) {
            if (const auto *text = std::get_if<TextContent>(&block))
              result += text->text;
          }
        }
        return result;
      },
      message);
}

std::string assistant_text(const AssistantMessage &message) {
  std::string result;
  for (const auto &block : message.content) {
    if (const auto *text = std::get_if<TextContent>(&block))
      result += text->text;
  }
  return result;
}

bool is_child_safe_tool(const std::shared_ptr<const ToolDefinition> &tool) {
  return tool && tool->capabilities().child_safe;
}

std::size_t message_bytes(const Message &message) {
  return json::to_json(message).size();
}

AgentMessageEnvelope message_envelope(Message message) {
  return AgentMessageEnvelope{.message = std::move(message)};
}

std::vector<Message> normalize_context(std::vector<Message> messages,
                                       std::size_t max_bytes) {
  std::set<std::string> calls;
  std::vector<Message> result;
  std::size_t bytes = 0;

  for (auto &message : messages) {
    if (const auto *tool_result = std::get_if<ToolResultMessage>(&message)) {
      if (!calls.contains(tool_result->tool_call_id))
        continue;
    }
    if (const auto *assistant = std::get_if<AssistantMessage>(&message)) {
      for (const auto &block : assistant->content) {
        if (const auto *call = std::get_if<ToolCall>(&block))
          calls.insert(call->id);
      }
    }

    const auto size = message_bytes(message);
    if (bytes + size > max_bytes)
      break;
    bytes += size;
    result.push_back(std::move(message));
  }

  // An assistant tool-call message without all of its results is not a valid
  // provider context. Drop that incomplete final exchange.
  while (!result.empty()) {
    const auto *assistant = std::get_if<AssistantMessage>(&result.back());
    if (assistant == nullptr)
      break;
    std::set<std::string> ids;
    for (const auto &block : assistant->content)
      if (const auto *call = std::get_if<ToolCall>(&block))
        ids.insert(call->id);
    if (ids.empty())
      break;
    std::set<std::string> results;
    for (std::size_t i = result.size(); i-- > 0;) {
      if (const auto *tool = std::get_if<ToolResultMessage>(&result[i]))
        results.insert(tool->tool_call_id);
      if (std::holds_alternative<UserMessage>(result[i]))
        break;
    }
    if (std::ranges::all_of(
            ids, [&results](const auto &id) { return results.contains(id); }))
      break;
    result.pop_back();
  }
  return result;
}

} // namespace

std::string_view agent_task_status_to_string(AgentTaskStatusKind status) {
  switch (status) {
  case AgentTaskStatusKind::pending_init:
    return "pending_init";
  case AgentTaskStatusKind::idle:
    return "idle";
  case AgentTaskStatusKind::running:
    return "running";
  case AgentTaskStatusKind::completed:
    return "completed";
  case AgentTaskStatusKind::errored:
    return "errored";
  case AgentTaskStatusKind::interrupted:
    return "interrupted";
  case AgentTaskStatusKind::closing:
    return "closing";
  case AgentTaskStatusKind::shutdown:
    return "shutdown";
  }
  return "shutdown";
}

std::string_view agent_interrupt_reason_to_string(AgentInterruptReason reason) {
  switch (reason) {
  case AgentInterruptReason::user:
    return "user";
  case AgentInterruptReason::parent:
    return "parent";
  case AgentInterruptReason::replacement_task:
    return "replacement_task";
  case AgentInterruptReason::shutdown:
    return "shutdown";
  case AgentInterruptReason::budget:
    return "budget";
  case AgentInterruptReason::timeout:
    return "timeout";
  }
  return "user";
}

TurnAbortReason turn_abort_reason(AgentInterruptReason reason) {
  switch (reason) {
  case AgentInterruptReason::user:
    return TurnAbortReason::user_interrupt;
  case AgentInterruptReason::parent:
    return TurnAbortReason::parent_interrupt;
  case AgentInterruptReason::replacement_task:
    return TurnAbortReason::replacement_task;
  case AgentInterruptReason::shutdown:
    return TurnAbortReason::shutdown;
  case AgentInterruptReason::budget:
    return TurnAbortReason::budget;
  case AgentInterruptReason::timeout:
    return TurnAbortReason::timeout;
  }
  return TurnAbortReason::unknown;
}

std::string_view agent_task_error_code(AgentTaskErrorKind kind) {
  switch (kind) {
  case AgentTaskErrorKind::not_found:
    return "not_found";
  case AgentTaskErrorKind::invalid_state:
    return "invalid_state";
  case AgentTaskErrorKind::duplicate_name:
    return "duplicate_name";
  case AgentTaskErrorKind::depth_limit:
    return "depth_limit";
  case AgentTaskErrorKind::execution_limit:
    return "execution_limit";
  case AgentTaskErrorKind::residency_limit:
    return "residency_limit";
  case AgentTaskErrorKind::invalid_context:
    return "invalid_context";
  case AgentTaskErrorKind::invalid_model:
    return "invalid_model";
  case AgentTaskErrorKind::invalid_tool:
    return "invalid_tool";
  case AgentTaskErrorKind::permission_denied:
    return "permission_denied";
  case AgentTaskErrorKind::shutting_down:
    return "shutting_down";
  case AgentTaskErrorKind::internal:
    return "internal";
  }
  return "internal";
}

struct AgentTaskManager::Task {
  AgentTaskId id;
  std::string task_path;
  std::optional<AgentTaskId> parent_id;
  std::string task_name;
  std::size_t depth{0};
  AgentSession *session{nullptr};
  std::unique_ptr<AgentSession> owned_session;
  std::set<AgentTaskId> children;

  mutable std::mutex mutex;
  std::condition_variable_any changed;
  AgentTaskStatusKind status{AgentTaskStatusKind::pending_init};
  std::optional<AgentTaskResult> result;
  std::optional<AgentRuntimeIdentity> runtime_identity;
  std::deque<AgentTaskManager::WorkItem> work;
  std::deque<Message> mailbox;
  std::optional<AgentInterruptReason> pending_interrupt;
  std::uint64_t generation{0};
  bool execution_reserved{false};
  bool close_requested{false};
  std::jthread runner;
};

AgentTaskManager::AgentTaskManager(AgentSession &root,
                                   Agent::Options child_options)
    : AgentTaskManager(root, std::move(child_options), Limits{}, {}) {}

AgentTaskManager::AgentTaskManager(AgentSession &root,
                                   Agent::Options child_options, Limits limits,
                                   EventCallback on_event)
    : root_(root), child_options_(std::move(child_options)), limits_(limits),
      on_event_(std::move(on_event)) {
  if (limits_.max_active_executions == 0 || limits_.max_resident_tasks == 0)
    throw AgentTaskError(AgentTaskErrorKind::internal,
                         "task limits must allow at least one task");

  auto root_task = std::make_shared<Task>();
  root_task->id = "root";
  root_task->task_path = "/root";
  root_task->task_name = "root";
  root_task->session = &root_;
  root_task->status = root_.agent().is_streaming()
                          ? AgentTaskStatusKind::running
                          : AgentTaskStatusKind::idle;
  tasks_.emplace(root_task->id, std::move(root_task));
}

AgentTaskManager::~AgentTaskManager() noexcept {
  try {
    shutdown();
  } catch (...) {
    std::terminate();
  }
}

void AgentTaskManager::set_endpoint_registration(
    RegisterEndpointCallback register_endpoint,
    UnregisterEndpointCallback unregister_endpoint) {
  std::scoped_lock lock(mutex_);
  if (!tasks_.empty() && tasks_.size() > 1)
    throw AgentTaskError(
        AgentTaskErrorKind::invalid_state,
        "endpoint registration must be configured before spawning children");
  register_endpoint_ = std::move(register_endpoint);
  unregister_endpoint_ = std::move(unregister_endpoint);
}

std::shared_ptr<AgentTaskManager::Task>
AgentTaskManager::find_task_locked(const AgentTaskId &target) const {
  if (auto it = tasks_.find(target); it != tasks_.end())
    return it->second;
  for (const auto &[id, task] : tasks_) {
    if (task->task_path == target)
      return task;
  }
  return nullptr;
}

AgentTaskSnapshot
AgentTaskManager::snapshot(const std::shared_ptr<Task> &task) {
  std::scoped_lock lock(task->mutex);
  AgentTaskSnapshot result;
  result.id = task->id;
  result.task_path = task->task_path;
  result.parent_id = task->parent_id;
  result.task_name = task->task_name;
  result.status = task->status;
  result.result = task->result;
  result.child_count = task->children.size();
  result.queued_message_count = task->mailbox.size() + task->work.size();
  result.generation = task->generation;
  return result;
}

void AgentTaskManager::touch_locked(const std::shared_ptr<Task> &task) {
  task->generation = ++generation_;
  task->changed.notify_all();
  changed_.notify_all();
}

void AgentTaskManager::emit(const AgentTaskEvent &event) const {
  if (!on_event_)
    return;
  try {
    on_event_(event);
  } catch (...) {
    // Observers are not allowed to affect lifecycle ownership.
    static_cast<void>(0);
  }
}

bool AgentTaskManager::valid_task_name(std::string_view name) {
  if (name.empty() || name.size() > 64)
    return false;
  if (std::isalnum(static_cast<unsigned char>(name.front())) == 0 &&
      name.front() != '_' && name.front() != '-')
    return false;
  return std::ranges::all_of(name, [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' ||
           c == '-' || c == '.';
  });
}

std::vector<std::shared_ptr<const ToolDefinition>>
AgentTaskManager::inherit_tools(const AgentContext &parent,
                                const std::vector<std::string> &requested) {
  std::vector<std::shared_ptr<const ToolDefinition>> safe;
  for (const auto &tool : parent.tools)
    if (is_child_safe_tool(tool))
      safe.push_back(tool);

  if (requested.empty())
    return safe;

  std::vector<std::shared_ptr<const ToolDefinition>> result;
  for (const auto &name : requested) {
    auto it = std::ranges::find_if(parent.tools, [&name](const auto &tool) {
      return tool && tool->name() == name;
    });
    if (it == parent.tools.end() || !is_child_safe_tool(*it))
      throw AgentTaskError(AgentTaskErrorKind::invalid_tool,
                           "tool is not available to child agents: " + name);
    if (std::ranges::none_of(
            result, [&name](const auto &tool) { return tool->name() == name; }))
      result.push_back(*it);
  }
  return result;
}

std::vector<Message>
AgentTaskManager::inherit_context(const AgentContext &parent,
                                  const ContextInheritance &request) const {
  std::vector<Message> messages;
  switch (request.mode) {
  case ContextInheritanceMode::none:
    break;
  case ContextInheritanceMode::full:
    messages = parent.messages;
    break;
  case ContextInheritanceMode::through_message: {
    const auto count =
        std::min(request.through.value_or(parent.messages.size()),
                 parent.messages.size());
    const auto offset =
        static_cast<std::vector<Message>::difference_type>(count);
    messages.assign(parent.messages.begin(), parent.messages.begin() + offset);
    break;
  }
  case ContextInheritanceMode::recent_messages: {
    const auto count = request.recent_count.value_or(16);
    const auto begin =
        parent.messages.size() - std::min(count, parent.messages.size());
    const auto offset =
        static_cast<std::vector<Message>::difference_type>(begin);
    messages.assign(parent.messages.begin() + offset, parent.messages.end());
    break;
  }
  }
  return normalize_context(std::move(messages), limits_.max_context_bytes);
}

std::shared_ptr<AgentTaskManager::Task> AgentTaskManager::make_task(
    const SpawnAgentRequest &request, const std::shared_ptr<Task> &parent,
    std::vector<Message> context, AgentTaskId task_id, std::string task_path,
    std::optional<AgentRuntimeIdentity> identity) {
  const auto parent_context = parent->session->agent().context_snapshot();
  auto options = child_options_;
  options.system_prompt =
      request.system_prompt.value_or(parent_context.system_prompt);
  options.model = parent_context.model;

  if (request.model_spec) {
    std::optional<Model> model;
    if (options.model_registry) {
      ModelSelection selection;
      if (!request.model_spec->contains('/'))
        selection.provider = parent_context.model.provider;
      selection.model = *request.model_spec;
      selection.source = "child";
      auto resolution = options.model_registry->resolve(selection);
      if (resolution)
        model = std::move(resolution.model);
    } else {
      model = find_model(*request.model_spec, parent_context.model.provider);
    }
    if (!model)
      throw AgentTaskError(AgentTaskErrorKind::invalid_model,
                           "unable to resolve child model: " +
                               *request.model_spec);
    options.model = std::move(*model);
  }

  // Children are restricted to C++ read-only tools in the in-memory MVP. This
  // lets us remove Lua hooks without silently bypassing their authority.
  options.before_tool_call = nullptr;
  options.after_tool_call = nullptr;
  options.should_stop_after_turn = nullptr;
  options.on_effective_context = nullptr;
  options.on_event = nullptr;
  options.prepare_context = nullptr;
  options.transform_context = nullptr;
  options.get_steering_messages = nullptr;
  options.get_follow_up_messages = nullptr;
  options.runtime_identity = identity;

  auto task = std::make_shared<Task>();
  task->id = std::move(task_id);
  task->task_path = std::move(task_path);
  task->parent_id = parent->id;
  task->task_name = request.task_name;
  task->depth = parent->depth + 1;
  task->owned_session = std::make_unique<AgentSession>(AgentSession::Config{
      .agent_options = std::move(options),
      .tools = inherit_tools(parent_context, request.requested_tools),
      .session_store = nullptr,
  });
  task->session = task->owned_session.get();
  task->runtime_identity = std::move(identity);
  task->session->agent().state().set_messages(std::move(context));
  UserMessage message;
  message.content.emplace_back(TextContent{.text = request.prompt});
  task->work.push_back(
      WorkItem{.messages = std::vector<AgentMessageEnvelope>{
                   message_envelope(Message{std::move(message)})}});
  task->execution_reserved = true;
  return task;
}

AgentTaskSnapshot AgentTaskManager::spawn(const SpawnAgentRequest &request) {
  if (!valid_task_name(request.task_name))
    throw AgentTaskError(
        AgentTaskErrorKind::invalid_context,
        "task_name must contain only letters, numbers, '_', '-', or '.'");
  if (request.prompt.empty())
    throw AgentTaskError(AgentTaskErrorKind::invalid_context,
                         "spawn prompt must not be empty");

  AgentTaskSnapshot result;
  std::shared_ptr<Task> parent;
  AgentTaskId task_id;
  std::string task_path;
  std::vector<Message> context;
  RegisterEndpointCallback register_endpoint;
  UnregisterEndpointCallback unregister_endpoint;
  {
    std::scoped_lock lock(mutex_);
    if (shutting_down_)
      throw AgentTaskError(AgentTaskErrorKind::shutting_down,
                           "agent task manager is shutting down");
    parent = find_task_locked(request.parent_id.empty() ? "root"
                                                        : request.parent_id);
    if (!parent)
      throw AgentTaskError(AgentTaskErrorKind::not_found,
                           "parent task not found");
    {
      std::scoped_lock parent_lock(parent->mutex);
      if (parent->status == AgentTaskStatusKind::closing ||
          parent->status == AgentTaskStatusKind::shutdown)
        throw AgentTaskError(AgentTaskErrorKind::invalid_state,
                             "parent task is closed");
      const auto pending_children_it = pending_children_.find(parent->id);
      const auto pending_children =
          pending_children_it == pending_children_.end()
              ? std::size_t{0}
              : pending_children_it->second;
      if (parent->children.size() + pending_children >=
          limits_.max_direct_children)
        throw AgentTaskError(AgentTaskErrorKind::residency_limit,
                             "parent child limit reached");
      for (const auto &child_id : parent->children) {
        const auto child = tasks_.at(child_id);
        if (child->task_name == request.task_name)
          throw AgentTaskError(AgentTaskErrorKind::duplicate_name,
                               "sibling task name already exists");
      }
      if (parent->depth + 1 > limits_.max_nesting_depth)
        throw AgentTaskError(AgentTaskErrorKind::depth_limit,
                             "agent nesting depth limit reached");
    }
    if (tasks_.size() - 1 + pending_spawns_ >= limits_.max_resident_tasks)
      throw AgentTaskError(AgentTaskErrorKind::residency_limit,
                           "resident child task limit reached");
    if (active_executions_ >= limits_.max_active_executions)
      throw AgentTaskError(AgentTaskErrorKind::execution_limit,
                           "active child execution limit reached");

    task_id = "agent_" + std::to_string(next_id_++);
    task_path = parent->task_path + "/" + request.task_name;
    if (tasks_.contains(task_id) || pending_task_paths_.contains(task_path) ||
        std::ranges::any_of(tasks_, [&task_path](const auto &entry) {
          return entry.second->task_path == task_path;
        }))
      throw AgentTaskError(AgentTaskErrorKind::duplicate_name,
                           "task identity already exists");
    auto parent_context = parent->session->agent().context_snapshot();
    context = inherit_context(parent_context, request.context);
    register_endpoint = register_endpoint_;
    unregister_endpoint = unregister_endpoint_;
    pending_task_paths_.insert(task_path);
    ++pending_children_[parent->id];
    ++pending_spawns_;
    ++active_executions_;
  }

  std::optional<AgentRuntimeIdentity> identity;
  bool endpoint_registered = false;
  std::shared_ptr<Task> task;
  auto release_reservation = [&](bool release_execution, bool release_path,
                                 bool release_child) {
    std::scoped_lock lock(mutex_);
    if (release_path)
      pending_task_paths_.erase(task_path);
    if (release_child) {
      if (auto pending = pending_children_.find(parent->id);
          pending != pending_children_.end()) {
        if (pending->second > 0)
          --pending->second;
        if (pending->second == 0)
          pending_children_.erase(pending);
      }
    }
    if (release_execution && active_executions_ > 0)
      --active_executions_;
    if (pending_spawns_ > 0)
      --pending_spawns_;
    changed_.notify_all();
  };
  try {
    if (register_endpoint) {
      identity = register_endpoint(task_id, task_path, parent->id);
      endpoint_registered = true;
    }
    task = make_task(request, parent, std::move(context), task_id, task_path,
                     std::move(identity));
  } catch (...) {
    if (unregister_endpoint && endpoint_registered) {
      try {
        unregister_endpoint(task_id);
      } catch (...) {
        static_cast<void>(0);
      }
    }
    release_reservation(true, true, true);
    throw;
  }

  bool registration_cancelled = false;
  bool registration_shutdown = false;
  bool runner_failed = false;
  std::exception_ptr runner_error;
  bool published = false;
  {
    std::scoped_lock lock(mutex_);
    const bool parent_gone = !tasks_.contains(parent->id);
    bool parent_closing = false;
    if (!parent_gone) {
      std::scoped_lock parent_lock(parent->mutex);
      parent_closing = parent->close_requested ||
                       parent->status == AgentTaskStatusKind::closing ||
                       parent->status == AgentTaskStatusKind::shutdown;
    }
    if (shutting_down_ || parent_gone || parent_closing) {
      registration_shutdown = shutting_down_;
      registration_cancelled = true;
    } else {
      try {
        task->runner =
            std::jthread([this, task](const std::stop_token &stop_token) {
              run_task(task, stop_token);
            });
        pending_task_paths_.erase(task_path);
        if (auto pending = pending_children_.find(parent->id);
            pending != pending_children_.end()) {
          if (pending->second > 0)
            --pending->second;
          if (pending->second == 0)
            pending_children_.erase(pending);
        }
        tasks_.emplace(task->id, task);
        parent->children.insert(task->id);
        task->execution_reserved = true;
        touch_locked(task);
        result = snapshot(task);
        published = true;
      } catch (...) {
        tasks_.erase(task->id);
        parent->children.erase(task->id);
        runner_failed = true;
        runner_error = std::current_exception();
      }
    }
  }

  if (registration_cancelled || runner_failed) {
    if (runner_failed && task->runner.joinable()) {
      task->runner.request_stop();
      task->session->agent().interrupt(TurnAbortReason::shutdown);
      task->runner.join();
    }
    if (unregister_endpoint && endpoint_registered) {
      try {
        unregister_endpoint(task_id);
      } catch (...) {
        static_cast<void>(0);
      }
    }
    release_reservation(true, true, true);
    if (runner_failed)
      throw AgentTaskError(AgentTaskErrorKind::internal,
                           "failed to start child task runner");
    throw AgentTaskError(
        registration_shutdown ? AgentTaskErrorKind::shutting_down
                              : AgentTaskErrorKind::invalid_state,
        registration_shutdown ? "agent task manager is shutting down"
                              : "parent task is closed");
  }

  if (published) {
    emit(AgentTaskSpawnedEvent{.id = task->id,
                               .task_path = task->task_path,
                               .parent_id = task->parent_id,
                               .task_name = task->task_name});
    release_reservation(false, false, false);
  }
  return result;
}

void AgentTaskManager::execute_work(const std::shared_ptr<Task> &task,
                                    std::vector<AgentMessageEnvelope> messages,
                                    AgentTaskResult &result, bool &aborted) {
  std::size_t output_bytes = 0;
  bool saw_text_delta = false;
  std::optional<AssistantMessage> final_message;
  auto callback = [this, task, &result, &aborted, &output_bytes,
                   &saw_text_delta, &final_message](const AgentEvent &event) {
    emit(ChildAgentEvent{.task_id = task->id, .event = event});
    if (std::holds_alternative<TurnAbortedEvent>(event)) {
      aborted = true;
      return;
    }
    if (const auto *update = std::get_if<MessageUpdateEvent>(&event)) {
      if (const auto *delta = std::get_if<AssistantMessageTextDeltaEvent>(
              &update->assistant_message_event)) {
        saw_text_delta = true;
        if (output_bytes < limits_.max_output_bytes) {
          const auto remaining = limits_.max_output_bytes - output_bytes;
          result.text.append(delta->delta.data(),
                             std::min(remaining, delta->delta.size()));
          output_bytes += std::min(remaining, delta->delta.size());
          if (delta->delta.size() > remaining)
            result.truncated = true;
        } else {
          result.truncated = true;
        }
      }
    } else if (const auto *end = std::get_if<MessageEndEvent>(&event)) {
      if (const auto *assistant = std::get_if<AssistantMessage>(&end->message))
        final_message = *assistant;
    }
  };

  const auto run = task->session->run_messages(std::move(messages), callback);
  if (!saw_text_delta && final_message)
    result.text = final_message->content.empty()
                      ? std::string{}
                      : assistant_text(*final_message);
  if (final_message) {
    result.stop_reason = final_message->stop_reason;
    result.usage = final_message->usage;
    if (final_message->error_message && !aborted)
      result.error = final_message->error_message;
  }
  if (run.error && !aborted)
    result.error = run.error;
  if (aborted)
    result.stop_reason = StopReason::aborted;
}

void AgentTaskManager::run_task(const std::shared_ptr<Task> &task,
                                const std::stop_token &stop_token) {
  std::stop_callback cancel_callback(stop_token, [task] {
    if (task->session)
      task->session->agent().interrupt(TurnAbortReason::shutdown);
  });

  for (;;) {
    WorkItem work;
    {
      std::unique_lock lock(task->mutex);
      task->changed.wait(lock, stop_token, [&] {
        return task->close_requested || !task->work.empty();
      });
      if (task->close_requested || stop_token.stop_requested())
        break;
      work = std::move(task->work.front());
      task->work.pop_front();
    }

    bool reserved = false;
    for (;;) {
      bool should_requeue = false;
      AgentTaskStatusKind previous = AgentTaskStatusKind::pending_init;
      {
        std::scoped_lock lock(mutex_, task->mutex);
        if (shutting_down_ || task->close_requested) {
          // Keep a local copy because the reservation loop exits without
          // executing this item when the task is closing or shutting down.
          task->work.push_front(work);
          should_requeue = true;
        } else if (task->execution_reserved ||
                   active_executions_ < limits_.max_active_executions) {
          if (!task->execution_reserved) {
            ++active_executions_;
            task->execution_reserved = true;
          }
          reserved = true;
          previous = task->status;
          task->status = AgentTaskStatusKind::running;
          touch_locked(task);
        }
      }
      if (should_requeue || reserved) {
        if (reserved && previous != AgentTaskStatusKind::running)
          emit(AgentTaskStatusChangedEvent{.id = task->id,
                                           .previous = previous,
                                           .current =
                                               AgentTaskStatusKind::running});
        break;
      }
      std::unique_lock lock(mutex_);
      changed_.wait_for(lock, std::chrono::milliseconds(25));
    }
    if (!reserved)
      break;

    AgentTaskResult result;
    bool aborted = false;
    execute_work(task, std::move(work.messages), result, aborted);

    AgentTaskStatusKind previous{AgentTaskStatusKind::pending_init};
    AgentTaskStatusKind current{AgentTaskStatusKind::pending_init};
    bool close_requested = false;
    {
      std::scoped_lock lock(mutex_, task->mutex);
      if (active_executions_ > 0)
        --active_executions_;
      task->execution_reserved = false;
      previous = task->status;
      task->result = std::move(result);
      close_requested = task->close_requested;
      if (close_requested)
        current = AgentTaskStatusKind::closing;
      else if (aborted)
        current = AgentTaskStatusKind::interrupted;
      else if (task->result && task->result->error)
        current = AgentTaskStatusKind::errored;
      else
        current = AgentTaskStatusKind::completed;
      task->status = current;
      touch_locked(task);
    }
    emit(AgentTaskStatusChangedEvent{
        .id = task->id, .previous = previous, .current = current});

    if (close_requested || stop_token.stop_requested())
      break;
  }

  AgentTaskStatusKind previous = AgentTaskStatusKind::shutdown;
  bool emit_closing = false;
  {
    std::scoped_lock lock(mutex_, task->mutex);
    if (task->status != AgentTaskStatusKind::shutdown &&
        task->status != AgentTaskStatusKind::closing) {
      previous = task->status;
      task->status = AgentTaskStatusKind::closing;
      touch_locked(task);
      emit_closing = true;
    }
  }
  if (emit_closing)
    emit(AgentTaskStatusChangedEvent{.id = task->id,
                                     .previous = previous,
                                     .current = AgentTaskStatusKind::closing});
}

std::optional<AgentTaskSnapshot>
AgentTaskManager::get(const AgentTaskId &target) const {
  std::scoped_lock lock(mutex_);
  auto task = find_task_locked(target);
  if (!task)
    return std::nullopt;
  return snapshot(task);
}

std::vector<AgentTaskSnapshot>
AgentTaskManager::list(std::optional<std::string_view> task_path_prefix) const {
  std::vector<AgentTaskSnapshot> result;
  std::scoped_lock lock(mutex_);
  for (const auto &[id, task] : tasks_) {
    if (task_path_prefix &&
        !std::string_view(task->task_path).starts_with(*task_path_prefix))
      continue;
    result.push_back(snapshot(task));
  }
  std::ranges::sort(result, {}, &AgentTaskSnapshot::task_path);
  return result;
}

AgentTaskSnapshot AgentTaskManager::send_message(const AgentTaskId &target,
                                                 Message message) {
  if (message_bytes(message) > limits_.max_message_bytes)
    throw AgentTaskError(AgentTaskErrorKind::residency_limit,
                         "task message exceeds size limit");
  std::shared_ptr<Task> task;
  {
    std::scoped_lock lock(mutex_);
    task = find_task_locked(target);
    if (!task)
      throw AgentTaskError(AgentTaskErrorKind::not_found, "task not found");
    std::scoped_lock task_lock(task->mutex);
    if (task->status == AgentTaskStatusKind::closing ||
        task->status == AgentTaskStatusKind::shutdown)
      throw AgentTaskError(AgentTaskErrorKind::invalid_state, "task is closed");
    if (task->mailbox.size() >= limits_.max_mailbox_items)
      throw AgentTaskError(AgentTaskErrorKind::residency_limit,
                           "task mailbox limit reached");
    task->mailbox.push_back(std::move(message));
    touch_locked(task);
  }
  task->changed.notify_all();
  emit(AgentTaskMessageQueuedEvent{.id = task->id, .triggers_turn = false});
  return snapshot(task);
}

AgentTaskSnapshot
AgentTaskManager::steer_envelopes(const AgentTaskId &target,
                                  std::vector<AgentMessageEnvelope> messages) {
  if (messages.empty())
    throw AgentTaskError(AgentTaskErrorKind::invalid_context,
                         "steering envelope list must not be empty");
  for (const auto &envelope : messages)
    if (message_bytes(envelope.message) > limits_.max_message_bytes)
      throw AgentTaskError(AgentTaskErrorKind::residency_limit,
                           "task message exceeds size limit");

  std::shared_ptr<Task> task;
  bool steer_running = false;
  AgentTaskStatusKind previous_status = AgentTaskStatusKind::running;
  std::optional<AgentTaskResult> previous_result;
  std::vector<AgentMessageEnvelope> root_messages;
  std::vector<AgentMessageEnvelope> running_messages;
  {
    std::scoped_lock lock(mutex_);
    task = find_task_locked(target);
    if (!task)
      throw AgentTaskError(AgentTaskErrorKind::not_found, "task not found");
    if (task->id == "root") {
      if (shutting_down_)
        throw AgentTaskError(AgentTaskErrorKind::shutting_down,
                             "agent task manager is shutting down");
      steer_running = true;
      root_messages = std::move(messages);
    } else {
      std::scoped_lock task_lock(task->mutex);
      if (task->status == AgentTaskStatusKind::closing ||
          task->status == AgentTaskStatusKind::shutdown ||
          task->close_requested)
        throw AgentTaskError(AgentTaskErrorKind::invalid_state,
                             "task is closed");
      if (task->work.size() + task->mailbox.size() >= limits_.max_mailbox_items)
        throw AgentTaskError(AgentTaskErrorKind::residency_limit,
                             "task work queue limit reached");

      steer_running = task->status == AgentTaskStatusKind::running ||
                      task->execution_reserved ||
                      task->status == AgentTaskStatusKind::pending_init;
      if (!steer_running) {
        if (active_executions_ >= limits_.max_active_executions)
          throw AgentTaskError(AgentTaskErrorKind::execution_limit,
                               "active child execution limit reached");
        previous_status = task->status;
        previous_result = std::move(task->result);
        task->status = AgentTaskStatusKind::pending_init;
        task->execution_reserved = true;
        ++active_executions_;
      }
      if (!steer_running) {
        task->work.push_back(
            WorkItem{.messages = std::move(messages),
                     .mailbox_delivery = true,
                     .previous_status = previous_status,
                     .previous_result = std::move(previous_result)});
        touch_locked(task);
      } else {
        running_messages = std::move(messages);
      }
    }
  }

  if (task->id == "root") {
    root_.agent().steer_envelopes(std::move(root_messages));
    return snapshot(task);
  }
  if (steer_running) {
    task->session->agent().steer_envelopes(std::move(running_messages));
    emit(AgentTaskMessageQueuedEvent{.id = task->id, .triggers_turn = false});
  } else {
    task->changed.notify_all();
    emit(AgentTaskStatusChangedEvent{.id = task->id,
                                     .previous = previous_status,
                                     .current =
                                         AgentTaskStatusKind::pending_init});
    emit(AgentTaskMessageQueuedEvent{.id = task->id, .triggers_turn = true});
  }
  return snapshot(task);
}

void AgentTaskManager::drop_mailbox_envelopes() {
  std::vector<std::shared_ptr<Task>> tasks;
  {
    std::scoped_lock lock(mutex_);
    for (const auto &[id, task] : tasks_)
      tasks.push_back(task);
  }
  std::vector<std::shared_ptr<Task>> changed_tasks;
  for (const auto &task : tasks) {
    {
      std::scoped_lock lock(mutex_, task->mutex);
      bool changed = false;
      for (auto it = task->work.begin(); it != task->work.end();) {
        if (!it->mailbox_delivery) {
          ++it;
          continue;
        }
        if (task->execution_reserved &&
            task->status == AgentTaskStatusKind::pending_init &&
            it->previous_status) {
          task->status = *it->previous_status;
          task->result = std::move(it->previous_result);
          task->execution_reserved = false;
          if (active_executions_ > 0)
            --active_executions_;
        }
        changed = true;
        it = task->work.erase(it);
      }
      if (changed) {
        task->generation = ++generation_;
        changed_tasks.push_back(task);
      }
    }
    task->session->agent().clear_mailbox_steering_queue();
  }
  for (const auto &task : changed_tasks)
    task->changed.notify_all();
  if (!changed_tasks.empty())
    changed_.notify_all();
}

AgentTaskSnapshot AgentTaskManager::follow_up(const AgentTaskId &target,
                                              const Message &message) {
  const auto text = message_text(message);
  if (text.empty())
    throw AgentTaskError(AgentTaskErrorKind::invalid_context,
                         "follow-up message must contain text");
  if (message_bytes(message) > limits_.max_message_bytes)
    throw AgentTaskError(AgentTaskErrorKind::residency_limit,
                         "task message exceeds size limit");
  std::shared_ptr<Task> task;
  AgentTaskStatusKind previous_status = AgentTaskStatusKind::running;
  bool queued_new_turn = false;
  {
    std::scoped_lock lock(mutex_);
    task = find_task_locked(target);
    if (!task)
      throw AgentTaskError(AgentTaskErrorKind::not_found, "task not found");
    std::scoped_lock task_lock(task->mutex);
    if (task->status == AgentTaskStatusKind::closing ||
        task->status == AgentTaskStatusKind::shutdown)
      throw AgentTaskError(AgentTaskErrorKind::invalid_state, "task is closed");
    if (task->work.size() + task->mailbox.size() >= limits_.max_mailbox_items)
      throw AgentTaskError(AgentTaskErrorKind::residency_limit,
                           "task work queue limit reached");
    if (task->status != AgentTaskStatusKind::running &&
        !task->execution_reserved) {
      if (active_executions_ >= limits_.max_active_executions)
        throw AgentTaskError(AgentTaskErrorKind::execution_limit,
                             "active child execution limit reached");
      previous_status = task->status;
      queued_new_turn = true;
      task->status = AgentTaskStatusKind::pending_init;
      task->result.reset();
      ++active_executions_;
      task->execution_reserved = true;
    }
    std::string prompt;
    while (!task->mailbox.empty()) {
      if (!prompt.empty())
        prompt += "\n\n";
      prompt += message_text(task->mailbox.front());
      task->mailbox.pop_front();
    }
    if (!prompt.empty())
      prompt += "\n\n";
    prompt += text;
    UserMessage follow_up_message;
    follow_up_message.content.emplace_back(
        TextContent{.text = std::move(prompt)});
    task->work.push_back(
        WorkItem{.messages = std::vector<AgentMessageEnvelope>{
                     message_envelope(Message{std::move(follow_up_message)})}});
    touch_locked(task);
  }
  task->changed.notify_all();
  if (queued_new_turn)
    emit(AgentTaskStatusChangedEvent{.id = task->id,
                                     .previous = previous_status,
                                     .current =
                                         AgentTaskStatusKind::pending_init});
  emit(AgentTaskMessageQueuedEvent{.id = task->id, .triggers_turn = true});
  return snapshot(task);
}

AgentTaskSnapshot AgentTaskManager::interrupt(const AgentTaskId &target,
                                              AgentInterruptReason reason) {
  std::shared_ptr<Task> task;
  AgentTaskStatusKind previous{AgentTaskStatusKind::pending_init};
  {
    std::scoped_lock lock(mutex_);
    task = find_task_locked(target);
    if (!task)
      throw AgentTaskError(AgentTaskErrorKind::not_found, "task not found");
    std::scoped_lock task_lock(task->mutex);
    previous = task->status;
    if (previous == AgentTaskStatusKind::running) {
      task->pending_interrupt = reason;
      touch_locked(task);
    }
  }
  if (previous == AgentTaskStatusKind::running) {
    task->session->agent().interrupt(turn_abort_reason(reason));
    emit(AgentTaskInterruptedEvent{.id = task->id, .reason = reason});
  }
  return snapshot(task);
}

AgentTaskSnapshot
AgentTaskManager::close_tasks(std::vector<std::shared_ptr<Task>> tasks) {
  if (tasks.empty())
    throw AgentTaskError(AgentTaskErrorKind::not_found, "task not found");

  for (const auto &task : tasks) {
    std::scoped_lock lock(task->mutex);
    task->close_requested = true;
    if (task->status != AgentTaskStatusKind::shutdown)
      task->status = AgentTaskStatusKind::closing;
    task->changed.notify_all();
  }
  changed_.notify_all();

  for (const auto &task : tasks) {
    task->session->agent().interrupt(TurnAbortReason::shutdown);
    task->runner.request_stop();
  }
  for (const auto &task : tasks)
    if (task->runner.joinable())
      task->runner.join();

  AgentTaskSnapshot target_snapshot;
  std::vector<AgentTaskEvent> closed_events;
  std::vector<AgentTaskId> unregister_ids;
  UnregisterEndpointCallback unregister_endpoint;
  {
    std::scoped_lock lock(mutex_);
    unregister_endpoint = unregister_endpoint_;
    for (const auto &task : tasks) {
      std::scoped_lock task_lock(task->mutex);
      if (task->execution_reserved && active_executions_ > 0)
        --active_executions_;
      task->execution_reserved = false;
      task->status = AgentTaskStatusKind::shutdown;
      if (task->runtime_identity)
        unregister_ids.push_back(task->id);
      touch_locked(task);
      if (task->id == tasks.front()->id) {
        target_snapshot.id = task->id;
        target_snapshot.task_path = task->task_path;
        target_snapshot.parent_id = task->parent_id;
        target_snapshot.task_name = task->task_name;
        target_snapshot.status = task->status;
        target_snapshot.result = task->result;
        target_snapshot.child_count = task->children.size();
        target_snapshot.queued_message_count =
            task->mailbox.size() + task->work.size();
        target_snapshot.generation = task->generation;
      }
      if (task->parent_id) {
        if (auto parent = find_task_locked(*task->parent_id)) {
          std::scoped_lock parent_lock(parent->mutex);
          parent->children.erase(task->id);
        }
      }
      closed_events.emplace_back(AgentTaskClosedEvent{task->id});
    }
    for (const auto &task : tasks)
      tasks_.erase(task->id);
  }
  changed_.notify_all();
  if (unregister_endpoint) {
    for (const auto &task_id : unregister_ids) {
      try {
        unregister_endpoint(task_id);
      } catch (...) {
        static_cast<void>(0);
      }
    }
  }
  for (const auto &event : closed_events)
    emit(event);
  return target_snapshot;
}
AgentTaskSnapshot AgentTaskManager::close(const AgentTaskId &target) {
  std::vector<std::shared_ptr<Task>> closing;
  {
    std::scoped_lock lock(mutex_);
    auto target_task = find_task_locked(target);
    if (!target_task)
      throw AgentTaskError(AgentTaskErrorKind::not_found, "task not found");
    if (target_task->id == "root")
      throw AgentTaskError(AgentTaskErrorKind::permission_denied,
                           "root task cannot be closed");
    std::function<void(const std::shared_ptr<Task> &)> collect =
        [&](const std::shared_ptr<Task> &task) {
          closing.push_back(task);
          for (const auto &child_id : task->children)
            if (auto child = find_task_locked(child_id))
              collect(child);
        };
    collect(target_task);
  }
  return close_tasks(std::move(closing));
}

AgentWaitResult AgentTaskManager::wait(const AgentWaitRequest &request,
                                       std::stop_token stop_token) const {
  const auto timeout = std::clamp(request.timeout, std::chrono::milliseconds(0),
                                  limits_.max_wait);
  std::unique_lock lock(mutex_);
  std::vector<std::shared_ptr<Task>> targets;
  if (request.targets.empty()) {
    for (const auto &[id, task] : tasks_)
      if (id != "root")
        targets.push_back(task);
  } else {
    for (const auto &target : request.targets) {
      auto task = find_task_locked(target);
      if (!task)
        throw AgentTaskError(AgentTaskErrorKind::not_found,
                             "wait target not found: " + target);
      targets.push_back(std::move(task));
    }
  }

  auto changed = [&] {
    if (stop_token.stop_requested())
      return true;
    return std::ranges::any_of(targets, [&](const auto &task) {
      return task->generation > request.after_generation;
    });
  };
  const bool observed =
      changed() || changed_.wait_for(lock, stop_token, timeout, changed);
  AgentWaitResult result;
  result.caller_interrupted = stop_token.stop_requested();
  result.timed_out = !observed && !result.caller_interrupted;
  result.generation = generation_;
  if (observed && !result.caller_interrupted) {
    for (const auto &task : targets)
      if (task->generation > request.after_generation)
        result.changed.push_back(snapshot(task));
  }
  return result;
}

void AgentTaskManager::shutdown() {
  std::vector<std::shared_ptr<Task>> children;
  {
    std::unique_lock lock(mutex_);
    if (shutting_down_) {
      changed_.wait(lock, [this] { return pending_spawns_ == 0; });
      return;
    }
    shutting_down_ = true;
    shutdown_source_.request_stop();
    changed_.wait(lock, [this] { return pending_spawns_ == 0; });
    for (const auto &[id, task] : tasks_)
      if (id != "root")
        children.push_back(task);
  }
  if (!children.empty())
    static_cast<void>(close_tasks(std::move(children)));
  changed_.notify_all();
}

bool AgentTaskManager::is_shutting_down() const {
  std::scoped_lock lock(mutex_);
  return shutting_down_;
}

std::size_t AgentTaskManager::active_executions() const {
  std::scoped_lock lock(mutex_);
  return active_executions_;
}

std::size_t AgentTaskManager::resident_tasks() const {
  std::scoped_lock lock(mutex_);
  return !tasks_.empty() ? tasks_.size() - 1 : 0;
}

} // namespace pi::core
