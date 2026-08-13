#include "core/mailbox/mailbox_store.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <string_view>
#include <vector>

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

int main() {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("pici-mailbox-" + std::to_string(::getpid()) + "-" +
                     std::to_string(suffix));
  std::filesystem::create_directories(root);
  const auto path = root / "mailbox.sqlite3";
  TimestampMs now = 1'000;
  std::size_t next_id = 0;
  const auto options = [&] {
    MailboxStoreOptions result{
        .path = path,
        .workspace_id = "workspace-a",
        .workspace_path = "/tmp/workspace-a",
        .clock = [&] { return now; },
        .id_generator = [&] { return "id-" + std::to_string(++next_id); },
    };
    return result;
  };

  try {
    MailboxStore store(options());
    CHECK_EQ(store.status(StatusRequest{.workspace_id = "workspace-a"}).schema_version, 1);

    store.register_process(ProcessRecord{
        .process_id = "process-a", .workspace_id = "workspace-a",
        .workspace_path = "/tmp/workspace-a", .pid = 12,
        .hostname = "test", .started_at_ms = now,
        .last_seen_at_ms = now, .lease_expires_at_ms = now + 100,
    });
    store.register_agent(AgentRecord{
        .agent_id = "agent-a", .process_id = "process-a", .kind = "root",
        .session_id = "session-a", .provider = "test", .model_id = "model",
        .status = "running", .started_at_ms = now,
    });
    CHECK_EQ(store.list_agents(AgentQuery{.now_ms = now}).size(), std::size_t{1});

    store.register_process(ProcessRecord{
        .process_id = "process-b", .workspace_id = "workspace-a",
        .workspace_path = "/tmp/workspace-a", .pid = 13,
        .hostname = "test", .started_at_ms = now,
        .last_seen_at_ms = now, .lease_expires_at_ms = now + 100,
    });
    store.register_agent(AgentRecord{
        .agent_id = "agent-b", .process_id = "process-b", .kind = "root",
        .session_id = "session-b", .provider = "test", .model_id = "model",
        .status = "running", .started_at_ms = now,
    });

    const auto receipt = store.send(SendRequest{
        .sender_agent_id = "agent-a", .sender_session_id = "session-a",
        .target = MailboxTarget{.agent_id = "agent-b"},
        .workspace_id = "workspace-a", .kind = MailboxMessageKind::note,
        .body = MailboxBody{.text = "hello"}, .created_at_ms = now,
    });
    CHECK(!receipt.message_id.empty());
    auto inspected = store.inspect(InboxQuery{.session_id = "session-b", .now_ms = now});
    CHECK_EQ(inspected.size(), std::size_t{1});
    CHECK_EQ(inspected.front().body.text, std::string("hello"));

    const auto claimed = store.claim(ClaimRequest{
        .session_id = "session-b", .agent_id = "agent-b", .now_ms = now,
    });
    CHECK_EQ(claimed.messages.size(), std::size_t{1});
    CHECK(claimed.messages.front().claim_token.has_value());
    store.acknowledge(AcknowledgeRequest{
        .message_id = receipt.message_id, .agent_id = "agent-b",
        .claim_token = *claimed.messages.front().claim_token, .now_ms = now,
    });
    CHECK(store.inspect(InboxQuery{.session_id = "session-b", .now_ms = now}).empty());

    store.send(SendRequest{
        .sender_agent_id = "agent-a", .sender_session_id = "session-a",
        .target = MailboxTarget{.session_id = "session-b"},
        .workspace_id = "workspace-a", .kind = MailboxMessageKind::note,
        .body = MailboxBody{.text = "late"}, .created_at_ms = now,
    });
    now += 101;
    CHECK_EQ(store.list_agents(AgentQuery{.now_ms = now}).size(), std::size_t{0});
    store.heartbeat_process("process-b", now, now + 100);
    CHECK_EQ(store.list_agents(AgentQuery{.now_ms = now}).size(), std::size_t{1});
    CHECK(store.status(StatusRequest{.workspace_id = "workspace-a"}).unread_messages > 0);
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
