#include "acp/handlers.h"
#include "acp/sse.h"
#include "core/agent.h"
#include "core/event_types.h"
#include "core/message_types.h"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace pi::acp {

namespace {

// ─── ID generation ───────────────────────────────────────────────────────────

static std::atomic<std::uint64_t> gRunCounter{0};

std::string make_run_id() {
  auto ts = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
  return "run-" + std::to_string(ts) + "-" +
         std::to_string(++gRunCounter);
}

// ─── pici Message → ACP Message ──────────────────────────────────────────────

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
std::vector<Message> messages_from_run(
    const std::string &agent_name,
    const std::string &accumulated_text,
    const std::vector<core::ToolResultMessage> &tool_results) {
  std::vector<Message> out;
  if (!accumulated_text.empty()) {
    out.push_back({agent_name, {{std::string{"text/plain"}, accumulated_text}}});
  }
  for (const auto &tr : tool_results) {
    auto text = "[tool:" + tr.tool_name + "] " + tool_result_text(tr);
    out.push_back({agent_name, {{std::string{"text/plain"}, std::move(text)}}});
  }
  return out;
}

// ─── Core run logic ───────────────────────────────────────────────────────────

struct RunResult {
  RunStatus status{RunStatus::completed};
  std::string accumulated_text;
  std::vector<core::ToolResultMessage> tool_results;
  std::string error;
};

// Drive one agent turn; call on_event for each AgentEvent.
// Returns the accumulated text and tool results.
RunResult drive_agent(core::Agent &agent,
                      const std::string &prompt,
                      const std::function<void(const core::AgentEvent &)> &on_event) {
  RunResult result;
  try {
    auto stream = agent.prompt(prompt);
    for (const auto &ev : stream) {
      if (on_event) on_event(ev);
      std::visit(
          [&result](const auto &e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, core::MessageUpdateEvent>) {
              std::visit(
                  [&result](const auto &ae) {
                    using AE = std::decay_t<decltype(ae)>;
                    if constexpr (std::is_same_v<AE, core::AssistantMessageTextDeltaEvent>)
                      result.accumulated_text += ae.delta;
                    else if constexpr (std::is_same_v<AE, core::AssistantMessageErrorEvent>) {
                      result.status = RunStatus::failed;
                      result.error  = ae.error.error_message.value_or("LLM error");
                    }
                  },
                  e.assistant_message_event);
            } else if constexpr (std::is_same_v<T, core::MessageEndEvent>) {
              if (const auto *am = std::get_if<core::AssistantMessage>(&e.message))
                if (am->error_message) {
                  result.status = RunStatus::failed;
                  result.error  = *am->error_message;
                }
            }
          },
          ev);
    }
  } catch (const std::exception &ex) {
    result.status = RunStatus::failed;
    result.error  = ex.what();
  }
  return result;
}

// ─── HTTP helpers ─────────────────────────────────────────────────────────────

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

} // namespace

// ─── Route registration ───────────────────────────────────────────────────────

void register_routes(httplib::Server &svr,
                     const ServerConfig &cfg,
                     std::shared_ptr<SessionStore> sessions) {
  const auto manifest_json = nlohmann::json(build_manifest(cfg));

  // ── GET /health ────────────────────────────────────────────────────────────
  svr.Get("/health", [](const httplib::Request &, httplib::Response &res) {
    res.set_content(R"({"status":"ok"})", "application/json");
  });

  // ── GET /agents ────────────────────────────────────────────────────────────
  svr.Get("/agents", [manifest_json](const httplib::Request &,
                                     httplib::Response &res) {
    json_response(res, 200, nlohmann::json::array({manifest_json}));
  });

  // ── GET /agents/{name} ─────────────────────────────────────────────────────
  svr.Get("/agents/:name", [&cfg, manifest_json](const httplib::Request &req,
                                                  httplib::Response &res) {
    if (req.path_params.at("name") != cfg.agent_name) {
      json_response(res, 404, {{"error", "agent not found"}});
      return;
    }
    json_response(res, 200, manifest_json);
  });

  // ── POST /runs ─────────────────────────────────────────────────────────────
  svr.Post("/runs", [&cfg, sessions](const httplib::Request &req,
                                      httplib::Response &res) {
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
          if (!prompt.empty()) prompt += '\n';
          prompt += part.content;
        }
      }
    }

    const std::string run_id = make_run_id();

    // Build agent on heap (Agent is non-movable due to internal mutex)
    auto agent_ptr = std::make_shared<core::Agent>(cfg.agent_opts);
    agent_ptr->set_tools(cfg.tools);

    // Restore session history if provided
    if (rcr.session_id) {
      auto hist = sessions->load(*rcr.session_id);
      if (!hist.empty()) agent_ptr->state().set_messages(std::move(hist));
    }

    // ── Streaming mode ─────────────────────────────────────────────────────
    if (rcr.mode == RunMode::stream) {
      res.set_chunked_content_provider(
          "text/event-stream",
          [&cfg, sessions, run_id, prompt, rcr,
           agent_ptr](std::size_t /*offset*/,
                      httplib::DataSink &sink) mutable -> bool {
            auto &agent = *agent_ptr;
            SseWriter sse(sink);

            // run.created
            Run r;
            r.run_id     = run_id;
            r.agent_name = cfg.agent_name;
            r.status     = RunStatus::created;
            if (rcr.session_id) r.session_id = rcr.session_id;
            sse.emit("run.created",
                     {{"type", "run.created"}, {"run", nlohmann::json(r)}});

            // run.in-progress
            r.status = RunStatus::in_progress;
            sse.emit("run.in-progress",
                     {{"type", "run.in-progress"}, {"run", nlohmann::json(r)}});

            // Drive the agent, emitting message.part for each text delta
            std::string accumulated;
            bool in_message = false;
            RunResult result;
            try {
              auto stream = agent.prompt(prompt);
              for (const auto &ev : stream) {
                std::visit(
                    [&](const auto &e) {
                      using T = std::decay_t<decltype(e)>;
                      if constexpr (std::is_same_v<T, core::MessageUpdateEvent>) {
                        std::visit(
                            [&](const auto &ae) {
                              using AE = std::decay_t<decltype(ae)>;
                              if constexpr (std::is_same_v<AE, core::AssistantMessageTextDeltaEvent>) {
                                if (!in_message) {
                                  // Emit message.created on first delta
                                  sse.emit("message.created",
                                           {{"type", "message.created"},
                                            {"message", {{"role", cfg.agent_name},
                                                         {"parts", nlohmann::json::array()}}}});
                                  in_message = true;
                                }
                                accumulated += ae.delta;
                                sse.emit("message.part",
                                         {{"type", "message.part"},
                                          {"part", {{"content_type", "text/plain"},
                                                    {"content", ae.delta}}}});
                              } else if constexpr (std::is_same_v<AE, core::AssistantMessageErrorEvent>) {
                                result.status = RunStatus::failed;
                                result.error  = ae.error.error_message.value_or("LLM error");
                              }
                            },
                            e.assistant_message_event);
                      } else if constexpr (std::is_same_v<T, core::ToolExecutionStartEvent>) {
                        sse.emit("message.part",
                                 {{"type", "message.part"},
                                  {"part", {{"content_type", "application/json"},
                                            {"content", nlohmann::json({{"tool", e.tool_name},
                                                                         {"args", e.args}}).dump()}}}});
                      } else if constexpr (std::is_same_v<T, core::MessageEndEvent>) {
                        if (const auto *am = std::get_if<core::AssistantMessage>(&e.message)) {
                          if (am->error_message) {
                            result.status = RunStatus::failed;
                            result.error  = *am->error_message;
                          }
                          result.accumulated_text = accumulated;
                        }
                      }
                    },
                    ev);
              }
            } catch (const std::exception &ex) {
              result.status = RunStatus::failed;
              result.error  = ex.what();
            }

            // Emit message.completed
            if (in_message) {
              Message completed_msg;
              completed_msg.role = cfg.agent_name;
              completed_msg.parts.push_back({"text/plain", accumulated});
              sse.emit("message.completed",
                       {{"type", "message.completed"},
                        {"message", nlohmann::json(completed_msg)}});
            }

            // Persist session
            if (rcr.session_id) {
              sessions->save(*rcr.session_id, agent.state().messages());
            }

            // Final run event
            r.status = result.status;
            if (!result.accumulated_text.empty())
              r.output.push_back({cfg.agent_name,
                                   {{"text/plain", result.accumulated_text}}});
            if (result.status == RunStatus::failed) {
              r.error = result.error;
              sse.emit("run.failed",
                       {{"type", "run.failed"}, {"run", nlohmann::json(r)}});
            } else {
              r.status = RunStatus::completed;
              sse.emit("run.completed",
                       {{"type", "run.completed"}, {"run", nlohmann::json(r)}});
            }
            sink.done(); // signal clean end of chunked stream
            return true;
          });
      return;
    }

    // ── Sync mode ──────────────────────────────────────────────────────────
    auto result = drive_agent(*agent_ptr, prompt, nullptr);

    if (rcr.session_id)
      sessions->save(*rcr.session_id, agent_ptr->state().messages());

    Run r;
    r.run_id     = run_id;
    r.agent_name = cfg.agent_name;
    r.status     = result.status;
    r.session_id = rcr.session_id;
    if (!result.accumulated_text.empty())
      r.output.push_back(
          {cfg.agent_name, {{"text/plain", result.accumulated_text}}});
    if (result.status == RunStatus::failed)
      r.error = result.error;

    json_response(res, 200, nlohmann::json(r));
  });

  // ── GET /runs/{run_id} ─────────────────────────────────────────────────────
  // Streaming runs complete synchronously so this is mostly a stub.
  svr.Get("/runs/:run_id", [](const httplib::Request &req,
                               httplib::Response &res) {
    json_response(res, 404,
                  {{"error", "run not found (streaming runs complete synchronously)"},
                   {"run_id", req.path_params.at("run_id")}});
  });
}

} // namespace pi::acp
