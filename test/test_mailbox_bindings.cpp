#include "core/mailbox/mailbox_bindings.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <source_location>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#include <unistd.h>

namespace tests {
int passed{0};
int failed{0};

bool check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
  if (condition) {
    ++passed;
    return true;
  }
  ++failed;
  std::cout << "FAIL " << location.file_name() << ":" << location.line()
            << " — " << expression << "\n";
  return false;
}
} // namespace tests

#define CHECK(expression) tests::check((expression), #expression)
#define CHECK_EQ(left, right) tests::check((left) == (right), #left " == " #right)

using namespace pi::core;

static std::shared_ptr<MailboxCoordinator>
make_coordinator(const std::filesystem::path &path, std::string process_id,
                 std::string root_agent_id, std::string session_id,
                 TimestampMs &now) {
  auto counter = std::make_shared<std::size_t>(0);
  MailboxCoordinatorOptions options;
  options.store.path = path;
  options.store.workspace_id = "workspace";
  options.store.workspace_path = path.parent_path().string();
  options.store.claim_lease_ms = 1'000;
  options.store.clock = [&now] { return now; };
  options.store.id_generator = [counter, process_id] {
    return process_id + "-message-" + std::to_string((*counter)++);
  };
  options.process_id = std::move(process_id);
  options.root_agent_id = std::move(root_agent_id);
  options.initial_session_id = std::move(session_id);
  options.provider = "test-provider";
  options.model_id = "test-model";
  options.heartbeat_interval = std::chrono::hours(1);
  options.stale_after = std::chrono::hours(2);
  options.cleanup_interval = std::chrono::hours(2);
  return std::make_shared<MailboxCoordinator>(std::move(options));
}

static bool has_error(const nlohmann::json &value, std::string_view code) {
  return value.contains("error") && value["error"].is_object() &&
         value["error"].value("code", "") == code;
}

int main() {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("pici-mailbox-bindings-" + std::to_string(::getpid()) +
                     "-" + std::to_string(suffix));
  std::filesystem::create_directories(root);
  std::filesystem::permissions(
      root, std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace);
  TimestampMs now = 1'000;

  try {
    const auto path = root / "mailbox.sqlite3";
    auto first = make_coordinator(path, "process-a", "agent-a", "session-a",
                                  now);
    auto second = make_coordinator(path, "process-b", "agent-b", "session-b",
                                   now);
    auto duplicate_session = make_coordinator(
        path, "process-c", "agent-c", "session-a", now);
    const auto first_bindings = make_mailbox_bindings(first);
    const auto second_bindings = make_mailbox_bindings(second);
    const auto call = [](const LuaHooks::MailboxBindings::Callback &callback,
                         nlohmann::json value,
                         std::stop_token stop_token = {}) {
      return callback(value, stop_token);
    };

    const auto self = call(first_bindings.self, nlohmann::json::object());
    CHECK_EQ(self["agent_id"], "agent-a");
    CHECK_EQ(self["session_id"], "session-a");
    CHECK(self.contains("workspace_path"));

    const auto list = call(first_bindings.list,
                           {{"include_self", true}, {"limit", 10}});
    CHECK(list.is_array());
    CHECK_EQ(list.size(), std::size_t{3});
    CHECK(list[0].contains("same_session_attached"));
    duplicate_session->deactivate_root();

    CHECK(has_error(call(first_bindings.send,
                         {{"target", {{"agent_id", "agent-b"},
                                        {"session_id", "session-b"}}},
                          {"text", "bad"}}),
                    "invalid_message"));
    CHECK(has_error(call(first_bindings.send,
                         {{"target", {{"agent_id", "agent-b"}}},
                          {"text", 42}}),
                    "invalid_message"));
    CHECK(has_error(call(first_bindings.list,
                         {{"include_self", "yes"}}),
                    "invalid_message"));
    CHECK(has_error(call(first_bindings.wait, {{"timeout_ms", -1}}),
                    "invalid_message"));
    CHECK(has_error(call(first_bindings.send,
                         {{"target", {{"agent_id", "agent-b"}}},
                          {"text", "bad"}, {"reply_to", ""}}),
                    "invalid_message"));
    CHECK(has_error(call(first_bindings.inbox, {{"limit", 51}}),
                    "invalid_message"));

    const auto exact = call(first_bindings.send,
                            {{"target", {{"agent_id", "agent-b"}}},
                             {"text", "exact"},
                             {"kind", "note"}});
    CHECK(exact.contains("message_id"));
    CHECK_EQ(exact["recipient_agent_id"], "agent-b");
    const auto inspected = call(second_bindings.inbox, {{"claim", false}});
    CHECK_EQ(inspected.size(), std::size_t{1});
    CHECK(!inspected[0].contains("claim_token"));
    CHECK_EQ(call(second_bindings.inbox, {{"claim", false}}).size(),
             std::size_t{1});

    const auto claimed_exact = call(second_bindings.inbox, {{"claim", true}});
    CHECK_EQ(claimed_exact.size(), std::size_t{1});
    const auto exact_ack = call(
        second_bindings.ack,
        {{"message_id", claimed_exact[0]["message_id"]},
         {"claim_token", claimed_exact[0]["claim_token"]}});
    CHECK_EQ(exact_ack["state"], "acknowledged");

    const auto session_send = call(second_bindings.send,
                                   {{"target", {{"session_id", "session-a"}}},
                                    {"text", "session"},
                                    {"kind", "steer"}});
    CHECK(session_send["recipient_agent_id"].is_null());
    const auto claimed = call(first_bindings.inbox, {{"claim", true}});
    CHECK_EQ(claimed.size(), std::size_t{1});
    CHECK(claimed[0].contains("claim_token"));
    const auto ack = call(first_bindings.ack,
                          {{"message_id", claimed[0]["message_id"]},
                           {"claim_token", claimed[0]["claim_token"]}});
    CHECK_EQ(ack["state"], "acknowledged");
    CHECK_EQ(call(first_bindings.ack,
                  {{"message_id", claimed[0]["message_id"]},
                   {"claim_token", claimed[0]["claim_token"]}})["state"],
             "acknowledged");

    const auto pending = call(first_bindings.request,
                              {{"target", {{"agent_id", "agent-b"}}},
                               {"text", "pending"},
                               {"timeout_ms", 0}});
    CHECK_EQ(pending["state"], "pending");
    const auto pending_id = pending["request_id"].get<std::string>();
    const auto pending_inbox = call(second_bindings.inbox, {{"claim", false}});
    CHECK(std::ranges::any_of(pending_inbox, [&](const auto &message) {
      return message["message_id"] == pending_id;
    }));
    const auto unrelated = call(
        second_bindings.reply,
        {{"message_id", exact["message_id"]}, {"text", "unrelated"}});
    CHECK(unrelated.contains("message_id"));
    const auto unrelated_inbox = call(first_bindings.inbox, {{"claim", false}});
    CHECK(std::ranges::any_of(unrelated_inbox, [&](const auto &message) {
      return message["message_id"] == unrelated["message_id"];
    }));

    std::promise<nlohmann::json> request_result;
    auto request_future = request_result.get_future();
    std::jthread request_thread([&] {
      request_result.set_value(call(
          first_bindings.request,
          {{"target", {{"agent_id", "agent-b"}}},
           {"text", "correlated"},
           {"timeout_ms", 5'000}}));
    });
    std::string correlated_id;
    for (int attempt = 0; attempt < 1'000 && correlated_id.empty(); ++attempt) {
      const auto messages = call(second_bindings.inbox, {{"claim", false}});
      for (const auto &message : messages)
        if (message["kind"] == "request" &&
            message["text"] == "correlated")
          correlated_id = message["message_id"].get<std::string>();
      if (correlated_id.empty())
        std::this_thread::yield();
    }
    CHECK(!correlated_id.empty());
    const auto reply = call(second_bindings.reply,
                            {{"message_id", correlated_id},
                             {"text", "correlated reply"}});
    CHECK_EQ(reply["recipient_agent_id"], "agent-a");
    const auto correlated = request_future.get();
    CHECK_EQ(correlated["state"], "replied");
    CHECK_EQ(correlated["reply_to"], correlated_id);
    request_thread.join();

    const auto late = call(first_bindings.request,
                           {{"target", {{"agent_id", "agent-b"}}},
                            {"text", "late"},
                            {"timeout_ms", 0}});
    const auto late_id = late["request_id"].get<std::string>();
    const auto late_reply = call(second_bindings.reply,
                                 {{"message_id", late_id},
                                  {"text", "late reply"}});
    CHECK_EQ(late_reply["recipient_agent_id"], "agent-a");
    CHECK(std::ranges::any_of(call(first_bindings.inbox, {{"claim", false}}),
                              [&](const auto &message) {
                                return message["reply_to"] == late_id;
                              }));

    const auto generation = first->status().mailbox.latest_generation;
    const auto waited = call(first_bindings.wait,
                             {{"after_generation", generation},
                              {"timeout_ms", 0}});
    CHECK(waited["timed_out"]);
    CHECK(!waited["presence_changed"]);
    CHECK(!waited["messages_changed"]);
    const auto status = call(first_bindings.status, nlohmann::json::object());
    CHECK_EQ(status["workspace_id"], "workspace");
    CHECK(status.contains("schema_version"));
    std::stop_source stop;
    stop.request_stop();
    CHECK(call(first_bindings.wait,
               {{"after_generation", generation}, {"timeout_ms", 1}},
               stop.get_token())["timed_out"]);

    first->stop();
    const auto stopped =
        call(first_bindings.self, nlohmann::json::object());
    CHECK(has_error(stopped, "not_found"));
    first.reset();
    const auto unavailable =
        call(make_mailbox_bindings(first).self, nlohmann::json::object());
    CHECK(has_error(unavailable, "not_found"));
  } catch (const std::exception &error) {
    std::cout << "unexpected exception: " << error.what() << "\n";
    ++tests::failed;
  }

  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::cout << "Tests: " << tests::passed << " passed, " << tests::failed
            << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
