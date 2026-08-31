#pragma once

// PiciProcess is the process composition root (docs/object-taxonomy.md): it
// owns the process-lifetime ModelCatalog, Authentication, and SessionStore
// instances that CLI and ACP would otherwise assemble independently. See
// plans/object-taxonomy-migration.md Phase 5.
//
// Mailbox/MailboxCoordinator ownership is deliberately not here yet:
// MailboxCoordinatorOptions requires root_agent_id and initial_session_id,
// values only known once a specific session is opened, and moving
// construction here first would require the multi-attachment redesign that
// is Phase 8's job. Mailbox construction stays inside
// cli::open_runtime_bundle() until then.

#include "core/auth/authentication.h"
#include "core/auth/credential_store.h"
#include "core/models.h"
#include "core/session/session_store.h"

#include <map>
#include <memory>
#include <string>

namespace pi::core {

class PiciProcess {
public:
  struct Config {
    std::map<std::string, ProviderConfig> providers;
    // Empty means "use SessionStore::default_sessions_dir()".
    std::string session_dir;
  };

  // Throws std::runtime_error (propagated from ModelCatalog construction /
  // validate_registered_apis()) on bad provider configuration.
  explicit PiciProcess(const Config &config);

  const std::shared_ptr<const ModelCatalog> &model_catalog() const {
    return model_catalog_;
  }
  const std::shared_ptr<auth::Authentication> &authentication() const {
    return authentication_;
  }
  const std::shared_ptr<SessionStore> &session_store() const {
    return session_store_;
  }

private:
  std::shared_ptr<const ModelCatalog> model_catalog_;
  auth::CredentialStore credential_store_;
  std::shared_ptr<auth::Authentication> authentication_;
  std::shared_ptr<SessionStore> session_store_;
};

} // namespace pi::core
