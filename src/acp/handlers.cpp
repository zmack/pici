#include "acp/handlers.h"
#include "acp/server.h"
#include "acp/sse.h"
#include "acp/task_events.h"
#include "acp/types.h"
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
#include <utility>
#include <variant>
#include <vector>

namespace pi::acp {

namespace {

// Translates Renderer callbacks into ACP SSE events.

// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::mutex durable_run_mutex;

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
      Message msg;
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

// Extract plain text from a pici AssistantMessage's content blocks.
std::string assistant_text(const core::AssistantMessage &am) {
  std::string text;
  for (const auto &block : am.content) {
    if (const auto *tc = std::get_if<core::TextContent>(&block))
      text += tc->text;
  }
  return text;
}

// Extract plain text from a pici ToolResultMessage.
std::string tool_result_text(const core::ToolResultMessage &tr) {
  std::string text;
  for (const auto &block : tr.content) {
    if (const auto *tc = std::get_if<core::TextContent>(&block))
      text += tc->text;
  }
  return text;
}

// Convert all messages accumulated during a run into ACP output messages.
std::vector<Message>
messages_from_run(const std::string &agent_name,
                  const std::string &accumulated_text,
                  const std::vector<core::ToolResultMessage> &tool_results) {
  std::vector<Message> out;
  if (!accumulated_text.empty()) {
    out.push_back({.role = agent_name,
                   .parts = {{.content_type = std::string{"text/plain"},
                              .content = accumulated_text}}});
  }
  for (const auto &tr : tool_results) {
    auto text = "[tool:" + tr.tool_name + "] " + tool_result_text(tr);
    out.push_back({.role = agent_name,
                   .parts = {{.content_type = std::string{"text/plain"},
                              .content = std::move(text)}}});
  }
  return out;
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

} // namespace

void register_routes(httplib::Server &svr, const ServerConfig &cfg,
                     const std::shared_ptr<core::SessionStore> &sessions,
                     const std::shared_ptr<core::AgentTaskManager> &tasks,
                     const std::shared_ptr<TaskEventHub> &task_events) {
  const auto manifest_json = nlohmann::json(build_manifest(cfg));

  svr.Get("/health", [](const httplib::Request &, httplib::Response &res) {
    res.set_content(R"({"status":"ok"})", "application/json");
  });

  svr.Get("/agents",
          // NOLINTNEXTLINE(bugprone-exception-escape)
          [manifest_json](const httplib::Request &, httplib::Response &res) {
            json_response(res, 200, nlohmann::json::array({manifest_json}));
          });

  svr.Get("/agents/:name",
          // NOLINTNEXTLINE(bugprone-exception-escape)
          [&cfg, manifest_json](const httplib::Request &req,
                                httplib::Response &res) {
            if (req.path_params.at("name") != cfg.agent_name) {
              json_response(res, 404, {{"error", "agent not found"}});
              return;
            }
            json_response(res, 200, manifest_json);
          });

  svr.Get("/tasks",
          [tasks](const httplib::Request &req, httplib::Response &res) {
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
          });

  svr.Get("/tasks/events", [task_events](const httplib::Request &req,
                                         httplib::Response &res) {
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
  });

  svr.Get("/tasks/:id", [tasks](const httplib::Request &req,
                                httplib::Response &res) {
    const auto snapshot = tasks->get(req.path_params.at("id"));
    if (!snapshot) {
      json_response(
          res, 404,
          {{"error", {{"code", "not_found"}, {"message", "task not found"}}}});
      return;
    }
    json_response(res, 200, task_snapshot_json(*snapshot));
  });

  svr.Post("/tasks", [tasks](const httplib::Request &req,
                             httplib::Response &res) {
    try {
      const auto snapshot = tasks->spawn(parse_spawn_request(parse_body(req)));
      json_response(res, 202, task_snapshot_json(snapshot));
    } catch (const core::AgentTaskError &error) {
      task_error_response(res, error);
    } catch (const std::exception &error) {
      json_response(res, 400, {{"error", error.what()}});
    }
  });

  svr.Post("/tasks/wait", [tasks](const httplib::Request &req,
                                  httplib::Response &res) {
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
  });

  auto register_task_message_route = [&svr, tasks](const char *path,
                                                   bool follow_up) {
    svr.Post(path, [tasks, follow_up](const httplib::Request &req,
                                      httplib::Response &res) {
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
    });
  };
  register_task_message_route("/tasks/:id/message", false);
  register_task_message_route("/tasks/:id/follow-up", true);

  svr.Post("/tasks/:id/interrupt", [tasks](const httplib::Request &req,
                                           httplib::Response &res) {
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
  });

  svr.Post("/tasks/:id/close",
           [tasks](const httplib::Request &req, httplib::Response &res) {
             try {
               const auto snapshot = tasks->close(req.path_params.at("id"));
               json_response(res, 200, task_snapshot_json(snapshot));
             } catch (const core::AgentTaskError &error) {
               task_error_response(res, error);
             } catch (const std::exception &error) {
               json_response(res, 400, {{"error", error.what()}});
             }
           });

  svr.Post(
      "/runs",
      [&cfg,
       sessions](const httplib::Request &req,
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

        // Extract user text from first text/plain part
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

        const std::string run_id = make_run_id();

        auto session_lock =
            std::make_shared<std::unique_lock<std::mutex>>(durable_run_mutex);
        auto session = std::make_shared<core::AgentSession>(
            core::AgentSession::Config{.agent_options = cfg.agent_opts,
                                       .model_registry = cfg.model_registry,
                                       .tools = cfg.tools,
                                       .session_store = sessions,
                                       .sandbox_policy = cfg.sandbox_policy});

        std::optional<std::string> active_session_id;

        // Restore or create durable session history if provided.
        if (rcr.session_id) {
          core::SessionHeader header;
          header.created = std::chrono::system_clock::to_time_t(
              std::chrono::system_clock::now());
          header.model = cfg.agent_opts.model.id;
          header.provider = cfg.agent_opts.model.provider;
          active_session_id = session->open_session(*rcr.session_id, header);
        }

        // An explicit request selection always wins over the restored model,
        // and is journaled before the run starts.
        if (rcr.provider || rcr.model) {
          if (!rcr.model || rcr.model->empty()) {
            json_response(
                res, 400,
                {{"error", "model is required when selecting a provider"}});
            return;
          }
          core::ModelSelection selection{.model = *rcr.model, .source = "acp"};
          if (rcr.provider)
            selection.provider = *rcr.provider;
          const auto resolution = session->resolve_model(selection);
          if (!resolution) {
            json_response(res, 400, {{"error", resolution.error}});
            return;
          }
          if (cfg.auth_resolver &&
              cfg.auth_resolver->availability(resolution.model->provider) ==
                  auth::AuthAvailability::missing) {
            json_response(res, 400,
                          {{"error", "missing authentication for provider '" +
                                         resolution.model->provider + "'"}});
            return;
          }
          try {
            session->set_model(*resolution.model,
                               session->agent().state().thinking_level());
          } catch (const std::exception &error) {
            json_response(res, 409, {{"error", error.what()}});
            return;
          }
        }

        const auto effective_model = session->agent().state().model();

        if (rcr.mode == RunMode::stream) {
          res.set_chunked_content_provider(
              "text/event-stream",
              // NOLINTNEXTLINE(bugprone-exception-escape)
              [&cfg, run_id, prompt, rcr, active_session_id, session,
               session_lock,
               effective_model](std::size_t /*offset*/,
                                httplib::DataSink &sink) mutable -> bool {
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
                sse.emit("run.in-progress", {{"type", "run.in-progress"},
                                             {"run", nlohmann::json(r)}});

                AcpSseRenderer renderer(sse, cfg.agent_name);
                auto result = session->run_prompt(
                    prompt, [&renderer](const core::AgentEvent &event) {
                      core::dispatch_event(event, renderer);
                    });
                if (result.error && !session->agent().state().error_message())
                  renderer.on_error(core::RendererErrorKind::unknown,
                                    *result.error);

                renderer.emit_run_final(r);
                sink.done();
                return true;
              });
          return;
        }

        SyncRenderer sr;
        auto result =
            session->run_prompt(prompt, [&sr](const core::AgentEvent &event) {
              core::dispatch_event(event, sr);
            });
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
      });

  // Streaming runs complete synchronously so this is mostly a stub.
  svr.Get(
      "/runs/:run_id", [](const httplib::Request &req, httplib::Response &res) {
        json_response(
            res, 404,
            {{"error", "run not found (streaming runs complete synchronously)"},
             {"run_id", req.path_params.at("run_id")}});
      });
}

} // namespace pi::acp
