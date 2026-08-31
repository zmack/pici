#include "cli/faux_control_mode.h"

#include "core/agent.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/providers/faux.h"
#include "core/providers/faux_control.h"
#include "core/session/session_runtime.h"
#include "core/session/session_record.h"
#include "core/session/session_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace pi;

namespace {

std::shared_ptr<const core::ModelCatalog> make_model_catalog() {
  core::ProviderConfig provider;
  provider.id = "faux-mode";
  provider.api = "faux-mode-test";
  provider.base_url = "http://faux.test/v1";
  provider.auth = core::ProviderAuthPolicy::none;
  core::ConfiguredModel configured;
  configured.id = "faux-model";
  provider.models.push_back(configured);
  return std::make_shared<const core::ModelCatalog>(
      std::map<std::string, core::ProviderConfig>{{"faux-mode", provider}});
}

core::SessionRuntime::Config make_session_config(
    const std::shared_ptr<const core::ModelCatalog> &model_catalog,
    const std::shared_ptr<core::SessionStore> &store) {
  core::Agent::Options options;
  options.model = core::Model{
      .id = "faux-model", .api = "faux-mode-test", .provider = "faux-mode"};
  options.model_catalog = model_catalog;
  return {.agent_options = std::move(options),
          .model_catalog = model_catalog,
          .session_store = store};
}

struct Fixture {
  std::shared_ptr<core::RemoteFauxClient> client =
      std::make_shared<core::RemoteFauxClient>();
  std::shared_ptr<core::ScriptedToolRegistry> tool_registry =
      std::make_shared<core::ScriptedToolRegistry>();
  std::shared_ptr<const core::ModelCatalog> model_catalog =
      make_model_catalog();
  std::shared_ptr<core::SessionStore> store =
      std::make_shared<core::SessionStore>(
          std::filesystem::temp_directory_path() /
          ("pici-faux-control-mode-" + std::to_string(::getpid())));
  core::SessionRuntime session;

  Fixture() : session(make_session_config(model_catalog, store)) {
    core::LLMClientRegistry::instance().register_client(
        "faux-mode-test", [client = client] { return client; });
    session.create_session(core::SessionHeader{.id = "faux-control-mode-test"});
  }
};

core::FauxClient::Script done_script() {
  core::AssistantMessage message;
  message.stop_reason = core::StopReason::stop;
  return {.events = {core::AssistantMessageDoneEvent{core::StopReason::stop,
                                                     message}}};
}

nlohmann::json end_round(std::string id = "round") {
  return {{"type", "round"},
          {"id", std::move(id)},
          {"stop_reason", "end_turn"},
          {"content", nlohmann::json::array()}};
}

int connect_socket(const std::string &path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  for (int attempt = 0; attempt < 100; ++attempt) {
    if (::connect(
            fd,
            reinterpret_cast<const sockaddr *>(
                &address), // NOLINT(cppcoreguidelines-pro-type-reinterpret-cast)
            sizeof(address)) == 0)
      return fd;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ::close(fd);
  return -1;
}

bool send_all(int fd, std::string_view data) {
  std::size_t sent = 0;
  while (sent < data.size()) {
    const auto count =
        ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
    if (count <= 0)
      return false;
    sent += static_cast<std::size_t>(count);
  }
  return true;
}

std::optional<nlohmann::json> read_line(int fd) {
  std::string line;
  char character = 0;
  while (true) {
    const auto count = ::recv(fd, &character, 1, 0);
    if (count <= 0)
      return std::nullopt;
    if (character == '\n')
      return nlohmann::json::parse(line, nullptr, false);
    line.push_back(character);
  }
}

} // namespace

TEST(FauxControlMode, SocketProtocolAndMailboxFlow) {
  {
    Fixture fixture;
    std::vector<nlohmann::json> output;
    std::vector<std::string> registered;
    int observed_events = 0;
    cli::FauxControlMode mode(
        fixture.session, *fixture.client, fixture.tool_registry,
        [&output](const nlohmann::json &value) { output.push_back(value); },
        [&registered](const std::string &name) { registered.push_back(name); },
        [&observed_events](const core::AgentEvent &) { ++observed_events; });

    mode.handle(nlohmann::json::array());
    EXPECT_TRUE(!output.empty() && !output.back().value("success", true));

    auto malformed_round = end_round("bad");
    malformed_round["content"] = {{{"type", "unknown"}}};
    mode.handle(malformed_round);
    EXPECT_TRUE(!output.back().value("success", true));

    auto tool_round = end_round("tool");
    tool_round["stop_reason"] = "tool_calls";
    tool_round["content"] = {{{"type", "tool_call"},
                              {"call_id", "call-1"},
                              {"name", "scripted"},
                              {"args", {{"value", 1}}}}};
    mode.handle(tool_round);
    mode.handle(tool_round);
    EXPECT_TRUE(output.back().value("success", false));
    EXPECT_TRUE(registered == std::vector<std::string>{"scripted"});

    mode.handle(end_round("turn"));
    mode.handle({{"type", "turn"}, {"id", "first"}});
    mode.handle({{"type", "turn"}, {"id", "second"}});
    EXPECT_TRUE(!output.back().value("success", true));
    mode.handle({{"type", "round"},
                 {"stop_reason", "end_turn"},
                 {"content", nlohmann::json::array()}});
    mode.wait_for_idle();
    bool completed = false;
    for (const auto &value : output)
      completed = completed || value.value("type", "") == "turn.completed";
    EXPECT_TRUE(completed);
    EXPECT_TRUE(observed_events > 0);

    mode.handle({{"type", "turn"},
                 {"id", "bad-prompt"},
                 {"prompt", {{"text", "bad"}, {"source", "invalid"}}}});
    EXPECT_TRUE(!output.back().value("success", true));

    auto reject_prompt = [&](nlohmann::json prompt, const char *id) {
      mode.handle(
          {{"type", "turn"}, {"id", id}, {"prompt", std::move(prompt)}});
      EXPECT_TRUE(!output.back().value("success", true));
    };
    reject_prompt(nlohmann::json::array(), "prompt-array");
    reject_prompt(nlohmann::json::object(), "prompt-missing-text");
    reject_prompt({{"text", 3}}, "prompt-number-text");
    reject_prompt({{"text", "text"}, {"source", 3}}, "prompt-number-source");
    reject_prompt({{"text", "text"}, {"message_id", 3}},
                  "prompt-number-metadata");

    fixture.client->push_round(done_script());
    mode.handle(
        {{"type", "turn"},
         {"id", "ordinary-prompt"},
         {"prompt", {{"text", "ordinary request"}, {"source", "ordinary"}}}});
    mode.wait_for_idle();
    bool saw_ordinary_request = false;
    for (const auto &value : output) {
      if (value.value("event", "") != "message_start" ||
          !value["data"].contains("request"))
        continue;
      const auto &request = value["data"]["request"];
      saw_ordinary_request =
          saw_ordinary_request || request.value("source", "") == "ordinary";
    }
    EXPECT_TRUE(saw_ordinary_request);

    fixture.client->push_round(done_script());
    mode.handle({{"type", "turn"},
                 {"id", "mailbox-prompt"},
                 {"prompt",
                  {{"text", "mailbox request"},
                   {"source", "mailbox"},
                   {"message_id", "message-1"},
                   {"sender_task_path", "/root/luna"}}}});
    mode.wait_for_idle();
    bool saw_mailbox_request = false;
    for (const auto &value : output) {
      if (value.value("event", "") != "message_start" ||
          !value["data"].contains("request"))
        continue;
      const auto &request = value["data"]["request"];
      saw_mailbox_request =
          saw_mailbox_request ||
          (request.value("source", "") == "mailbox" &&
           request.value("message_id", "") == "message-1" &&
           request.value("sender_task_path", "") == "/root/luna");
    }
    EXPECT_TRUE(saw_mailbox_request);

    mode.handle({{"type", "quit"}, {"id", "quit"}});
    EXPECT_TRUE(mode.quit_requested());
    EXPECT_TRUE(output.back().value("success", false));
  }

  {
    Fixture fixture;
    std::vector<nlohmann::json> output;
    std::mutex output_mutex;
    cli::FauxControlMode mode(
        fixture.session, *fixture.client, fixture.tool_registry,
        [&output, &output_mutex](const nlohmann::json &value) {
          std::scoped_lock lock(output_mutex);
          output.push_back(value);
        });
    std::thread first(
        [&] { mode.handle({{"type", "turn"}, {"id", "first"}}); });
    std::thread second(
        [&] { mode.handle({{"type", "turn"}, {"id", "second"}}); });
    first.join();
    second.join();
    fixture.client->push_round(done_script());
    mode.wait_for_idle();
    int accepted = 0;
    int rejected = 0;
    for (const auto &value : output) {
      if (value.value("type", "") == "response") {
        accepted += value.value("success", false) ? 1 : 0;
        rejected += value.value("success", true) ? 0 : 1;
      }
    }
    EXPECT_TRUE(accepted == 1);
    EXPECT_TRUE(rejected == 1);
    EXPECT_TRUE(output.back().value("type", "") == "turn.completed");
    mode.stop();
  }

  {
    Fixture fixture;
    std::vector<nlohmann::json> output;
    cli::FauxControlMode mode(
        fixture.session, *fixture.client, fixture.tool_registry,
        [&output](const nlohmann::json &value) { output.push_back(value); });
    std::thread external([&] { fixture.session.run_prompt("", {}); });
    bool streaming = false;
    for (int attempt = 0; attempt < 100 && !streaming; ++attempt) {
      streaming = fixture.session.agent().is_streaming();
      if (!streaming)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(streaming);
    mode.handle({{"type", "turn"}, {"id", "external"}});
    EXPECT_TRUE(!output.back().value("success", true));
    fixture.client->push_round(done_script());
    external.join();
    fixture.client->push_round(done_script());
    mode.handle({{"type", "turn"}, {"id", "after-external"}});
    mode.wait_for_idle();
    EXPECT_TRUE(output.back().value("type", "") == "turn.completed");
    mode.stop();
  }

  {
    Fixture fixture;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("pici-faux-drop-" + std::to_string(::getpid()) + ".sock");
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    int server_result = -1;
    std::thread server([&] {
      server_result =
          cli::run_faux_control_socket(fixture.session, *fixture.client,
                                       fixture.tool_registry, path.string());
    });
    int fd = -1;
    for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
      fd = connect_socket(path.string());
      if (fd < 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(fd >= 0);
    if (fd >= 0) {
      EXPECT_TRUE(send_all(fd, R"({"type":"turn","id":"drop"})"));
      EXPECT_TRUE(send_all(fd, "\n"));
      const auto ack = read_line(fd);
      EXPECT_TRUE(ack.has_value() && ack->value("success", false));
      ::close(fd);
    }
    fd = connect_socket(path.string());
    EXPECT_TRUE(fd >= 0);
    if (fd >= 0) {
      EXPECT_TRUE(send_all(fd, R"({"type":"quit","id":"quit"})"));
      EXPECT_TRUE(send_all(fd, "\n"));
      const auto ack = read_line(fd);
      EXPECT_TRUE(ack.has_value() && ack->value("success", false));
      ::close(fd);
    }
    server.join();
    EXPECT_TRUE(server_result == 0);
    EXPECT_TRUE(!std::filesystem::exists(path));
  }

  {
    Fixture fixture;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("pici-faux-idle-reconnect-" + std::to_string(::getpid()) + ".sock");
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    int server_result = -1;
    std::thread server([&] {
      server_result =
          cli::run_faux_control_socket(fixture.session, *fixture.client,
                                       fixture.tool_registry, path.string());
    });
    int fd = -1;
    for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
      fd = connect_socket(path.string());
      if (fd < 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(fd >= 0);
    if (fd >= 0) {
      ::close(fd);
    }
    fd = connect_socket(path.string());
    EXPECT_TRUE(fd >= 0);
    if (fd >= 0) {
      EXPECT_TRUE(send_all(fd, R"({"type":"turn","id":"reconnect-turn"})"));
      EXPECT_TRUE(send_all(fd, "\n"));
      const auto turn_ack = read_line(fd);
      EXPECT_TRUE(turn_ack.has_value() && turn_ack->value("success", false));
      const auto round = end_round("idle-round").dump() + "\n";
      EXPECT_TRUE(send_all(fd, round));
      bool round_acked = false;
      bool completed = false;
      for (int attempt = 0; attempt < 100 && !(round_acked && completed);
           ++attempt) {
        const auto event = read_line(fd);
        if (!event.has_value())
          break;
        round_acked = round_acked || (event->value("command", "") == "round" &&
                                      event->value("success", false));
        completed = completed || event->value("type", "") == "turn.completed";
      }
      EXPECT_TRUE(round_acked);
      EXPECT_TRUE(completed);
      EXPECT_TRUE(send_all(fd, R"({"type":"quit","id":"quit"})"));
      EXPECT_TRUE(send_all(fd, "\n"));
      const auto quit_ack = read_line(fd);
      EXPECT_TRUE(quit_ack.has_value() && quit_ack->value("success", false));
      ::close(fd);
    }
    server.join();
    EXPECT_TRUE(server_result == 0);
    EXPECT_TRUE(!std::filesystem::exists(path));
  }
  {
    Fixture fixture;
    const auto path =
        std::filesystem::temp_directory_path() /
        ("pici-faux-control-" + std::to_string(::getpid()) + ".sock");
    std::error_code cleanup_error;
    std::filesystem::remove(path, cleanup_error);
    const auto regular_path = path.string() + ".regular";
    {
      std::ofstream regular(regular_path);
      regular << "keep";
    }
    EXPECT_TRUE(cli::run_faux_control_socket(fixture.session, *fixture.client,
                                             fixture.tool_registry,
                                             regular_path) == 1);
    EXPECT_TRUE(std::filesystem::exists(regular_path));
    std::filesystem::remove(regular_path, cleanup_error);

    int server_result = -1;
    std::vector<std::string> registered;
    std::thread server([&] {
      server_result = cli::run_faux_control_socket(
          fixture.session, *fixture.client, fixture.tool_registry,
          path.string(), [&registered](const std::string &name) {
            registered.push_back(name);
          });
    });

    int fd = -1;
    for (int attempt = 0; attempt < 100 && fd < 0; ++attempt) {
      fd = connect_socket(path.string());
      if (fd < 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    EXPECT_TRUE(fd >= 0);
    if (fd >= 0) {
      const auto round = end_round("socket-round").dump() + "\n";
      EXPECT_TRUE(send_all(fd, round.substr(0, 7)));
      EXPECT_TRUE(send_all(fd, round.substr(7)));
      const auto round_ack = read_line(fd);
      EXPECT_TRUE(round_ack.has_value() && round_ack->value("success", false));

      EXPECT_TRUE(send_all(fd, "{bad-json}\n\n{"));
      EXPECT_TRUE(send_all(fd, "\"type\":\"unknown\"}\n"));
      const auto invalid = read_line(fd);
      const auto unknown = read_line(fd);
      EXPECT_TRUE(invalid.has_value() &&
                  invalid->value("error", "") == "invalid JSON");
      EXPECT_TRUE(unknown.has_value() && !unknown->value("success", true));

      EXPECT_TRUE(send_all(fd, R"({"type":"quit","id":"quit"})"));
      EXPECT_TRUE(send_all(fd, "\n"));
      const auto quit_ack = read_line(fd);
      EXPECT_TRUE(quit_ack.has_value() && quit_ack->value("success", false));
      ::close(fd);
    }
    server.join();
    EXPECT_TRUE(server_result == 0);
    EXPECT_TRUE(!std::filesystem::exists(path));
  }

  // Deterministic regression driver (plan milestone 6, item 1): an ordinary
  // request whose two tool calls finish out of call order, followed by a
  // final answer; and a mailbox request whose reply is queued before the
  // final acknowledgement. Assertions read the structured event stream
  // rather than painted terminal output, which pure frame tests already
  // cover in test_region_renderer.cpp.
  {
    Fixture fixture;
    std::vector<nlohmann::json> output;
    auto &agent = fixture.session.agent();
    cli::FauxControlMode mode(
        fixture.session, *fixture.client, fixture.tool_registry,
        [&output](const nlohmann::json &value) { output.push_back(value); },
        [&agent,
         tool_registry = fixture.tool_registry](const std::string &name) {
          agent.add_tool(
              std::make_shared<core::ScriptedTool>(name, tool_registry));
        });

    mode.handle(
        {{"type", "round"},
         {"id", "ordinary-tools"},
         {"stop_reason", "tool_calls"},
         {"content",
          {{{"type", "text"},
            {"text", "Checking how tool blocks are inserted..."}},
           {{"type", "tool_call"},
            {"call_id", "slow-call"},
            {"name", "slow-tool"},
            {"args", nlohmann::json::object()},
            {"result", {{"content", "slow result"}, {"is_error", false}}},
            {"finish_after_ms", 60}},
           {{"type", "tool_call"},
            {"call_id", "fast-call"},
            {"name", "fast-tool"},
            {"args", nlohmann::json::object()},
            {"result", {{"content", "fast result"}, {"is_error", false}}},
            {"finish_after_ms", 5}}}}});
    mode.handle(
        {{"type", "round"},
         {"id", "ordinary-closing"},
         {"stop_reason", "end_turn"},
         {"content",
          {{{"type", "text"},
            {"text", "Tool blocks were ordered by completion rather than call "
                     "order."}}}}});
    mode.handle({{"type", "turn"},
                 {"id", "ordinary-driver"},
                 {"prompt",
                  {{"text", "Why are parallel tool calls displayed out of "
                            "order?"},
                   {"source", "ordinary"}}}});
    mode.wait_for_idle();
    EXPECT_TRUE(output.back().value("type", "") == "turn.completed");

    std::optional<std::uint64_t> request_sequence;
    std::optional<std::uint64_t> slow_start_sequence;
    std::optional<std::uint64_t> fast_start_sequence;
    std::optional<std::uint64_t> slow_end_sequence;
    std::optional<std::uint64_t> fast_end_sequence;
    std::optional<std::uint64_t> answer_sequence;
    for (const auto &value : output) {
      if (value.value("type", "") != "event")
        continue;
      const auto event = value.value("event", "");
      const auto &data = value["data"];
      const auto sequence = value.value("sequence", std::uint64_t{0});
      if (event == "message_start" && data.contains("request") &&
          data["request"].value("source", "") == "ordinary")
        request_sequence = sequence;
      else if (event == "tool_execution_start" &&
               data.value("tool_call_id", "") == "slow-call")
        slow_start_sequence = sequence;
      else if (event == "tool_execution_start" &&
               data.value("tool_call_id", "") == "fast-call")
        fast_start_sequence = sequence;
      else if (event == "tool_execution_end" &&
               data.value("tool_call_id", "") == "slow-call")
        slow_end_sequence = sequence;
      else if (event == "tool_execution_end" &&
               data.value("tool_call_id", "") == "fast-call")
        fast_end_sequence = sequence;
      else if (event == "message_end" &&
               data["message"].value("stopReason", "") == "stop" &&
               !data["message"]["content"].empty() &&
               data["message"]["content"][0]
                       .value("text", "")
                       .find("ordered by completion") != std::string::npos)
        answer_sequence = sequence;
    }
    EXPECT_TRUE(request_sequence.has_value());
    EXPECT_TRUE(slow_start_sequence.has_value() &&
                fast_start_sequence.has_value());
    EXPECT_TRUE(slow_end_sequence.has_value() && fast_end_sequence.has_value());
    EXPECT_TRUE(answer_sequence.has_value());
    // Calls dispatch in call order even though the fast tool's shorter
    // finish_after_ms makes it complete first. Their completion events may
    // land in either order on the wire; the renderer is responsible for
    // pinning both regions to call order regardless (see
    // test_tool_regions_preserve_call_order in test_region_renderer.cpp).
    EXPECT_TRUE(request_sequence < slow_start_sequence);
    EXPECT_TRUE(slow_start_sequence < fast_start_sequence);
    // The final answer always lands after both tool completions regardless
    // of their out-of-order finish.
    EXPECT_TRUE(slow_end_sequence < answer_sequence);
    EXPECT_TRUE(fast_end_sequence < answer_sequence);
    mode.stop();
  }

  {
    Fixture fixture;
    std::vector<nlohmann::json> output;
    auto &agent = fixture.session.agent();
    cli::FauxControlMode mode(
        fixture.session, *fixture.client, fixture.tool_registry,
        [&output](const nlohmann::json &value) { output.push_back(value); },
        [&agent,
         tool_registry = fixture.tool_registry](const std::string &name) {
          agent.add_tool(
              std::make_shared<core::ScriptedTool>(name, tool_registry));
        });

    mode.handle(
        {{"type", "round"},
         {"id", "mailbox-reply"},
         {"stop_reason", "tool_calls"},
         {"content",
          {{{"type", "tool_call"},
            {"call_id", "reply-call"},
            {"name", "agents_reply"},
            {"args", {{"message_id", "message-1"}, {"text", "queued reply"}}},
            {"result", {{"content", "queued"}, {"is_error", false}}},
            {"presentation",
             {{"kind", "mailbox_reply_queued"},
              {"request_message_id", "message-1"},
              {"recipient_session_id", "session-luna"},
              {"recipient_agent_id", "agent-luna"},
              {"text", "queued reply"}}},
            {"finish_after_ms", 5}}}}});
    mode.handle({{"type", "round"},
                 {"id", "mailbox-closing"},
                 {"stop_reason", "end_turn"},
                 {"content",
                  {{{"type", "text"},
                    {"text", "The mailbox reply was queued successfully."}}}}});
    mode.handle({{"type", "turn"},
                 {"id", "mailbox-driver"},
                 {"prompt",
                  {{"text", "Implement milestone 2 and verify it in tmux."},
                   {"source", "mailbox"},
                   {"message_id", "message-1"},
                   {"sender_task_path", "/root/luna"}}}});
    mode.wait_for_idle();
    EXPECT_TRUE(output.back().value("type", "") == "turn.completed");

    std::optional<std::uint64_t> request_sequence;
    std::optional<std::uint64_t> reply_sequence;
    std::optional<std::uint64_t> answer_sequence;
    for (const auto &value : output) {
      if (value.value("type", "") != "event")
        continue;
      const auto event = value.value("event", "");
      const auto &data = value["data"];
      const auto sequence = value.value("sequence", std::uint64_t{0});
      if (event == "message_start" && data.contains("request") &&
          data["request"].value("source", "") == "mailbox")
        request_sequence = sequence;
      else if (event == "tool_presentation") {
        EXPECT_TRUE(data.value("kind", "") == "mailbox_reply_queued");
        EXPECT_TRUE(data.value("request_message_id", "") == "message-1");
        EXPECT_TRUE(data.value("recipient_agent_id", "") == "agent-luna");
        EXPECT_TRUE(data.value("state", "") == "queued");
        reply_sequence = sequence;
      } else if (event == "message_end" &&
                 data["message"].value("stopReason", "") == "stop" &&
                 !data["message"]["content"].empty() &&
                 data["message"]["content"][0]
                         .value("text", "")
                         .find("queued successfully") != std::string::npos)
        answer_sequence = sequence;
    }
    EXPECT_TRUE(request_sequence.has_value());
    EXPECT_TRUE(reply_sequence.has_value());
    EXPECT_TRUE(answer_sequence.has_value());
    EXPECT_TRUE(request_sequence < reply_sequence);
    EXPECT_TRUE(reply_sequence < answer_sequence);
    mode.stop();
  }
}
