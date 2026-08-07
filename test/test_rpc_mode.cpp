#include "cli/rpc_mode.h"
#include "core/llm_client.h"
#include "core/providers/faux.h"
#include "core/session/session_store.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <mutex>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace pi;

namespace tests {
int passed{0}, failed{0};
bool check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
  if (condition) {
    ++passed;
    return true;
  }
  ++failed;
  std::cerr << location.file_name() << ':' << location.line() << ": "
            << expression << "\n";
  return false;
}
} // namespace tests

#define CHECK(expr) tests::check(!!(expr), #expr)

int main() {
  core::AssistantMessage final_message;
  final_message.content.push_back(core::TextContent{.text = "rpc reply"});
  core::FauxClient::Script script;
  script.events = {
      core::AssistantMessageEvent{core::AssistantMessageStartEvent{}},
      core::AssistantMessageEvent{
          core::AssistantMessageTextDeltaEvent{0, "rpc reply", {}}},
      core::AssistantMessageEvent{core::AssistantMessageDoneEvent{
          core::StopReason::stop, final_message}},
  };
  core::LLMClientRegistry::instance().register_client("rpc-faux", [script] {
    return std::make_shared<core::FauxClient>(std::vector{script});
  });

  core::Model model;
  model.id = "faux";
  model.name = "faux";
  model.api = "rpc-faux";
  model.provider = "faux";
  auto store = std::make_shared<core::SessionStore>(
      std::filesystem::temp_directory_path() / "pici-rpc-mode-test");
  core::ProviderConfig provider;
  provider.id = "faux";
  provider.api = "rpc-faux";
  provider.base_url = "http://faux.test/v1";
  provider.auth = core::ProviderAuthPolicy::none;
  core::ConfiguredModel configured;
  configured.id = "faux";
  provider.models.push_back(configured);
  core::ConfiguredModel other;
  other.id = "other";
  provider.models.push_back(other);
  auto registry = std::make_shared<const core::ModelRegistry>(
      std::map<std::string, core::ProviderConfig>{{"faux", provider}});

  core::Agent::Options options;
  options.model = std::move(model);
  options.model_registry = registry;
  core::AgentSession session({.agent_options = std::move(options),
                              .model_registry = registry,
                              .session_store = store});
  core::SessionHeader header{.id = "rpc-test"};
  session.create_session(header);

  std::mutex mutex;
  std::vector<nlohmann::json> output;
  cli::RpcMode mode(session, [&](const nlohmann::json &line) {
    std::scoped_lock lock(mutex);
    output.push_back(line);
  });

  mode.handle({{"id", "models"}, {"type", "list_models"}});
  mode.handle({{"id", "switch"}, {"type", "set_model"},
               {"provider", "faux"}, {"model", "other"}});
  mode.handle({{"id", "state"}, {"type", "get_state"}});
  mode.handle({{"id", "prompt"}, {"type", "prompt"}, {"message", "hello"}});
  mode.wait_for_idle();
  mode.handle({{"id", "messages"}, {"type", "get_messages"}});
  mode.handle(
      {{"id", "name"}, {"type", "set_session_name"}, {"name", "RPC test"}});

  bool got_state = false;
  bool got_models = false;
  bool got_switch = false;
  bool got_ack = false;
  bool got_delta = false;
  bool got_sequence = false;
  bool got_complete = false;
  bool got_messages = false;
  bool got_name = false;
  std::scoped_lock lock(mutex);
  for (const auto &line : output) {
    got_state = got_state || (line.value("command", "") == "get_state" &&
                              line.value("success", false) &&
                              line["data"]["model"]["id"] == "other");
    got_models = got_models ||
                 (line.value("command", "") == "list_models" &&
                  line.value("success", false) &&
                  line["data"]["models"].size() >= 2);
    got_switch = got_switch ||
                 (line.value("command", "") == "set_model" &&
                  line.value("success", false) &&
                  line["data"]["current"]["id"] == "other");
    got_ack = got_ack || (line.value("command", "") == "prompt" &&
                          line.value("success", false));
    got_delta = got_delta || (line.value("event", "") == "message_update" &&
                              line["data"].value("kind", "") == "text_delta" &&
                              line["data"].value("delta", "") == "rpc reply");
    got_sequence = got_sequence ||
                   (line.value("type", "") == "event" &&
                    line.value("sequence", 0ULL) > 0ULL);
    got_complete = got_complete || line.value("type", "") == "run.completed";
    got_messages =
        got_messages ||
        (line.value("command", "") == "get_messages" &&
         line.value("success", false) && line["data"]["messages"].size() == 2);
    got_name = got_name || (line.value("command", "") == "set_session_name" &&
                            line.value("success", false));
  }
  CHECK(got_state);
  CHECK(got_models);
  CHECK(got_switch);
  CHECK(got_ack);
  CHECK(got_delta);
  CHECK(got_sequence);
  CHECK(got_complete);
  CHECK(got_messages);
  CHECK(got_name);
  std::cout << "rpc mode: " << tests::passed << " passed, " << tests::failed
            << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
