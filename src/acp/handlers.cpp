#include "acp/handlers.h"
#include "acp/server.h"
#include "acp/session_store.h"
#include "acp/sse.h"
#include "acp/types.h"
#include "core/agent.h"
#include "core/message_types.h"
#include "core/stream_renderer.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <httplib.h>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pi::acp {

namespace {

// Translates Renderer callbacks into ACP SSE events.

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

// ─────────────────────────────────────────────────────────────

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

// ───────────────────────────────────────────────────────

void register_routes(httplib::Server &svr, const ServerConfig &cfg,
                     const std::shared_ptr<SessionStore> &sessions) {
  const auto manifest_json = nlohmann::json(build_manifest(cfg));

  // ── GET /health ────────────────────────────────────────────────────────────
  svr.Get("/health", [](const httplib::Request &, httplib::Response &res) {
    res.set_content(R"({"status":"ok"})", "application/json");
  });

  // ── GET /agents ────────────────────────────────────────────────────────────
  svr.Get("/agents",
          [manifest_json](const httplib::Request &, httplib::Response &res) {
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
          if (!prompt.empty())
            prompt += '\n';
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
      if (!hist.empty())
        agent_ptr->state().set_messages(std::move(hist));
    }

    // ── Streaming mode ─────────────────────────────────────────────────────
    if (rcr.mode == RunMode::stream) {
      res.set_chunked_content_provider(
          "text/event-stream",
          [&cfg, sessions, run_id, prompt, rcr, agent_ptr](
              std::size_t /*offset*/, httplib::DataSink &sink) mutable -> bool {
            SseWriter sse(sink);

            Run r;
            r.run_id = run_id;
            r.agent_name = cfg.agent_name;
            r.status = RunStatus::created;
            if (rcr.session_id)
              r.session_id = rcr.session_id;
            sse.emit("run.created",
                     {{"type", "run.created"}, {"run", nlohmann::json(r)}});
            r.status = RunStatus::in_progress;
            sse.emit("run.in-progress",
                     {{"type", "run.in-progress"}, {"run", nlohmann::json(r)}});

            AcpSseRenderer renderer(sse, cfg.agent_name);
            try {
              for (const auto &ev : agent_ptr->prompt(prompt))
                core::dispatch_event(ev, renderer);
            } catch (const std::exception &ex) {
              renderer.on_error(core::RendererErrorKind::unknown, ex.what());
            }

            if (rcr.session_id)
              sessions->save(*rcr.session_id, agent_ptr->state().messages());

            renderer.emit_run_final(r);
            sink.done();
            return true;
          });
      return;
    }

    // ── Sync mode ──────────────────────────────────────────────────────────
    SyncRenderer sr;
    try {
      for (const auto &ev : agent_ptr->prompt(prompt))
        core::dispatch_event(ev, sr);
    } catch (const std::exception &ex) {
      sr.on_error(core::RendererErrorKind::unknown, ex.what());
    }

    if (rcr.session_id)
      sessions->save(*rcr.session_id, agent_ptr->state().messages());

    Run r;
    r.run_id = run_id;
    r.agent_name = cfg.agent_name;
    r.status = sr.status();
    r.session_id = rcr.session_id;
    if (!sr.accumulated().empty())
      r.output.push_back({.role = cfg.agent_name,
                          .parts = {{.content_type = "text/plain",
                                     .content = sr.accumulated()}}});
    if (sr.status() == RunStatus::failed)
      r.error = sr.error();

    json_response(res, 200, nlohmann::json(r));
  });

  // ── GET /runs/{run_id} ─────────────────────────────────────────────────────
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
