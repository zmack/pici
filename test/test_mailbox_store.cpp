#include "core/mailbox/mailbox_store.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <stop_token>
#include <string_view>

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

namespace {

ProcessRecord process(std::string id, std::string workspace, TimestampMs now,
                      TimestampMs lease = 100'000) {
  return ProcessRecord{.process_id = std::move(id),
                       .workspace_id = std::move(workspace),
                       .workspace_path = "/tmp/workspace",
                       .pid = 12,
                       .hostname = "test",
                       .started_at_ms = now,
                       .last_seen_at_ms = now,
                       .lease_expires_at_ms = now + lease};
}

AgentRecord agent(std::string id, std::string process_id, std::string session,
                  TimestampMs now, std::optional<std::string> owner = {}) {
  return AgentRecord{.agent_id = std::move(id),
                     .process_id = std::move(process_id),
                     .kind = owner ? "subagent" : "root",
                     .owner_agent_id = std::move(owner),
                     .session_id = std::move(session),
                     .provider = "test",
                     .model_id = "model",
                     .status = "running",
                     .started_at_ms = now};
}

MailboxErrorCode error_code(auto &&call) {
  try {
    call();
  } catch (const MailboxError &error) {
    return error.code();
  }
  return MailboxErrorCode::internal;
}

} // namespace

int main() {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("pici-mailbox-" + std::to_string(::getpid()) + "-" +
                     std::to_string(suffix));
  std::filesystem::create_directories(root);
  const auto path = root / "mailbox.sqlite3";
  TimestampMs now = 1'000;
  std::size_t next_id = 0;
  const auto options = [&](std::string workspace,
                           MailboxScope scope = MailboxScope::workspace) {
    return MailboxStoreOptions{
        .path = path,
        .workspace_id = std::move(workspace),
        .workspace_path = "/tmp/workspace",
        .scope = scope,
        .clock = [&] { return now; },
        .id_generator = [&] { return "id-" + std::to_string(++next_id); },
    };
  };

  try {
    {
      MailboxStore store(options("workspace-a"));
      CHECK_EQ(store.status(StatusRequest{.workspace_id = "workspace-a"})
                   .schema_version,
               1);

      store.register_process(process("process-a", "workspace-a", now));
      store.register_agent(agent("agent-a", "process-a", "session-a", now));
      store.register_process(process("process-b", "workspace-a", now));
      store.register_agent(agent("agent-b", "process-b", "session-b", now));

      const auto initial_wait = store.wait_for_change(
          WaitRequest{.workspace_id = "workspace-a", .timeout_ms = 0});
      CHECK(initial_wait.generation > 0);
      CHECK(initial_wait.presence_changed);
      CHECK(!initial_wait.messages_changed);

      const auto exact = store.send(SendRequest{
          .message_id = "exact-message",
          .sender_agent_id = "agent-a",
          .sender_session_id = "session-a",
          .target = MailboxTarget{.agent_id = "agent-b"},
          .workspace_id = "workspace-a",
          .body = MailboxBody{.text = "hello"},
          .created_at_ms = now,
      });
      CHECK_EQ(exact.recipient_agent_id.value(), std::string("agent-b"));
      const auto message_wait = store.wait_for_change(WaitRequest{
          .workspace_id = "workspace-a",
          .after_generation = initial_wait.generation,
          .timeout_ms = 0});
      CHECK(message_wait.messages_changed);
      CHECK(!message_wait.presence_changed);

      auto inspected = store.inspect(
          InboxQuery{.session_id = "session-b", .now_ms = now});
      CHECK_EQ(inspected.size(), std::size_t{1});
      CHECK_EQ(inspected.front().body.text, std::string("hello"));
      const auto claimed = store.claim(ClaimRequest{
          .session_id = "session-b", .agent_id = "agent-b", .now_ms = now});
      CHECK_EQ(claimed.messages.size(), std::size_t{1});
      CHECK(claimed.messages.front().claim_token.has_value());
      const auto token = *claimed.messages.front().claim_token;
      CHECK(error_code([&] {
              store.acknowledge(AcknowledgeRequest{
                  .message_id = exact.message_id,
                  .agent_id = "wrong-agent",
                  .claim_token = token,
                  .now_ms = now});
            }) == MailboxErrorCode::invalid_claim);
      store.acknowledge(AcknowledgeRequest{.message_id = exact.message_id,
                                           .agent_id = "agent-b",
                                           .claim_token = token,
                                           .now_ms = now});
      store.acknowledge(AcknowledgeRequest{.message_id = exact.message_id,
                                           .agent_id = "agent-b",
                                           .claim_token = token,
                                           .now_ms = now});
      CHECK(store.inspect(
                InboxQuery{.session_id = "session-b", .now_ms = now})
                .empty());

      const auto session = store.send(SendRequest{
          .sender_agent_id = "agent-a",
          .sender_session_id = "session-a",
          .target = MailboxTarget{.session_id = "session-b"},
          .workspace_id = "workspace-a",
          .body = MailboxBody{.text = "durable"},
          .created_at_ms = now,
      });
      CHECK(!session.recipient_agent_id.has_value());
      store.register_process(process("process-c", "workspace-a", now));
      store.register_agent(agent("agent-c", "process-c", "session-b", now));
      CHECK(error_code([&] {
              store.send(SendRequest{
                  .sender_agent_id = "agent-a",
                  .sender_session_id = "session-a",
                  .target = MailboxTarget{.session_id = "session-b"},
                  .workspace_id = "workspace-a",
                  .body = MailboxBody{.text = "ambiguous"},
                  .created_at_ms = now});
            }) == MailboxErrorCode::ambiguous_target);
      store.close_process("process-b", now);
      store.close_process("process-c", now);
      store.register_process(process("process-d", "workspace-a", now));
      store.register_agent(agent("agent-d", "process-d", "session-b", now));
      CHECK_EQ(store.claim(ClaimRequest{.session_id = "session-b",
                                        .agent_id = "agent-d",
                                        .now_ms = now})
                   .messages.size(),
               std::size_t{1});

      const auto exclusive = store.send(SendRequest{
          .sender_agent_id = "agent-a",
          .sender_session_id = "session-a",
          .target = MailboxTarget{.session_id = "session-b"},
          .workspace_id = "workspace-a",
          .body = MailboxBody{.text = "exclusive"},
          .created_at_ms = now,
      });
      const auto exclusive_claim = store.claim(ClaimRequest{
          .session_id = "session-b", .agent_id = "agent-d", .now_ms = now});
      CHECK_EQ(exclusive_claim.messages.size(), std::size_t{1});
      CHECK_EQ(exclusive_claim.messages.front().message_id, exclusive.message_id);
      store.register_process(process("process-e", "workspace-a", now));
      store.register_agent(agent("agent-e", "process-e", "session-b", now));
      CHECK_EQ(store.list_agents(AgentQuery{.session_id = "session-b",
                                            .now_ms = now})
                   .size(),
               std::size_t{2});
      CHECK(store.claim(ClaimRequest{.session_id = "session-b",
                                     .agent_id = "agent-e",
                                     .now_ms = now})
                .messages.empty());
      now += 31'000;
      CHECK_EQ(store.inspect(InboxQuery{.session_id = "session-b",
                                        .now_ms = now})
                   .size(),
               std::size_t{2});
      CHECK_EQ(store.claim(ClaimRequest{.session_id = "session-b",
                                        .agent_id = "agent-e",
                                        .limit = 1,
                                        .now_ms = now,
                                        .lease_ms = 30'000})
                   .messages.size(),
               std::size_t{1});

      store.register_process(process("process-order", "workspace-a", now));
      store.register_agent(
          agent("agent-order", "process-order", "session-order", now));
      const auto ordered_a = store.send(SendRequest{
          .message_id = "ordered-a",
          .sender_agent_id = "agent-a",
          .sender_session_id = "session-a",
          .target = MailboxTarget{.session_id = "session-order"},
          .workspace_id = "workspace-a",
          .body = MailboxBody{.text = "a"},
          .created_at_ms = now,
      });
      const auto ordered_b = store.send(SendRequest{
          .message_id = "ordered-b",
          .sender_agent_id = "agent-a",
          .sender_session_id = "session-a",
          .target = MailboxTarget{.session_id = "session-order"},
          .workspace_id = "workspace-a",
          .body = MailboxBody{.text = "b"},
          .created_at_ms = now,
      });
      CHECK_EQ(ordered_a.message_id, std::string("ordered-a"));
      CHECK_EQ(store.inspect(InboxQuery{.session_id = "session-order",
                                        .now_ms = now})
                   .front()
                   .message_id,
               std::string("ordered-a"));
      CHECK(error_code([&] {
              store.send(SendRequest{
                  .message_id = ordered_b.message_id,
                  .sender_agent_id = "agent-a",
                  .sender_session_id = "session-a",
                  .target = MailboxTarget{.session_id = "session-order"},
                  .workspace_id = "workspace-a",
                  .body = MailboxBody{.text = "duplicate"},
                  .created_at_ms = now});
            }) == MailboxErrorCode::invalid_message);

      const auto generation =
          store.status(StatusRequest{.workspace_id = "workspace-a"})
              .latest_generation;
      std::stop_source stop;
      stop.request_stop();
      CHECK(store.wait_for_change(
                WaitRequest{.workspace_id = "workspace-a",
                            .after_generation = generation,
                            .timeout_ms = 60'000},
                stop.get_token())
                .timed_out);
    }

    MailboxStore reopened(options("workspace-a"));
    CHECK(reopened.inspect(
              InboxQuery{.session_id = "session-order", .now_ms = now})
              .size() >= 2);

    MailboxStore global(options("workspace-a", MailboxScope::global));
    global.register_process(process("process-b2", "workspace-b", now));
    global.register_agent(
        agent("agent-b2", "process-b2", "session-b2", now));
    MailboxStore workspace_b(options("workspace-b"));
    CHECK_EQ(workspace_b.list_agents(AgentQuery{.now_ms = now}).size(),
             std::size_t{1});
    CHECK(error_code([&] {
            reopened.list_agents(AgentQuery{.workspace_id = "workspace-b",
                                             .now_ms = now});
          }) == MailboxErrorCode::permission_denied);
    const auto cross_workspace = global.send(SendRequest{
        .sender_agent_id = "agent-a",
        .sender_session_id = "session-a",
        .target = MailboxTarget{.agent_id = "agent-b2"},
        .workspace_id = "workspace-b",
        .body = MailboxBody{.text = "cross-workspace"},
        .created_at_ms = now,
    });
    CHECK_EQ(workspace_b.inspect(InboxQuery{.session_id = "session-b2",
                                            .now_ms = now})
                 .front()
                 .message_id,
             cross_workspace.message_id);
    CHECK(error_code([&] {
            reopened.send(SendRequest{
                .sender_agent_id = "agent-a",
                .sender_session_id = "session-a",
                .target = MailboxTarget{.agent_id = "agent-b2"},
                .workspace_id = "workspace-b",
                .body = MailboxBody{.text = "blocked"},
                .created_at_ms = now});
          }) == MailboxErrorCode::permission_denied);

    const auto cleanup_process = process("process-cleanup", "workspace-a", now);
    reopened.register_process(cleanup_process);
    reopened.register_agent(
        agent("agent-parent", "process-cleanup", "cleanup", now));
    reopened.register_agent(agent("agent-child", "process-cleanup", "child",
                                  now, std::string("agent-parent")));
    reopened.close_process("process-cleanup", now);
    now += 500;
    const auto cleanup = reopened.cleanup(CleanupRequest{
        .now_ms = now, .acknowledged_retention_ms = 0, .stale_retention_ms = 0});
    CHECK(cleanup.agents_removed >= 2);
    CHECK(cleanup.processes_removed >= 1);

    reopened.register_process(process("process-root-claim", "workspace-a",
                                      now));
    reopened.register_agent(
        agent("root-claim", "process-root-claim", "shared-session", now));
    const auto root_only = reopened.send(SendRequest{
        .message_id = "root-only-claim",
        .sender_agent_id = "agent-a",
        .sender_session_id = "session-a",
        .target = MailboxTarget{.session_id = "shared-session"},
        .workspace_id = "workspace-a",
        .body = MailboxBody{.text = "root only"},
        .created_at_ms = now});
    reopened.register_agent(agent("child-claim", "process-root-claim",
                                  "shared-session", now,
                                  std::string("root-claim")));
    CHECK(reopened.claim(ClaimRequest{.session_id = "shared-session",
                                     .agent_id = "child-claim",
                                     .now_ms = now})
              .messages.empty());
    const auto root_claim = reopened.claim(ClaimRequest{
        .session_id = "shared-session", .agent_id = "root-claim", .now_ms = now});
    CHECK_EQ(root_claim.messages.size(), std::size_t{1});
    CHECK_EQ(root_claim.messages.front().message_id, root_only.message_id);

    const auto newer_path = root / "newer.sqlite3";
    sqlite3 *database = nullptr;
    CHECK_EQ(sqlite3_open(newer_path.c_str(), &database), SQLITE_OK);
    CHECK_EQ(sqlite3_exec(database, "PRAGMA user_version=99", nullptr, nullptr,
                           nullptr),
             SQLITE_OK);
    sqlite3_close(database);
    const auto newer_options = MailboxStoreOptions{
        .path = newer_path,
        .workspace_id = "workspace-a",
        .workspace_path = "/tmp/workspace",
        .clock = [&] { return now; },
        .id_generator = [&] { return "newer-" + std::to_string(++next_id); },
    };
    CHECK(error_code([&] { MailboxStore newer(newer_options); }) ==
          MailboxErrorCode::incompatible_schema);
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
