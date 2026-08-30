#pragma once

#include "core/mailbox/mailbox_types.h"

#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <vector>

struct sqlite3;

namespace pi::core {

class MailboxStore {
public:
  explicit MailboxStore(MailboxStoreOptions options);
  ~MailboxStore();

  MailboxStore(const MailboxStore &) = delete;
  MailboxStore &operator=(const MailboxStore &) = delete;
  MailboxStore(MailboxStore &&) = delete;
  MailboxStore &operator=(MailboxStore &&) = delete;

  void migrate();
  void register_process(const ProcessRecord &process);
  void heartbeat_process(std::string_view process_id, TimestampMs now,
                         TimestampMs lease_expires_at);
  void close_process(std::string_view process_id, TimestampMs now);

  void register_agent(const AgentRecord &agent);
  void update_agent(const AgentUpdate &update);
  void close_agent(std::string_view agent_id, TimestampMs now);
  std::vector<AgentRecord> list_agents(const AgentQuery &query);

  MailboxEnqueueReceipt send(const EnqueueMailboxEntryRequest &request);
  std::vector<MailboxEntry> inspect(const InboxQuery &query);
  ClaimResult claim(const ClaimRequest &request);
  // Sets delivered_at_ms to `now_ms` on a claimed entry. Callers are the
  // two MailboxCoordinator sites that convert a claimed entry into an
  // AgentInput (claim_idle_root_turn(), poll_inbox()) -- per
  // docs/architecture-lexicon.md's "Delivery converts a claimed actionable
  // entry into agent input," that conversion is delivery, not claim()
  // itself (the raw agents_claim Lua API also calls claim() without ever
  // becoming agent input, so it must not set this). Overwrites on every
  // call, including redelivery after a lease expiry, so it always reflects
  // the most recent delivery attempt rather than only the first.
  void mark_delivered(const std::string &entry_id,
                      const std::string &workspace_id, TimestampMs now_ms);
  void acknowledge(const AcknowledgeRequest &request);
  WaitResult wait_for_change(const WaitRequest &request,
                             std::stop_token stop_token = {});
  MailboxStatus status(const StatusRequest &request);
  CleanupResult cleanup(const CleanupRequest &request);

  const MailboxStoreOptions &options() const { return options_; }

private:
  MailboxStoreOptions options_;
  sqlite3 *database_{nullptr};
  mutable std::mutex mutex_;

  void open();
  void configure_connection();
  void check_workspace(std::string_view workspace_id) const;
};

} // namespace pi::core
