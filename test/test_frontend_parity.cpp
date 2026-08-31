// Lexicon "Required flows" -> "Ordinary input": frontends parse external
// input into AgentInput, invoke an agent run, and observe/render the same
// resulting events; "Frontend / adapter" says all three frontends should
// "call the same runtime behavior." This test drives one equivalent prompt
// through the CLI-equivalent path (core::SessionRuntime directly), JSONL RPC
// (cli::RpcMode), and ACP (a real in-process HTTP server, per
// test_acp.cpp's fixture pattern) and asserts they observe the same
// assistant text for one full turn. See
// plans/session-runtime-migration.md Phase 1 item 5.
//
// This is deliberately a single-turn, tool-free scenario (no tool call) to
// keep the three drivers comparable without depending on Phase 2's not-yet-
// built shared construction; each leg wires its own minimal
// SessionRuntime/ACP server by hand, matching how cmd_run() and acp/main.cpp
// each do so independently today.
//
// Phase 2 closed item 1 (model/auth/sandbox resolution parity) by
// extracting cli::resolve_model_selection() (cli/session_runtime.h) as the
// single function both cmd_run() (src/main.cpp) and pi-acp's main() now
// call -- see their call sites. That makes "parity" hold by construction
// (one function, two callers), so the test below is really a
// characterization/regression suite for resolve_model_selection() itself:
// it pins the cases that differed between the two frontends' previous,
// independent implementations before unification (see
// cli/session_runtime.h's resolve_model_selection() doc comment), so a
// future edit can't silently reopen that drift for either caller.

#include "acp/server.h"
#include "cli/args.h"
#include "cli/rpc_mode.h"
#include "cli/session_runtime.h"
#include "core/agent.h"
#include "core/builtin_tools.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/providers/faux.h"
#include "core/session/agent_session.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <httplib.h>
#include <nlohmann/json.hpp>

using namespace pi;

namespace {

std::shared_ptr<const core::ModelCatalog> resolution_test_registry() {
  core::ProviderConfig provider;
  provider.id = "known-provider";
  provider.api = "openai-completions";
  provider.base_url = "http://known-provider.test/v1";
  provider.auth = core::ProviderAuthPolicy::none;
  core::ConfiguredModel configured;
  configured.id = "known-model";
  configured.name = "known-model";
  provider.models.push_back(configured);
  return std::make_shared<const core::ModelCatalog>(
      std::map<std::string, core::ProviderConfig>{
          {"known-provider", provider}});
}

void test_resolution_parity() {
  const auto registry = resolution_test_registry();

  // Explicit --model resolves through the registry.
  {
    cli::Args args;
    args.model = "known-provider/known-model";
    const auto resolution = cli::resolve_model_selection(args, registry);
    EXPECT_TRUE(static_cast<bool>(resolution));
    if (resolution)
      EXPECT_EQ(resolution.model->provider, std::string("known-provider"));
  }

  // Explicit --provider naming a known provider builds its default model.
  {
    cli::Args args;
    args.provider = "known-provider";
    const auto resolution = cli::resolve_model_selection(args, registry);
    EXPECT_TRUE(static_cast<bool>(resolution));
    if (resolution)
      EXPECT_EQ(resolution.model->base_url,
                std::string("http://known-provider.test/v1"));
  }

  // Unknown --provider with no --base-url errors.
  {
    cli::Args args;
    args.provider = "no-such-provider";
    const auto resolution = cli::resolve_model_selection(args, registry);
    EXPECT_TRUE(!resolution);
  }

  // Unknown --provider combined with an explicit --base-url falls through
  // to a permissive default-model construction using that base_url,
  // instead of erroring. This is the CLI's pre-existing, more permissive
  // behavior; ACP previously always errored here (see
  // cli/session_runtime.h's resolve_model_selection() doc comment) --
  // unification resolved it in the CLI's favor. Pinning this prevents that
  // divergence from silently reopening.
  {
    cli::Args args;
    args.provider = "no-such-provider";
    args.base_url = "http://custom.test/v1";
    const auto resolution = cli::resolve_model_selection(args, registry);
    EXPECT_TRUE(static_cast<bool>(resolution));
    if (resolution)
      EXPECT_EQ(resolution.model->base_url,
                std::string("http://custom.test/v1"));
  }

  // No --model/--provider falls back to the local default.
  {
    cli::Args args;
    const auto resolution = cli::resolve_model_selection(args, registry);
    EXPECT_TRUE(static_cast<bool>(resolution));
    if (resolution)
      EXPECT_EQ(resolution.model->provider, std::string("local"));
  }
}

constexpr std::string_view kParityText = "parity check response text";

core::FauxClient::Script single_turn_script() {
  core::AssistantMessage final_message;
  final_message.content.emplace_back(
      core::TextContent{std::string(kParityText)});
  final_message.stop_reason = core::StopReason::stop;
  core::FauxClient::Script script;
  script.events = {
      core::AssistantMessageEvent{core::AssistantMessageStartEvent{}},
      core::AssistantMessageEvent{core::AssistantMessageTextDeltaEvent{
          0, std::string(kParityText), {}}},
      core::AssistantMessageEvent{core::AssistantMessageDoneEvent{
          core::StopReason::stop, final_message}},
  };
  return script;
}

// Leg 1: CLI-equivalent — core::SessionRuntime::run_prompt directly, the same
// call cmd_run() drives (src/main.cpp).
std::string drive_cli_leg() {
  core::LLMClientRegistry::instance().register_client("parity-cli-faux", [] {
    return std::make_shared<core::FauxClient>(
        std::vector{single_turn_script()});
  });

  core::Model model;
  model.id = "parity-model";
  model.api = "parity-cli-faux";
  model.provider = "parity-cli-faux";
  core::Agent::Options options;
  options.model = model;

  core::SessionRuntime session({.agent_options = options});
  const auto result = session.run_prompt("ping");
  EXPECT_TRUE(!result.error.has_value());

  const auto &messages = session.agent().state().messages();
  for (const auto &message : messages | std::views::reverse) {
    if (const auto *assistant = std::get_if<core::AssistantMessage>(&message)) {
      for (const auto &block : assistant->content)
        if (const auto *text = std::get_if<core::TextContent>(&block))
          return text->text;
    }
  }
  return {};
}

// Leg 2: JSONL RPC — cli::RpcMode, the same class src/cli/rpc_mode.cpp
// drives from cmd_run()'s RPC mode.
std::string drive_rpc_leg() {
  core::LLMClientRegistry::instance().register_client("parity-rpc-faux", [] {
    return std::make_shared<core::FauxClient>(
        std::vector{single_turn_script()});
  });

  core::Model model;
  model.id = "parity-model";
  model.api = "parity-rpc-faux";
  model.provider = "parity-rpc-faux";
  core::Agent::Options options;
  options.model = model;

  core::SessionRuntime session({.agent_options = options});

  std::mutex mutex;
  std::vector<nlohmann::json> output;
  cli::RpcMode mode(session, [&](const nlohmann::json &line) {
    std::scoped_lock lock(mutex);
    output.push_back(line);
  });
  mode.handle({{"id", "prompt"}, {"type", "prompt"}, {"message", "ping"}});
  mode.wait_for_idle();

  std::scoped_lock lock(mutex);
  for (const auto &line : output) {
    if (line.value("event", "") == "message_update" &&
        line["data"].value("kind", "") == "text_delta")
      return line["data"].value("delta", "");
  }
  return {};
}

// Leg 3: ACP — a real in-process HTTP server (pi::acp::run_server), the same
// entry point acp/main.cpp uses; mirrors test_acp.cpp's AcpFixture pattern.
std::string drive_acp_leg() {
  core::LLMClientRegistry::instance().register_client("parity-acp-faux", [] {
    return std::make_shared<core::FauxClient>(
        std::vector{single_turn_script()});
  });

  acp::ServerConfig cfg;
  cfg.agent_name = "parity-agent";
  cfg.agent_description = "frontend parity test agent";
  cfg.threads = 2;

  core::Model model;
  model.id = "parity-model";
  model.api = "parity-acp-faux";
  model.provider = "parity-acp-faux";
  model.context_window = 4096;
  model.max_tokens = 512;
  cfg.agent_opts.model = model;

  core::ProviderConfig provider;
  provider.id = "parity-acp-faux";
  provider.api = "parity-acp-faux";
  provider.base_url = "http://parity.test/v1";
  provider.auth = core::ProviderAuthPolicy::none;
  core::ConfiguredModel configured;
  configured.id = "parity-model";
  provider.models.push_back(configured);
  cfg.model_catalog = std::make_shared<const core::ModelCatalog>(
      std::map<std::string, core::ProviderConfig>{
          {"parity-acp-faux", provider}});
  cfg.agent_opts.model_catalog = cfg.model_catalog;
  cfg.agent_opts.get_api_key =
      [](std::string_view) -> std::optional<std::string> {
    return std::nullopt;
  };
  cfg.tools = core::create_all_tools(std::filesystem::temp_directory_path());

  std::atomic<int> port{0};
  std::thread server_thread([cfg = std::move(cfg), &port]() mutable {
    acp::run_server(port, std::move(cfg));
  });
  server_thread.detach();
  for (int attempt = 0; attempt < 50 && port == 0; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (port == 0)
    return {};

  httplib::Client http_client("127.0.0.1", port.load());
  nlohmann::json body = {
      {"agent_name", "parity-agent"},
      {"mode", "sync"},
      {"input",
       {{{"role", "user"},
         {"parts", {{{"content_type", "text/plain"}, {"content", "ping"}}}}}}}};
  auto response = http_client.Post("/runs", body.dump(), "application/json");
  if (response == nullptr || response->status != 200)
    return {};
  const auto parsed = nlohmann::json::parse(response->body);
  if (!parsed.contains("output") || parsed["output"].empty())
    return {};
  for (const auto &part : parsed["output"][0]["parts"])
    if (part.value("content_type", "") == "text/plain")
      return part.value("content", "");
  return {};
}

// plans/session-runtime-migration.md Phase 7's definition of done: with
// durable_run_mutex removed, concurrent ACP sessions must not
// cross-contaminate transcripts. core::Agent rejects a second concurrent
// run against *the same* instance (see the audit in src/acp/handlers.cpp
// above its old durable_run_mutex declaration), so two concurrent /runs
// calls against two *different* session_ids both succeeding is itself
// proof they were served by two distinct SessionRuntimes, not a shared one
// silently splicing the two conversations together; each response's own
// echoed session_id then confirms which is which. test_acp.cpp's
// test_run_concurrent_sessions_no_blocking/
// test_run_concurrent_same_session_conflict cover the same registry with a
// deliberately slow script and a same-session collision; this leg reuses
// this file's own single-turn ACP setup instead of building a new fixture,
// per the plan's "use it directly" guidance.
void test_acp_concurrent_session_isolation() {
  core::LLMClientRegistry::instance().register_client(
      "parity-concurrent-faux", [] {
        return std::make_shared<core::FauxClient>(
            std::vector{single_turn_script()});
      });

  acp::ServerConfig cfg;
  cfg.agent_name = "parity-concurrent-agent";
  cfg.agent_description = "frontend parity concurrency test agent";
  cfg.threads = 2;

  core::Model model;
  model.id = "parity-concurrent-model";
  model.api = "parity-concurrent-faux";
  model.provider = "parity-concurrent-faux";
  model.context_window = 4096;
  model.max_tokens = 512;
  cfg.agent_opts.model = model;

  core::ProviderConfig provider;
  provider.id = "parity-concurrent-faux";
  provider.api = "parity-concurrent-faux";
  provider.base_url = "http://parity-concurrent.test/v1";
  provider.auth = core::ProviderAuthPolicy::none;
  core::ConfiguredModel configured;
  configured.id = "parity-concurrent-model";
  provider.models.push_back(configured);
  cfg.model_catalog = std::make_shared<const core::ModelCatalog>(
      std::map<std::string, core::ProviderConfig>{
          {"parity-concurrent-faux", provider}});
  cfg.agent_opts.model_catalog = cfg.model_catalog;
  cfg.agent_opts.get_api_key =
      [](std::string_view) -> std::optional<std::string> {
    return std::nullopt;
  };
  cfg.tools = core::create_all_tools(std::filesystem::temp_directory_path());

  std::atomic<int> port{0};
  std::thread server_thread([cfg = std::move(cfg), &port]() mutable {
    acp::run_server(port, std::move(cfg));
  });
  server_thread.detach();
  for (int attempt = 0; attempt < 50 && port == 0; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  if (port == 0) {
    ADD_FAILURE() << "ACP parity server failed to start";
    return;
  }

  auto request_json = [](const std::string &session_id) {
    return nlohmann::json{
        {"agent_name", "parity-concurrent-agent"},
        {"mode", "sync"},
        {"session_id", session_id},
        {"input",
         {{{"role", "user"},
           {"parts",
            {{{"content_type", "text/plain"}, {"content", "ping"}}}}}}}}
        .dump();
  };

  httplib::Result result_a;
  httplib::Result result_b;
  std::thread thread_a([&] {
    httplib::Client client("127.0.0.1", port.load());
    result_a = client.Post("/runs", request_json("parity-concurrent-a"),
                           "application/json");
  });
  std::thread thread_b([&] {
    httplib::Client client("127.0.0.1", port.load());
    result_b = client.Post("/runs", request_json("parity-concurrent-b"),
                           "application/json");
  });
  thread_a.join();
  thread_b.join();

  if (result_a == nullptr || result_b == nullptr) {
    ADD_FAILURE() << "concurrent ACP parity request failed";
    return;
  }
  EXPECT_EQ(result_a->status, 200);
  EXPECT_EQ(result_b->status, 200);
  const auto parsed_a = nlohmann::json::parse(result_a->body);
  const auto parsed_b = nlohmann::json::parse(result_b->body);
  EXPECT_EQ(parsed_a.value("session_id", ""),
            std::string("parity-concurrent-a"));
  EXPECT_EQ(parsed_b.value("session_id", ""),
            std::string("parity-concurrent-b"));
}

} // namespace

TEST(FrontendParity, ResolutionAndCrossFrontendBehavior) {
  test_resolution_parity();

  const auto cli_text = drive_cli_leg();
  const auto rpc_text = drive_rpc_leg();
  const auto acp_text = drive_acp_leg();

  EXPECT_EQ(cli_text, std::string(kParityText));
  EXPECT_EQ(rpc_text, std::string(kParityText));
  EXPECT_EQ(acp_text, std::string(kParityText));
}

TEST(FrontendParity, ConcurrentAcpSessionIsolation) {
  test_acp_concurrent_session_isolation();
}
