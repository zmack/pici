#include "core/process/pici_process.h"

#include "core/auth/authentication.h"
#include "core/llm_client.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/models.h"
#include "core/session/session_store.h"

#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace pi::core {

namespace {

std::shared_ptr<ModelCatalog> build_model_catalog(
    const std::map<std::string, ProviderConfig> &providers,
    std::shared_ptr<InferenceAdapterCollection> inference_adapters) {
  auto catalog = std::make_shared<ModelCatalog>(providers, nullptr,
                                                std::move(inference_adapters));
  catalog->validate_registered_apis();
  return catalog;
}

} // namespace

PiciProcess::PiciProcess(const Config &config)
    : model_catalog_(
          build_model_catalog(config.providers, config.inference_adapters)),
      authentication_(std::make_shared<auth::Authentication>(
          model_catalog_, credential_store_)),
      session_store_(std::make_shared<SessionStore>(
          config.session_dir.empty()
              ? SessionStore::default_sessions_dir()
              : std::filesystem::path(config.session_dir))) {}

std::shared_ptr<Mailbox>
PiciProcess::ensure_mailbox(const MailboxOptions &options) const {
  std::scoped_lock lock(mailbox_mutex_);
  if (!mailbox_)
    mailbox_ = std::make_shared<Mailbox>(options);
  return mailbox_;
}

} // namespace pi::core
