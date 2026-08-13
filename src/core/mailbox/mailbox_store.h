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

  SendReceipt send(const SendRequest &request);
  std::vector<MailboxMessage> inspect(const InboxQuery &query);
  ClaimResult claim(const ClaimRequest &request);
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
