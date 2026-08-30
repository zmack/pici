#include "cli/mailbox_runtime.h"

#include "cli/config.h"
#include "core/session/session_id.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <unistd.h>
#include <utility>

namespace pi::cli {

namespace {

std::string local_hostname() {
  std::array<char, 256> buffer{};
  if (::gethostname(buffer.data(), buffer.size() - 1) == 0) {
    buffer.back() = '\0';
    return {buffer.data()};
  }
  return "local";
}

std::string workspace_identity(const std::filesystem::path &workspace_path) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const auto character : workspace_path.string()) {
    hash ^= static_cast<unsigned char>(character);
    hash *= 1099511628211ULL;
  }
  std::ostringstream result;
  result << "workspace-" << std::hex << hash;
  return result.str();
}

} // namespace

core::MailboxLaunchOptions
resolve_mailbox_launch_options(const Args &args, const core::Model &model,
                               std::filesystem::path workspace_path) {
  std::error_code canonical_error;
  const auto canonical =
      std::filesystem::weakly_canonical(workspace_path, canonical_error);
  if (!canonical_error)
    workspace_path = canonical;
  else
    workspace_path = workspace_path.lexically_normal();

  const auto settings =
      args.config_document ? args.config_document->mailbox : MailboxSettings{};
  std::filesystem::path mailbox_path =
      args.mailbox_path.empty() ? settings.path
                                : std::filesystem::path(args.mailbox_path);
  if (mailbox_path.empty())
    mailbox_path = default_mailbox_path(
        args.config_path.empty() ? default_config_path()
                                 : std::filesystem::path(args.config_path));

  core::MailboxLaunchOptions result;
  result.resolved_path = mailbox_path;
  result.workspace_path = workspace_path;
  auto &options = result.coordinator;
  options.store.path = std::move(mailbox_path);
  options.store.workspace_id = workspace_identity(workspace_path);
  options.store.workspace_path = workspace_path.string();
  options.store.scope = settings.scope == "global"
                            ? core::MailboxScope::global
                            : core::MailboxScope::workspace;
  options.store.claim_lease_ms = settings.claim_lease_ms;
  options.store.retention_days = settings.retention_days;
  options.store.clock = [] {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  options.store.id_generator = [] { return core::generate_session_id(); };
  options.process_id = core::generate_session_id();
  options.root_agent_id = core::generate_session_id();
  options.provider = model.provider;
  options.model_id = model.id;
  options.hostname = local_hostname();
  options.heartbeat_interval =
      std::chrono::milliseconds(settings.heartbeat_interval_ms);
  options.stale_after = std::chrono::milliseconds(settings.stale_after_ms);
  options.poll_interval = std::chrono::milliseconds(settings.poll_interval_ms);
  options.cleanup_interval = std::chrono::hours(1);
  return result;
}

} // namespace pi::cli
