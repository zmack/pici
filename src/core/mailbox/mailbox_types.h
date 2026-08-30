#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace pi::core {

using TimestampMs = std::int64_t;
using Clock = std::function<TimestampMs()>;
using IdGenerator = std::function<std::string()>;

enum class MailboxScope { workspace, global };

enum class MailboxErrorCode {
  not_found,
  permission_denied,
  ambiguous_target,
  busy,
  invalid_message,
  invalid_claim,
  incompatible_schema,
  corrupt,
  internal,
};

std::string_view mailbox_error_code_to_string(MailboxErrorCode code);

class MailboxError : public std::runtime_error {
public:
  MailboxError(MailboxErrorCode code, std::string_view message);

  MailboxErrorCode code() const noexcept { return code_; }

private:
  MailboxErrorCode code_;
};

enum class MailboxEntryKind { steer, note, request, reply };

std::string_view mailbox_entry_kind_to_string(MailboxEntryKind kind);
std::optional<MailboxEntryKind>
mailbox_entry_kind_from_string(std::string_view value);

struct MailboxStoreOptions {
  std::filesystem::path path;
  std::string workspace_id;
  std::string workspace_path;
  MailboxScope scope{MailboxScope::workspace};
  std::int64_t claim_lease_ms{30'000};
  std::int64_t retention_days{30};
  Clock clock;
  IdGenerator id_generator;
};

struct ProcessRecord {
  std::string process_id;
  std::string workspace_id;
  std::string workspace_path;
  std::int64_t pid{0};
  std::string hostname;
  std::int64_t protocol_version{1};
  std::string capabilities_json{"[]"};
  TimestampMs started_at_ms{0};
  TimestampMs last_seen_at_ms{0};
  TimestampMs lease_expires_at_ms{0};
  std::optional<TimestampMs> closed_at_ms;
};

struct AgentRecord {
  std::string agent_id;
  std::string process_id;
  std::string kind;
  std::optional<std::string> owner_agent_id;
  std::string session_id;
  std::optional<std::string> session_name;
  std::optional<std::string> task_id;
  std::optional<std::string> task_path;
  std::string provider;
  std::string model_id;
  std::string status;
  TimestampMs started_at_ms{0};
  std::optional<TimestampMs> closed_at_ms;
  std::string workspace_id;
  std::string workspace_path;
  TimestampMs last_seen_at_ms{0};
  TimestampMs lease_expires_at_ms{0};
};

struct AgentUpdate {
  std::string agent_id;
  std::optional<std::string> workspace_id;
  std::optional<std::string> session_name;
  std::optional<std::string> provider;
  std::optional<std::string> model_id;
  std::optional<std::string> status;
  // An empty outer optional leaves the column unchanged. An engaged inner
  // optional sets the column, including an explicit null to reopen an agent.
  std::optional<std::optional<TimestampMs>> closed_at_ms;
};

struct AgentQuery {
  std::optional<std::string> workspace_id;
  std::optional<std::string> session_id;
  std::optional<std::string> agent_id;
  bool include_stale{false};
  bool include_closed{false};
  std::size_t limit{100};
  TimestampMs now_ms{0};
};

struct MailboxTarget {
  std::optional<std::string> session_id;
  std::optional<std::string> agent_id;
};

struct MailboxPayload {
  std::string text;
  std::vector<std::pair<std::string, std::string>> metadata;
};

struct EnqueueMailboxEntryRequest {
  // In-memory field name only; the SQLite column and Lua-facing JSON key
  // stay "message_id"/"reply_to" -- see plans/session-runtime-migration.md
  // Phase 4b. Translation happens at mailbox_store.cpp's SQL bind/read
  // sites and mailbox_bindings.cpp's Lua JSON sites.
  std::optional<std::string> entry_id;
  std::string sender_agent_id;
  std::string sender_session_id;
  MailboxTarget target;
  std::string workspace_id;
  MailboxEntryKind kind{MailboxEntryKind::note};
  MailboxPayload body;
  std::optional<std::string> reply_to_entry_id;
  TimestampMs created_at_ms{0};
  TimestampMs available_at_ms{0};
};

struct MailboxEnqueueReceipt {
  std::string entry_id;
  std::string recipient_session_id;
  std::optional<std::string> recipient_agent_id;
  TimestampMs created_at_ms{0};
};

struct MailboxEntry {
  std::string entry_id;
  std::string sender_agent_id;
  std::string sender_session_id;
  std::string recipient_session_id;
  std::optional<std::string> recipient_agent_id;
  std::string workspace_id;
  MailboxEntryKind kind{MailboxEntryKind::note};
  MailboxPayload body;
  std::optional<std::string> reply_to_entry_id;
  TimestampMs created_at_ms{0};
  TimestampMs available_at_ms{0};
  std::optional<std::string> claim_agent_id;
  std::optional<std::string> claim_token;
  std::optional<TimestampMs> claim_expires_at_ms;
  std::optional<TimestampMs> delivered_at_ms;
  std::optional<TimestampMs> acknowledged_at_ms;
  std::optional<TimestampMs> failed_at_ms;
  std::optional<std::string> failure;
};

struct InboxQuery {
  std::string session_id;
  std::optional<std::string> workspace_id;
  std::optional<std::string> agent_id;
  // Required with agent_id for caller-scoped inspection.  An empty value is
  // retained only for privileged store diagnostics.
  std::string agent_kind;
  std::optional<std::string> entry_id;
  std::vector<MailboxEntryKind> kinds;
  bool include_acknowledged{false};
  bool claimable_only{false};
  std::size_t limit{50};
  TimestampMs now_ms{0};
};

struct ClaimRequest {
  std::string session_id;
  std::string agent_id;
  std::optional<std::string> workspace_id;
  std::vector<MailboxEntryKind> kinds;
  std::size_t limit{50};
  TimestampMs now_ms{0};
  std::int64_t lease_ms{30'000};
};

struct ClaimResult {
  std::vector<MailboxEntry> messages;
};

struct AcknowledgeRequest {
  std::string entry_id;
  std::string agent_id;
  std::string claim_token;
  std::optional<std::string> workspace_id;
  TimestampMs now_ms{0};
};

struct WaitRequest {
  std::string workspace_id;
  std::int64_t after_generation{0};
  std::int64_t timeout_ms{30'000};
  std::int64_t poll_interval_ms{250};
};

struct WaitResult {
  bool timed_out{false};
  std::int64_t generation{0};
  bool presence_changed{false};
  bool messages_changed{false};
};

struct StatusRequest {
  std::string workspace_id;
};

struct MailboxStatus {
  std::int64_t schema_version{0};
  std::string workspace_id;
  std::size_t live_agents{0};
  std::size_t unread_messages{0};
  std::int64_t latest_generation{0};
};

struct CleanupRequest {
  std::optional<std::string> workspace_id;
  TimestampMs now_ms{0};
  std::int64_t acknowledged_retention_ms{0};
  std::int64_t stale_retention_ms{0};
};

struct CleanupResult {
  std::size_t messages_removed{0};
  std::size_t agents_removed{0};
  std::size_t processes_removed{0};
  std::size_t events_removed{0};
};

} // namespace pi::core
