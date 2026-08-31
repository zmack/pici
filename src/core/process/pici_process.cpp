#include "core/process/pici_process.h"

#include "core/auth/authentication.h"
#include "core/models.h"
#include "core/session/session_store.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace pi::core {

namespace {

std::shared_ptr<const ModelCatalog>
build_model_catalog(const std::map<std::string, ProviderConfig> &providers) {
  auto catalog = std::make_shared<ModelCatalog>(providers);
  catalog->validate_registered_apis();
  return catalog;
}

} // namespace

PiciProcess::PiciProcess(const Config &config)
    : model_catalog_(build_model_catalog(config.providers)),
      authentication_(std::make_shared<auth::Authentication>(
          model_catalog_, credential_store_)),
      session_store_(std::make_shared<SessionStore>(
          config.session_dir.empty()
              ? SessionStore::default_sessions_dir()
              : std::filesystem::path(config.session_dir))) {}

} // namespace pi::core
