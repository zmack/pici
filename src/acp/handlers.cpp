#include "acp/handlers.h"
#include "acp/server.h"
#include "acp/sse.h"
#include "acp/task_events.h"
#include "acp/types.h"
#include "cli/session_runtime.h"
#include "core/agent.h"
#include "core/agent_task.h"
#include "core/auth/auth_resolver.h"
#include "core/event_types.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/session/agent_session.h"
#include "core/session/session_record.h"
#include "core/session/session_store.h"
#include "core/stream_renderer.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <httplib.h>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace pi::acp {

namespace {

// Translates Renderer callbacks into ACP SSE events.

// Phase 7 of plans/session-runtime-migration.md removed the process-wide
// `durable_run_mutex` that used to wrap the whole /runs request (session
// lookup/construction through run completion), serializing every ACP run
// regardless of which session it targeted. Pre-removal audit of what that
// mutex actually protected, so nothing relies on it silently:
//   - core::SessionStore already has its own internal mutex_ (see
//     core/session/session_store.h) -- concurrent access across sessions
//     was already safe without the coarse lock.
//   - core::AgentTaskManager already self-locks internally throughout
//     agent_task.cpp -- also independent of the coarse lock.
//   - core::Agent (inside SessionRuntime) already rejects a second
//     concurrent run against *the same instance* by throwing
//     std::runtime_error ("Agent is already processing...") rather than
//     corrupting state -- see Agent::prompt() in agent.cpp. So invariant 3
//     ("one activation binds to at most one active session at a time") was
//     already mechanically enforced per-SessionRuntime; the coarse mutex
//     only additionally serialized runs against *different* sessions,
//     which is exactly the over-serialization this phase removes.
// The only state that genuinely needed protection is the session_runtimes
// registry below (a plain, unsynchronized std::unordered_map) -- see
// session_runtimes_mutex, held only for the brief lookup/insert, not for
// the run's duration. A same-session_id collision now surfaces as the
// "already processing" exception, caught at the run call sites below and
// turned into an explicit response instead of an uncaught throw.

class AcpSseRenderer final : public core::Renderer {
public:
  AcpSseRenderer(SseWriter &sse, std::string agent_name)
      : sse_(sse), agent_name_(std::move(agent_name)) {}

  void on_turn_start() override {
    in_message_ = false;
    accumulated_.clear();
    status_ = RunStatus::completed;
    error_ = {};
  }

  void on_text_delta(std::string_view delta) override {
    if (!in_message_) {
      sse_.emit("message.created", {{"type", "message.created"},
                                    {"message",
                                     {{"role", agent_name_},
                                      {"parts", nlohmann::json::array()}}}});
      in_message_ = true;
    }
    accumulated_ += delta;
    sse_.emit("message.part", {{"type", "message.part"},
                               {"part",
                                {{"content_type", "text/plain"},
                                 {"content", std::string(delta)}}}});
  }

  void on_tool_start(std::string_view call_id, std::string_view name,
                     std::string_view args) override {
    sse_.emit(
        "message.part",
        {{"type", "message.part"},
         {"part",
          {{"content_type", "application/json"},
           {"content", nlohmann::json({{"tool_call_id", std::string(call_id)},
                                       {"tool", std::string(name)},
                                       {"args", std::string(args)}})
                           .dump()}}}});
  }

  void on_message_end(const core::TokenUsage &) override {
    if (in_message_) {
      AcpMessage msg;
      msg.role = agent_name_;
      msg.parts.push_back(
          {.content_type = "text/plain", .content = accumulated_});
      sse_.emit("message.completed", {{"type", "message.completed"},
                                      {"message", nlohmann::json(msg)}});
      in_message_ = false;
    }
  }

  void on_turn_end() override {} // run.completed emitted in emit_run_final

  void on_error(core::RendererErrorKind, std::string_view msg) override {
    status_ = RunStatus::failed;
    error_ = std::string(msg);
  }

  // Call once after the agent loop completes to emit the final run event.
  void emit_run_final(Run &r) {
    r.status = status_;
    if (!accumulated_.empty())
      r.output.push_back(
          {.role = agent_name_,
           .parts = {{.content_type = "text/plain", .content = accumulated_}}});
    if (status_ == RunStatus::failed) {
      r.error = error_;
      sse_.emit("run.failed",
                {{"type", "run.failed"}, {"run", nlohmann::json(r)}});
    } else {
      r.status = RunStatus::completed;
      sse_.emit("run.completed",
                {{"type", "run.completed"}, {"run", nlohmann::json(r)}});
    }
  }

private:
  SseWriter &sse_;
  std::string agent_name_;
  std::string accumulated_;
  std::string error_;
  RunStatus status_{RunStatus::completed};
  bool in_message_{false};
};

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<std::uint64_t> gRunCounter{0};

std::string make_run_id() {
  auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  return "run-" + std::to_string(ts) + "-" + std::to_string(++gRunCounter);
}

class SyncRenderer final : public core::Renderer {
public:
  void on_text_delta(std::string_view d) override { accumulated_ += d; }
  void on_error(core::RendererErrorKind, std::string_view msg) override {
    status_ = RunStatus::failed;
    error_ = std::string(msg);
  }
  const std::string &accumulated() const { return accumulated_; }
  RunStatus status() const { return status_; }
  const std::string &error() const { return error_; }

private:
  std::string accumulated_, error_;
  RunStatus status_{RunStatus::completed};
};

void json_response(httplib::Response &res, int status,
                   const nlohmann::json &body) {
  res.status = status;
  res.set_content(body.dump(), "application/json");
}

nlohmann::json parse_body(const httplib::Request &req) {
  auto parsed = nlohmann::json::parse(req.body, nullptr, false);
  if (parsed.is_discarded())
    throw std::runtime_error("invalid JSON body");
  return parsed;
}

nlohmann::json task_snapshot_json(const core::AgentTaskSnapshot &snapshot) {
  nlohmann::json value = {
      {"id", snapshot.id},
      {"task_path", snapshot.task_path},
      {"task_name", snapshot.task_name},
      {"status", core::agent_task_status_to_string(snapshot.status)},
      {"child_count", snapshot.child_count},
      {"queued_message_count", snapshot.queued_message_count},
      {"generation", snapshot.generation},
  };
  if (snapshot.parent_id)
    value["parent_id"] = *snapshot.parent_id;
  else
    value["parent_id"] = nullptr;
  if (snapshot.result) {
    value["result"] = {
        {"text", snapshot.result->text},
        {"stop_reason",
         core::stop_reason_to_string(snapshot.result->stop_reason)},
        {"truncated", snapshot.result->truncated},
        {"usage",
         {{"input", snapshot.result->usage.input},
          {"output", snapshot.result->usage.output},
          {"total_tokens", snapshot.result->usage.total_tokens}}}};
    if (snapshot.result->error)
      value["result"]["error"] = *snapshot.result->error;
    else
      value["result"]["error"] = nullptr;
  } else {
    value["result"] = nullptr;
  }
  return value;
}

int task_error_status(core::AgentTaskErrorKind kind) {
  switch (kind) {
  case core::AgentTaskErrorKind::not_found:
    return 404;
  case core::AgentTaskErrorKind::invalid_state:
  case core::AgentTaskErrorKind::duplicate_name:
  case core::AgentTaskErrorKind::execution_limit:
  case core::AgentTaskErrorKind::residency_limit:
  case core::AgentTaskErrorKind::shutting_down:
    return 409;
  case core::AgentTaskErrorKind::internal:
    return 500;
  default:
    return 400;
  }
}

void task_error_response(httplib::Response &res,
                         const core::AgentTaskError &error) {
  json_response(
      res, task_error_status(error.kind()),
      {{"error",
        {{"code", error.code()}, {"message", std::string(error.what())}}}});
}

core::ContextInheritance parse_task_context(const nlohmann::json &value) {
  core::ContextInheritance context;
  if (!value.is_object())
    return context;
  const auto mode = value.value("mode", std::string("none"));
  if (mode == "none")
    context.mode = core::ContextInheritanceMode::none;
  else if (mode == "full")
    context.mode = core::ContextInheritanceMode::full;
  else if (mode == "through_message")
    context.mode = core::ContextInheritanceMode::through_message;
  else if (mode == "recent_messages")
    context.mode = core::ContextInheritanceMode::recent_messages;
  else
    throw core::AgentTaskError(core::AgentTaskErrorKind::invalid_context,
                               "unknown context inheritance mode");
  if (value.contains("through"))
    context.through = value.at("through").get<std::size_t>();
  if (value.contains("recent_count"))
    context.recent_count = value.at("recent_count").get<std::size_t>();
  return context;
}

core::SpawnAgentRequest parse_spawn_request(const nlohmann::json &value) {
  core::SpawnAgentRequest request;
  request.parent_id = value.value("parent_id", std::string{});
  request.task_name = value.value("task_name", value.value("name", ""));
  request.prompt = value.value("prompt", value.value("message", ""));
  if (value.contains("context"))
    request.context = parse_task_context(value.at("context"));
  if (value.contains("system_prompt") && !value.at("system_prompt").is_null())
    request.system_prompt = value.at("system_prompt").get<std::string>();
  if (value.contains("model") && !value.at("model").is_null())
    request.model_spec = value.at("model").get<std::string>();
  if (value.contains("tools"))
    request.requested_tools = value.at("tools").get<std::vector<std::string>>();
  request.allow_subagents = value.value("allow_subagents", false);
  request.allow_write_tools = value.value("allow_write_tools", false);
  return request;
}

core::Message task_message(std::string text) {
  core::UserMessage message;
  message.content.emplace_back(core::TextContent{.text = std::move(text)});
  return core::Message{std::move(message)};
}

core::AgentInterruptReason parse_interrupt_reason(std::string_view reason) {
  if (reason == "user")
    return core::AgentInterruptReason::user;
  if (reason == "parent")
    return core::AgentInterruptReason::parent;
  if (reason == "replacement_task")
    return core::AgentInterruptReason::replacement_task;
  if (reason == "shutdown")
    return core::AgentInterruptReason::shutdown;
  if (reason == "budget")
    return core::AgentInterruptReason::budget;
  if (reason == "timeout")
    return core::AgentInterruptReason::timeout;
  throw core::AgentTaskError(core::AgentTaskErrorKind::invalid_context,
                             "unknown interrupt reason");
}

std::uint64_t query_uint64(const httplib::Request &req, std::string_view name,
                           std::uint64_t fallback) {
  if (!req.has_param(std::string(name)))
    return fallback;
  try {
    return std::stoull(req.get_param_value(std::string(name)));
  } catch (const std::exception &) {
    throw std::runtime_error("invalid numeric query parameter: " +
                             std::string(name));
  }
}

// Durable-session SessionRuntime reuse (plans/session-runtime-migration.md
// Phase 6, decision (a)): successive /runs calls against the same
// session_id share one SessionRuntime rather than each rebuilding one from
// scratch and reloading the transcript from disk. Anonymous runs (no
// session_id) are never registered, matching pre-Phase-6 behavior exactly.
// A free function rather than inlined at its call site so the /runs
// handler's own cognitive-complexity score only pays for one call, not this
// lookup-or-construct branching.
//
// Phase 7 removed the process-wide durable_run_mutex that used to guard
// this incidentally by wrapping the whole /runs request; registry_mutex
// below protects only this function's brief lookup/insert, not run
// execution, so concurrent /runs calls against different session_ids (or
// anonymous runs) now execute fully in parallel.
std::shared_ptr<core::SessionRuntime> find_or_create_session_runtime(
    const ServerConfig &cfg,
    const std::shared_ptr<core::SessionStore> &sessions,
    const std::optional<std::string> &session_id, std::mutex &registry_mutex,
    std::unordered_map<std::string, std::shared_ptr<core::SessionRuntime>>
        &session_runtimes) {
  if (session_id && !session_id->empty()) {
    std::scoped_lock lock(registry_mutex);
    if (auto existing = session_runtimes.find(*session_id);
        existing != session_runtimes.end())
      return existing->second;
  }
  // ACP never enables auto-compaction (see cli/session_runtime.h); the
  // unused cli::Args{} below is only read when that capability is on.
  auto session =
      std::make_shared<core::SessionRuntime>(cli::build_agent_session_config(
          cfg.agent_opts, cfg.model_registry, cfg.tools, sessions,
          cfg.sandbox_policy, cli::Args{},
          cli::SessionRuntimeCapabilities{.enable_mailbox = false,
                                          .enable_hooks = false,
                                          .enable_skills = false,
                                          .enable_context_files = false,
                                          .enable_auto_compaction = false}));
  if (session_id && !session_id->empty()) {
    std::scoped_lock lock(registry_mutex);
    // Another thread may have raced this one and already inserted a
    // runtime for the same session_id while this one was under
    // construction (construction happens outside the lock, deliberately,
    // so it never blocks unrelated sessions' lookups) -- prefer whichever
    // one won the race so every caller ends up sharing a single runtime
    // per session_id, rather than the second constructor's result
    // silently replacing the first and orphaning any run already
    // in flight against it.
    auto [it, inserted] = session_runtimes.try_emplace(*session_id, session);
    if (!inserted)
      return it->second;
  }
  return session;
}

// The route handlers below were formerly all inlined as lambdas directly
// inside register_routes(); each is factored out purely to keep that
// function's branch count down to route wiring, with no behavior change.

void health_handler(const httplib::Request &, httplib::Response &res) {
  res.set_content(R"({"status":"ok"})", "application/json");
}

httplib::Server::Handler
make_agents_list_handler(nlohmann::json manifest_json) {
  // NOLINTNEXTLINE(bugprone-exception-escape)
  return [manifest_json](const httplib::Request &, httplib::Response &res) {
    json_response(res, 200, nlohmann::json::array({manifest_json}));
  };
}

httplib::Server::Handler make_agent_get_handler(const ServerConfig &cfg,
                                                nlohmann::json manifest_json) {
  // NOLINTNEXTLINE(bugprone-exception-escape)
  return [&cfg, manifest_json](const httplib::Request &req,
                               httplib::Response &res) {
    if (req.path_params.at("name") != cfg.agent_name) {
      json_response(res, 404, {{"error", "agent not found"}});
      return;
    }
    json_response(res, 200, manifest_json);
  };
}

httplib::Server::Handler
make_tasks_list_handler(std::shared_ptr<core::AgentTaskManager> tasks) {
  return [tasks](const httplib::Request &req, httplib::Response &res) {
    try {
      std::optional<std::string_view> prefix;
      std::string prefix_storage;
      if (req.has_param("path_prefix")) {
        prefix_storage = req.get_param_value("path_prefix");
        prefix = prefix_storage;
      }
      nlohmann::json values = nlohmann::json::array();
      for (const auto &snapshot : tasks->list(prefix))
        values.push_back(task_snapshot_json(snapshot));
      json_response(res, 200, values);
    } catch (const core::AgentTaskError &error) {
      task_error_response(res, error);
    } catch (const std::exception &error) {
      json_response(res, 400, {{"error", error.what()}});
    }
  };
}

httplib::Server::Handler
make_tasks_events_handler(std::shared_ptr<TaskEventHub> task_events) {
  return [task_events](const httplib::Request &req, httplib::Response &res) {
    try {
      const auto after = query_uint64(req, "after_generation", 0);
      const auto timeout = query_uint64(req, "timeout_ms", 30000);
      const bool stream =
          req.has_param("stream") && req.get_param_value("stream") != "0";
      if (!stream) {
        const auto batch =
            task_events->wait_since(after, std::chrono::milliseconds(timeout));
        nlohmann::json events = nlohmann::json::array();
        for (const auto &record : batch.events)
          events.push_back(
              {{"sequence", record.sequence}, {"event", record.event}});
        json_response(res, 200,
                      {{"generation", batch.generation},
                       {"timed_out", batch.timed_out},
                       {"events", std::move(events)}});
        return;
      }

      res.set_chunked_content_provider(
          "text/event-stream",
          [task_events, after](std::size_t /*offset*/,
                               httplib::DataSink &sink) mutable {
            auto generation = after;
            while (sink.is_writable()) {
              const auto batch =
                  task_events->wait_since(generation, std::chrono::seconds(15));
              if (batch.events.empty()) {
                static constexpr std::string_view keep_alive = ": ping\n\n";
                sink.write(keep_alive.data(), keep_alive.size());
                continue;
              }
              for (const auto &record : batch.events) {
                const auto data = nlohmann::json{{"sequence", record.sequence},
                                                 {"event", record.event}};
                const auto payload =
                    "event: task\ndata: " + data.dump() + "\n\n";
                sink.write(payload.data(), payload.size());
              }
              generation = batch.generation;
            }
            sink.done();
            return true;
          });
    } catch (const std::exception &error) {
      json_response(res, 400, {{"error", error.what()}});
    }
  };
}

httplib::Server::Handler
make_task_get_handler(std::shared_ptr<core::AgentTaskManager> tasks) {
  return [tasks](const httplib::Request &req, httplib::Response &res) {
    const auto snapshot = tasks->get(req.path_params.at("id"));
    if (!snapshot) {
      json_response(
          res, 404,
          {{"error", {{"code", "not_found"}, {"message", "task not found"}}}});
      return;
    }
    json_response(res, 200, task_snapshot_json(*snapshot));
  };
}

httplib::Server::Handler
make_task_create_handler(std::shared_ptr<core::AgentTaskManager> tasks) {
  return [tasks](const httplib::Request &req, httplib::Response &res) {
    try {
      const auto snapshot = tasks->spawn(parse_spawn_request(parse_body(req)));
      json_response(res, 202, task_snapshot_json(snapshot));
    } catch (const core::AgentTaskError &error) {
      task_error_response(res, error);
    } catch (const std::exception &error) {
      json_response(res, 400, {{"error", error.what()}});
    }
  };
}

httplib::Server::Handler
make_task_wait_handler(std::shared_ptr<core::AgentTaskManager> tasks) {
  return [tasks](const httplib::Request &req, httplib::Response &res) {
    try {
      const auto body = parse_body(req);
      core::AgentWaitRequest request;
      request.targets = body.value("targets", std::vector<std::string>{});
      request.after_generation = body.value("after_generation", 0ULL);
      request.timeout =
          std::chrono::milliseconds(body.value("timeout_ms", 30000ULL));
      const auto result = tasks->wait(request);
      nlohmann::json changed = nlohmann::json::array();
      for (const auto &snapshot : result.changed)
        changed.push_back(task_snapshot_json(snapshot));
      json_response(res, 200,
                    {{"timed_out", result.timed_out},
                     {"caller_interrupted", result.caller_interrupted},
                     {"generation", result.generation},
                     {"changed", std::move(changed)}});
    } catch (const core::AgentTaskError &error) {
      task_error_response(res, error);
    } catch (const std::exception &error) {
      json_response(res, 400, {{"error", error.what()}});
    }
  };
}

httplib::Server::Handler
make_task_message_handler(std::shared_ptr<core::AgentTaskManager> tasks,
                          bool follow_up) {
  return
      [tasks, follow_up](const httplib::Request &req, httplib::Response &res) {
        try {
          const auto body = parse_body(req);
          const auto text =
              body.value("message", body.value("prompt", std::string{}));
          if (text.empty())
            throw std::runtime_error("message must not be empty");
          const auto target = req.path_params.at("id");
          const auto snapshot =
              follow_up ? tasks->follow_up(target, task_message(text))
                        : tasks->send_message(target, task_message(text));
          json_response(res, 202, task_snapshot_json(snapshot));
        } catch (const core::AgentTaskError &error) {
          task_error_response(res, error);
        } catch (const std::exception &error) {
          json_response(res, 400, {{"error", error.what()}});
        }
      };
}

httplib::Server::Handler
make_task_interrupt_handler(std::shared_ptr<core::AgentTaskManager> tasks) {
  return [tasks](const httplib::Request &req, httplib::Response &res) {
    try {
      const auto body =
          req.body.empty() ? nlohmann::json::object() : parse_body(req);
      const auto reason =
          parse_interrupt_reason(body.value("reason", std::string("user")));
      const auto snapshot = tasks->interrupt(req.path_params.at("id"), reason);
      json_response(res, 202, task_snapshot_json(snapshot));
    } catch (const core::AgentTaskError &error) {
      task_error_response(res, error);
    } catch (const std::exception &error) {
      json_response(res, 400, {{"error", error.what()}});
    }
  };
}

httplib::Server::Handler
make_task_close_handler(std::shared_ptr<core::AgentTaskManager> tasks) {
  return [tasks](const httplib::Request &req, httplib::Response &res) {
    try {
      const auto snapshot = tasks->close(req.path_params.at("id"));
      json_response(res, 200, task_snapshot_json(snapshot));
    } catch (const core::AgentTaskError &error) {
      task_error_response(res, error);
    } catch (const std::exception &error) {
      json_response(res, 400, {{"error", error.what()}});
    }
  };
}

std::string extract_run_prompt(const RunCreateRequest &rcr) {
  std::string prompt;
  for (const auto &msg : rcr.input) {
    for (const auto &part : msg.parts) {
      if (part.content_type == "text/plain") {
        if (!prompt.empty())
          prompt += '\n';
        prompt += part.content;
      }
    }
  }
  return prompt;
}

void open_run_session(const ServerConfig &cfg, core::SessionRuntime &session,
                      const std::string &session_id) {
  core::SessionHeader header;
  header.created =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  header.model = cfg.agent_opts.model.id;
  header.provider = cfg.agent_opts.model.provider;
  session.open_session(session_id, header);
}

// Applies an explicit provider/model override from the run request, writing
// an error response and returning false if the caller should stop.
bool apply_requested_run_model(const RunCreateRequest &rcr,
                               const ServerConfig &cfg,
                               core::SessionRuntime &session,
                               httplib::Response &res) {
  if (!rcr.model || rcr.model->empty()) {
    json_response(res, 400,
                  {{"error", "model is required when selecting a provider"}});
    return false;
  }
  core::ModelSelection selection{.model = *rcr.model, .source = "acp"};
  if (rcr.provider)
    selection.provider = *rcr.provider;
  const auto resolution = session.resolve_model(selection);
  if (!resolution) {
    json_response(res, 400, {{"error", resolution.error}});
    return false;
  }
  if (cfg.auth_resolver &&
      cfg.auth_resolver->availability(resolution.model->provider) ==
          auth::AuthAvailability::missing) {
    json_response(res, 400,
                  {{"error", "missing authentication for provider '" +
                                 resolution.model->provider + "'"}});
    return false;
  }
  try {
    session.set_model(*resolution.model,
                      session.agent().state().thinking_level());
  } catch (const std::exception &error) {
    json_response(res, 409, {{"error", error.what()}});
    return false;
  }
  return true;
}

httplib::ContentProviderWithoutLength
make_run_stream_provider(const ServerConfig &cfg, std::string run_id,
                         std::string prompt,
                         std::optional<std::string> active_session_id,
                         std::shared_ptr<core::SessionRuntime> session,
                         core::Model effective_model) {
  // NOLINTNEXTLINE(bugprone-exception-escape)
  return [&cfg, run_id = std::move(run_id), prompt = std::move(prompt),
          active_session_id = std::move(active_session_id), session,
          effective_model = std::move(effective_model)](
             std::size_t /*offset*/, httplib::DataSink &sink) mutable -> bool {
    SseWriter sse(sink);

    Run r;
    r.run_id = run_id;
    r.agent_name = cfg.agent_name;
    r.status = RunStatus::created;
    r.session_id = active_session_id;
    r.provider = effective_model.provider;
    r.model = effective_model.id;
    sse.emit("run.created",
             {{"type", "run.created"}, {"run", nlohmann::json(r)}});
    r.status = RunStatus::in_progress;
    sse.emit("run.in-progress",
             {{"type", "run.in-progress"}, {"run", nlohmann::json(r)}});

    AcpSseRenderer renderer(sse, cfg.agent_name);
    // run.created/run.in-progress are already on the wire by this point, so
    // a same-session_id collision with another in-flight run (core::Agent
    // rejects a second concurrent run against the same instance -- see the
    // audit above durable_run_mutex's old declaration) can no longer become
    // an HTTP error status; report it the same way any other run_prompt
    // failure is reported, through the renderer and a failed final run
    // event.
    core::SessionRuntime::RunResult result;
    try {
      result = session->run_prompt(prompt,
                                   [&renderer](const core::AgentEvent &event) {
                                     core::dispatch_event(event, renderer);
                                   });
    } catch (const std::exception &error) {
      result.error = error.what();
    }
    if (result.error && !session->agent().state().error_message())
      renderer.on_error(core::RendererErrorKind::unknown, *result.error);

    renderer.emit_run_final(r);
    sink.done();
    return true;
  };
}

void send_run_sync_response(
    const ServerConfig &cfg, const std::string &run_id,
    const std::string &prompt,
    const std::optional<std::string> &active_session_id,
    const std::shared_ptr<core::SessionRuntime> &session,
    const core::Model &effective_model, httplib::Response &res) {
  SyncRenderer sr;
  core::SessionRuntime::RunResult result;
  try {
    result = session->run_prompt(prompt, [&sr](const core::AgentEvent &event) {
      core::dispatch_event(event, sr);
    });
  } catch (const std::exception &error) {
    json_response(res, 409, {{"error", error.what()}});
    return;
  }
  if (result.error && !session->agent().state().error_message())
    sr.on_error(core::RendererErrorKind::unknown, *result.error);

  Run r;
  r.run_id = run_id;
  r.agent_name = cfg.agent_name;
  r.status = sr.status();
  r.session_id = active_session_id;
  r.provider = effective_model.provider;
  r.model = effective_model.id;
  if (!sr.accumulated().empty())
    r.output.push_back({.role = cfg.agent_name,
                        .parts = {{.content_type = "text/plain",
                                   .content = sr.accumulated()}}});
  if (sr.status() == RunStatus::failed)
    r.error = sr.error();

  json_response(res, 200, nlohmann::json(r));
}

httplib::Server::Handler make_run_create_handler(
    const ServerConfig &cfg, std::shared_ptr<core::SessionStore> sessions,
    std::shared_ptr<std::mutex> session_runtimes_mutex,
    std::shared_ptr<
        std::unordered_map<std::string, std::shared_ptr<core::SessionRuntime>>>
        session_runtimes) {
  return [&cfg, sessions, session_runtimes_mutex, session_runtimes](
             const httplib::Request &req,
             httplib::Response &res) { // NOLINT(bugprone-exception-escape):
                                       // httplib owns callback errors.
    RunCreateRequest rcr;
    try {
      from_json(parse_body(req), rcr);
    } catch (const std::exception &e) {
      json_response(res, 400, {{"error", e.what()}});
      return;
    }
    if (rcr.agent_name != cfg.agent_name) {
      json_response(res, 404, {{"error", "agent not found"}});
      return;
    }
    if (rcr.input.empty()) {
      json_response(res, 400, {{"error", "input must not be empty"}});
      return;
    }

    const auto prompt = extract_run_prompt(rcr);
    const std::string run_id = make_run_id();

    auto session = find_or_create_session_runtime(cfg, sessions, rcr.session_id,
                                                  *session_runtimes_mutex,
                                                  *session_runtimes);

    // Restore or create durable session history the first time this
    // SessionRuntime sees rcr.session_id; a reused runtime already has it
    // active in memory (and re-opening would reload the transcript from
    // disk, discarding continuity the whole point of reuse is to keep).
    if (rcr.session_id && !session->active_session_id())
      open_run_session(cfg, *session, *rcr.session_id);
    std::optional<std::string> active_session_id;
    if (rcr.session_id)
      active_session_id = session->active_session_id();

    // An explicit request selection always wins over the restored model,
    // and is journaled before the run starts.
    if ((rcr.provider || rcr.model) &&
        !apply_requested_run_model(rcr, cfg, *session, res))
      return;

    const auto effective_model = session->agent().state().model();

    if (rcr.mode == RunMode::stream) {
      res.set_chunked_content_provider(
          "text/event-stream",
          make_run_stream_provider(cfg, run_id, prompt, active_session_id,
                                   session, effective_model));
      return;
    }

    send_run_sync_response(cfg, run_id, prompt, active_session_id, session,
                           effective_model, res);
  };
}

void run_not_found_handler(const httplib::Request &req,
                           httplib::Response &res) {
  json_response(
      res, 404,
      {{"error", "run not found (streaming runs complete synchronously)"},
       {"run_id", req.path_params.at("run_id")}});
}

} // namespace

void register_routes(httplib::Server &svr, const ServerConfig &cfg,
                     const std::shared_ptr<core::SessionStore> &sessions,
                     const std::shared_ptr<core::AgentTaskManager> &tasks,
                     const std::shared_ptr<TaskEventHub> &task_events) {
  const auto manifest_json = nlohmann::json(build_manifest(cfg));

  svr.Get("/health", health_handler);
  svr.Get("/agents", make_agents_list_handler(manifest_json));
  svr.Get("/agents/:name", make_agent_get_handler(cfg, manifest_json));
  svr.Get("/tasks", make_tasks_list_handler(tasks));
  svr.Get("/tasks/events", make_tasks_events_handler(task_events));
  svr.Get("/tasks/:id", make_task_get_handler(tasks));
  svr.Post("/tasks", make_task_create_handler(tasks));
  svr.Post("/tasks/wait", make_task_wait_handler(tasks));
  svr.Post("/tasks/:id/message", make_task_message_handler(tasks, false));
  svr.Post("/tasks/:id/follow-up", make_task_message_handler(tasks, true));
  svr.Post("/tasks/:id/interrupt", make_task_interrupt_handler(tasks));
  svr.Post("/tasks/:id/close", make_task_close_handler(tasks));

  // Registry backing find_or_create_session_runtime() above, and the small
  // mutex that protects only its lookup/insert (see the pre-removal audit
  // above durable_run_mutex's old declaration) -- run execution itself is
  // no longer serialized through this.
  auto session_runtimes_mutex = std::make_shared<std::mutex>();
  auto session_runtimes = std::make_shared<
      std::unordered_map<std::string, std::shared_ptr<core::SessionRuntime>>>();
  svr.Post("/runs",
           make_run_create_handler(cfg, sessions, session_runtimes_mutex,
                                   session_runtimes));

  // Streaming runs complete synchronously so this is mostly a stub.
  svr.Get("/runs/:run_id", run_not_found_handler);
}

} // namespace pi::acp
