#include "core/mailbox/mailbox_store.h"
#include "support/gtest_helpers.h"

#include <gtest/gtest.h>

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <source_location>
#include <stop_token>
#include <string_view>

#include <unistd.h>

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

class MailboxStoreTest : public testing::Test {
protected:
  pi::test::TemporaryDirectory directory{"pici-mailbox"};
  TimestampMs now{1'000};
  std::size_t next_id{0};

  void SetUp() override {
    std::filesystem::permissions(directory.path(),
                                 std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
  }

  MailboxStoreOptions options(std::string workspace,
                              MailboxScope scope = MailboxScope::workspace) {
    return MailboxStoreOptions{
        .path = directory.path() / "mailbox.sqlite3",
        .workspace_id = std::move(workspace),
        .workspace_path = "/tmp/workspace",
        .scope = scope,
        .clock = [&] { return now; },
        .id_generator = [&] { return "id-" + std::to_string(++next_id); },
    };
  }
};

TEST_F(MailboxStoreTest, ClaimsAcksAndResolvesSessionTargets) {
  {
    MailboxStore store(options("workspace-a"));
    EXPECT_EQ(store.status(StatusRequest{.workspace_id = "workspace-a"})
                  .schema_version,
              1);

    store.register_process(process("process-a", "workspace-a", now));
    store.register_agent(agent("agent-a", "process-a", "session-a", now));
    store.register_process(process("process-b", "workspace-a", now));
    store.register_agent(agent("agent-b", "process-b", "session-b", now));

    const auto initial_wait = store.wait_for_change(
        WaitRequest{.workspace_id = "workspace-a", .timeout_ms = 0});
    EXPECT_TRUE(initial_wait.generation > 0);
    EXPECT_TRUE(initial_wait.presence_changed);
    EXPECT_TRUE(!initial_wait.messages_changed);

    const auto exact = store.send(EnqueueMailboxEntryRequest{
        .entry_id = "exact-message",
        .sender_agent_id = "agent-a",
        .sender_session_id = "session-a",
        .target = MailboxTarget{.agent_id = "agent-b"},
        .workspace_id = "workspace-a",
        .body = MailboxPayload{.text = "hello"},
        .created_at_ms = now,
    });
    EXPECT_EQ(exact.recipient_agent_id.value(), std::string("agent-b"));
    const auto message_wait = store.wait_for_change(
        WaitRequest{.workspace_id = "workspace-a",
                    .after_generation = initial_wait.generation,
                    .timeout_ms = 0});
    EXPECT_TRUE(message_wait.messages_changed);
    EXPECT_TRUE(!message_wait.presence_changed);

    auto inspected =
        store.inspect(InboxQuery{.session_id = "session-b", .now_ms = now});
    EXPECT_EQ(inspected.size(), std::size_t{1});
    EXPECT_EQ(inspected.front().body.text, std::string("hello"));
    const auto claimed = store.claim(ClaimRequest{
        .session_id = "session-b", .agent_id = "agent-b", .now_ms = now});
    EXPECT_EQ(claimed.messages.size(), std::size_t{1});
    ASSERT_TRUE(claimed.messages.front().claim_token.has_value());
    // claim() alone is not delivery (docs/architecture-lexicon.md:
    // "Delivery converts a claimed actionable entry into agent input") --
    // the raw agents_claim Lua path calls claim() without ever building
    // agent input, so delivered_at_ms must stay unset until a caller
    // (MailboxCoordinator's two delivery sites) explicitly calls
    // mark_delivered().
    EXPECT_TRUE(!claimed.messages.front().delivered_at_ms.has_value());
    store.mark_delivered(exact.entry_id, "workspace-a", now);
    const auto after_mark_delivered =
        store.inspect(InboxQuery{.session_id = "session-b", .now_ms = now});
    EXPECT_EQ(after_mark_delivered.size(), std::size_t{1});
    EXPECT_TRUE(after_mark_delivered.front().delivered_at_ms.has_value());
    EXPECT_EQ(*after_mark_delivered.front().delivered_at_ms, now);
    const auto token = *claimed.messages.front().claim_token;
    EXPECT_TRUE(error_code([&] {
                  store.acknowledge(
                      AcknowledgeRequest{.entry_id = exact.entry_id,
                                         .agent_id = "wrong-agent",
                                         .claim_token = token,
                                         .now_ms = now});
                }) == MailboxErrorCode::invalid_claim);
    store.acknowledge(AcknowledgeRequest{.entry_id = exact.entry_id,
                                         .agent_id = "agent-b",
                                         .claim_token = token,
                                         .now_ms = now});
    store.acknowledge(AcknowledgeRequest{.entry_id = exact.entry_id,
                                         .agent_id = "agent-b",
                                         .claim_token = token,
                                         .now_ms = now});
    EXPECT_TRUE(
        store.inspect(InboxQuery{.session_id = "session-b", .now_ms = now})
            .empty());

    const auto session = store.send(EnqueueMailboxEntryRequest{
        .sender_agent_id = "agent-a",
        .sender_session_id = "session-a",
        .target = MailboxTarget{.session_id = "session-b"},
        .workspace_id = "workspace-a",
        .body = MailboxPayload{.text = "durable"},
        .created_at_ms = now,
    });
    EXPECT_TRUE(!session.recipient_agent_id.has_value());
    store.register_process(process("process-c", "workspace-a", now));
    store.register_agent(agent("agent-c", "process-c", "session-b", now));
    EXPECT_TRUE(error_code([&] {
                  store.send(EnqueueMailboxEntryRequest{
                      .sender_agent_id = "agent-a",
                      .sender_session_id = "session-a",
                      .target = MailboxTarget{.session_id = "session-b"},
                      .workspace_id = "workspace-a",
                      .body = MailboxPayload{.text = "ambiguous"},
                      .created_at_ms = now});
                }) == MailboxErrorCode::ambiguous_target);
    store.close_process("process-b", now);
    store.close_process("process-c", now);
    store.register_process(process("process-d", "workspace-a", now));
    store.register_agent(agent("agent-d", "process-d", "session-b", now));
    EXPECT_EQ(store
                  .claim(ClaimRequest{.session_id = "session-b",
                                      .agent_id = "agent-d",
                                      .now_ms = now})
                  .messages.size(),
              std::size_t{1});

    const auto exclusive = store.send(EnqueueMailboxEntryRequest{
        .sender_agent_id = "agent-a",
        .sender_session_id = "session-a",
        .target = MailboxTarget{.session_id = "session-b"},
        .workspace_id = "workspace-a",
        .body = MailboxPayload{.text = "exclusive"},
        .created_at_ms = now,
    });
    const auto exclusive_claim = store.claim(ClaimRequest{
        .session_id = "session-b", .agent_id = "agent-d", .now_ms = now});
    EXPECT_EQ(exclusive_claim.messages.size(), std::size_t{1});
    EXPECT_EQ(exclusive_claim.messages.front().entry_id, exclusive.entry_id);
    store.register_process(process("process-e", "workspace-a", now));
    store.register_agent(agent("agent-e", "process-e", "session-b", now));
    EXPECT_EQ(
        store.list_agents(AgentQuery{.session_id = "session-b", .now_ms = now})
            .size(),
        std::size_t{2});
    EXPECT_TRUE(store
                    .claim(ClaimRequest{.session_id = "session-b",
                                        .agent_id = "agent-e",
                                        .now_ms = now})
                    .messages.empty());
    now += 31'000;
    EXPECT_EQ(
        store.inspect(InboxQuery{.session_id = "session-b", .now_ms = now})
            .size(),
        std::size_t{2});
    EXPECT_EQ(store
                  .claim(ClaimRequest{.session_id = "session-b",
                                      .agent_id = "agent-e",
                                      .limit = 1,
                                      .now_ms = now,
                                      .lease_ms = 30'000})
                  .messages.size(),
              std::size_t{1});
  }
}

TEST_F(MailboxStoreTest, OrdersFiltersAndRejectsDuplicateMessages) {
  MailboxStore store(options("workspace-a"));
  store.register_process(process("process-order", "workspace-a", now));
  store.register_agent(
      agent("agent-order", "process-order", "session-order", now));
  const auto ordered_a = store.send(EnqueueMailboxEntryRequest{
      .entry_id = "ordered-a",
      .sender_agent_id = "agent-a",
      .sender_session_id = "session-a",
      .target = MailboxTarget{.session_id = "session-order"},
      .workspace_id = "workspace-a",
      .body = MailboxPayload{.text = "a"},
      .created_at_ms = now,
  });
  const auto ordered_b = store.send(EnqueueMailboxEntryRequest{
      .entry_id = "ordered-b",
      .sender_agent_id = "agent-a",
      .sender_session_id = "session-a",
      .target = MailboxTarget{.session_id = "session-order"},
      .workspace_id = "workspace-a",
      .body = MailboxPayload{.text = "b"},
      .created_at_ms = now,
  });
  EXPECT_EQ(ordered_a.entry_id, std::string("ordered-a"));
  EXPECT_EQ(
      store.inspect(InboxQuery{.session_id = "session-order", .now_ms = now})
          .front()
          .entry_id,
      std::string("ordered-a"));
  // Recipient and kind predicates are applied in SQL before LIMIT: an
  // unrelated row must not starve the caller's exact, filtered inbox.
  store.register_process(process("process-other", "workspace-a", now));
  store.register_agent(
      agent("agent-other", "process-other", "session-order", now));
  store.send(EnqueueMailboxEntryRequest{
      .entry_id = "filtered-other",
      .sender_agent_id = "agent-a",
      .sender_session_id = "session-a",
      .target = MailboxTarget{.agent_id = "agent-other"},
      .workspace_id = "workspace-a",
      .kind = MailboxEntryKind::steer,
      .body = MailboxPayload{.text = "other"},
      .created_at_ms = now});
  store.send(EnqueueMailboxEntryRequest{
      .entry_id = "filtered-note",
      .sender_agent_id = "agent-a",
      .sender_session_id = "session-a",
      .target = MailboxTarget{.agent_id = "agent-order"},
      .workspace_id = "workspace-a",
      .kind = MailboxEntryKind::note,
      .body = MailboxPayload{.text = "note"},
      .created_at_ms = now});
  store.send(EnqueueMailboxEntryRequest{
      .entry_id = "filtered-steer",
      .sender_agent_id = "agent-a",
      .sender_session_id = "session-a",
      .target = MailboxTarget{.agent_id = "agent-order"},
      .workspace_id = "workspace-a",
      .kind = MailboxEntryKind::steer,
      .body = MailboxPayload{.text = "steer"},
      .created_at_ms = now});
  const auto filtered =
      store.inspect(InboxQuery{.session_id = "session-order",
                               .agent_id = "agent-order",
                               .agent_kind = "root",
                               .kinds = {MailboxEntryKind::steer},
                               .limit = 1,
                               .now_ms = now});
  EXPECT_EQ(filtered.size(), std::size_t{1});
  EXPECT_EQ(filtered.front().entry_id, std::string("filtered-steer"));
  EXPECT_TRUE(error_code([&] {
                store.send(EnqueueMailboxEntryRequest{
                    .entry_id = ordered_b.entry_id,
                    .sender_agent_id = "agent-a",
                    .sender_session_id = "session-a",
                    .target = MailboxTarget{.agent_id = "agent-order"},
                    .workspace_id = "workspace-a",
                    .body = MailboxPayload{.text = "duplicate"},
                    .created_at_ms = now});
              }) == MailboxErrorCode::invalid_message);

  const auto generation =
      store.status(StatusRequest{.workspace_id = "workspace-a"})
          .latest_generation;
  std::stop_source stop;
  stop.request_stop();
  EXPECT_TRUE(store
                  .wait_for_change(WaitRequest{.workspace_id = "workspace-a",
                                               .after_generation = generation,
                                               .timeout_ms = 60'000},
                                   stop.get_token())
                  .timed_out);
}

TEST_F(MailboxStoreTest, ReopensScopesAndCleansUpAgents) {
  {
    MailboxStore seed(options("workspace-a"));
    seed.register_process(process("process-seed", "workspace-a", now));
    seed.register_agent(
        agent("agent-seed", "process-seed", "session-order", now));
    seed.send(EnqueueMailboxEntryRequest{
        .entry_id = "seed-message",
        .sender_agent_id = "agent-seed",
        .sender_session_id = "session-order",
        .target = MailboxTarget{.session_id = "session-order"},
        .workspace_id = "workspace-a",
        .body = MailboxPayload{.text = "seed"},
        .created_at_ms = now});
    seed.send(EnqueueMailboxEntryRequest{
        .entry_id = "seed-message-2",
        .sender_agent_id = "agent-seed",
        .sender_session_id = "session-order",
        .target = MailboxTarget{.session_id = "session-order"},
        .workspace_id = "workspace-a",
        .body = MailboxPayload{.text = "seed-2"},
        .created_at_ms = now});
  }
  MailboxStore reopened(options("workspace-a"));
  EXPECT_TRUE(
      reopened.inspect(InboxQuery{.session_id = "session-order", .now_ms = now})
          .size() >= 2);

  MailboxStore global(options("workspace-a", MailboxScope::global));
  global.register_process(process("process-b2", "workspace-b", now));
  global.register_agent(agent("agent-b2", "process-b2", "session-b2", now));
  MailboxStore workspace_b(options("workspace-b"));
  EXPECT_EQ(workspace_b.list_agents(AgentQuery{.now_ms = now}).size(),
            std::size_t{1});
  EXPECT_TRUE(error_code([&] {
                reopened.list_agents(
                    AgentQuery{.workspace_id = "workspace-b", .now_ms = now});
              }) == MailboxErrorCode::permission_denied);
  const auto cross_workspace = global.send(EnqueueMailboxEntryRequest{
      .sender_agent_id = "agent-a",
      .sender_session_id = "session-a",
      .target = MailboxTarget{.agent_id = "agent-b2"},
      .workspace_id = "workspace-b",
      .body = MailboxPayload{.text = "cross-workspace"},
      .created_at_ms = now,
  });
  EXPECT_EQ(
      workspace_b.inspect(InboxQuery{.session_id = "session-b2", .now_ms = now})
          .front()
          .entry_id,
      cross_workspace.entry_id);
  EXPECT_TRUE(error_code([&] {
                reopened.send(EnqueueMailboxEntryRequest{
                    .sender_agent_id = "agent-a",
                    .sender_session_id = "session-a",
                    .target = MailboxTarget{.agent_id = "agent-b2"},
                    .workspace_id = "workspace-b",
                    .body = MailboxPayload{.text = "blocked"},
                    .created_at_ms = now});
              }) == MailboxErrorCode::permission_denied);

  const auto cleanup_process = process("process-cleanup", "workspace-a", now);
  reopened.register_process(cleanup_process);
  reopened.register_agent(
      agent("agent-parent", "process-cleanup", "cleanup", now));
  reopened.register_agent(agent("agent-child", "process-cleanup", "child", now,
                                std::string("agent-parent")));
  reopened.close_process("process-cleanup", now);
  now += 500;
  const auto cleanup = reopened.cleanup(CleanupRequest{
      .now_ms = now, .acknowledged_retention_ms = 0, .stale_retention_ms = 0});
  EXPECT_TRUE(cleanup.agents_removed >= 2);
  EXPECT_TRUE(cleanup.processes_removed >= 1);

  reopened.register_process(process("process-root-claim", "workspace-a", now));
  reopened.register_agent(
      agent("root-claim", "process-root-claim", "shared-session", now));
  const auto root_only = reopened.send(EnqueueMailboxEntryRequest{
      .entry_id = "root-only-claim",
      .sender_agent_id = "agent-a",
      .sender_session_id = "session-a",
      .target = MailboxTarget{.session_id = "shared-session"},
      .workspace_id = "workspace-a",
      .body = MailboxPayload{.text = "root only"},
      .created_at_ms = now});
  reopened.register_agent(agent("child-claim", "process-root-claim",
                                "shared-session", now,
                                std::string("root-claim")));
  reopened.register_agent(agent("child-claim-2", "process-root-claim",
                                "shared-session", now,
                                std::string("root-claim")));
  const auto session_with_children = reopened.send(EnqueueMailboxEntryRequest{
      .entry_id = "session-with-children",
      .sender_agent_id = "agent-a",
      .sender_session_id = "session-a",
      .target = MailboxTarget{.session_id = "shared-session"},
      .workspace_id = "workspace-a",
      .body = MailboxPayload{.text = "root attachment"},
      .created_at_ms = now,
  });
  EXPECT_TRUE(!session_with_children.recipient_agent_id.has_value());
  EXPECT_TRUE(reopened
                  .claim(ClaimRequest{.session_id = "shared-session",
                                      .agent_id = "child-claim",
                                      .now_ms = now})
                  .messages.empty());
  EXPECT_TRUE(reopened
                  .inspect(InboxQuery{.session_id = "shared-session",
                                      .agent_id = "child-claim-2",
                                      .agent_kind = "subagent",
                                      .now_ms = now})
                  .empty());
  const auto root_claim = reopened.claim(ClaimRequest{
      .session_id = "shared-session", .agent_id = "root-claim", .now_ms = now});
  EXPECT_EQ(root_claim.messages.size(), std::size_t{2});
  EXPECT_EQ(root_claim.messages.front().entry_id, root_only.entry_id);
  EXPECT_EQ(root_claim.messages.back().entry_id,
            session_with_children.entry_id);
}

TEST_F(MailboxStoreTest, RejectsUnsafePathsAndSchemas) {
  const auto insecure_dir = directory.path() / "insecure";
  std::filesystem::create_directories(insecure_dir);
  std::filesystem::permissions(insecure_dir,
                               std::filesystem::perms::owner_all |
                                   std::filesystem::perms::group_read,
                               std::filesystem::perm_options::replace);
  EXPECT_TRUE(error_code([&] {
                MailboxStore insecure(MailboxStoreOptions{
                    .path = insecure_dir / "mailbox.sqlite3",
                    .workspace_id = "workspace-a",
                    .workspace_path = "/tmp/workspace",
                    .clock = [&] { return now; },
                    .id_generator = [&] { return "insecure"; }});
              }) == MailboxErrorCode::permission_denied);

  const auto symlink_target = directory.path() / "symlink-target.sqlite3";
  sqlite3 *symlink_database = nullptr;
  EXPECT_EQ(sqlite3_open(symlink_target.c_str(), &symlink_database), SQLITE_OK);
  sqlite3_close(symlink_database);
  std::filesystem::permissions(symlink_target,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  const auto symlink_path = directory.path() / "symlink.sqlite3";
  std::filesystem::create_symlink(symlink_target, symlink_path);
  EXPECT_TRUE(error_code([&] {
                MailboxStore symlink(MailboxStoreOptions{
                    .path = symlink_path,
                    .workspace_id = "workspace-a",
                    .workspace_path = "/tmp/workspace",
                    .clock = [&] { return now; },
                    .id_generator = [&] { return "symlink"; }});
              }) == MailboxErrorCode::permission_denied);

  const auto nonregular_path = directory.path() / "nonregular.sqlite3";
  std::filesystem::create_directory(nonregular_path);
  EXPECT_TRUE(error_code([&] {
                MailboxStore nonregular(MailboxStoreOptions{
                    .path = nonregular_path,
                    .workspace_id = "workspace-a",
                    .workspace_path = "/tmp/workspace",
                    .clock = [&] { return now; },
                    .id_generator = [&] { return "nonregular"; }});
              }) == MailboxErrorCode::permission_denied);

  const auto corrupt_path = directory.path() / "corrupt.sqlite3";
  {
    std::ofstream corrupt(corrupt_path);
    corrupt << "not a sqlite database";
  }
  std::filesystem::permissions(corrupt_path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  EXPECT_TRUE(error_code([&] {
                MailboxStore corrupt_store(MailboxStoreOptions{
                    .path = corrupt_path,
                    .workspace_id = "workspace-a",
                    .workspace_path = "/tmp/workspace",
                    .clock = [&] { return now; },
                    .id_generator = [&] { return "corrupt"; }});
              }) == MailboxErrorCode::corrupt);

  const auto newer_path = directory.path() / "newer.sqlite3";
  sqlite3 *database = nullptr;
  EXPECT_EQ(sqlite3_open(newer_path.c_str(), &database), SQLITE_OK);
  EXPECT_EQ(sqlite3_exec(database, "PRAGMA user_version=99", nullptr, nullptr,
                         nullptr),
            SQLITE_OK);
  sqlite3_close(database);
  std::filesystem::permissions(newer_path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace);
  const auto newer_options = MailboxStoreOptions{
      .path = newer_path,
      .workspace_id = "workspace-a",
      .workspace_path = "/tmp/workspace",
      .clock = [&] { return now; },
      .id_generator = [&] { return "newer-" + std::to_string(++next_id); },
  };
  EXPECT_TRUE(error_code([&] { MailboxStore newer(newer_options); }) ==
              MailboxErrorCode::incompatible_schema);
}
