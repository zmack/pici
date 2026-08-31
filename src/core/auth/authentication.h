#pragma once

#include "core/auth/authentication_adapter.h"
#include "core/auth/openai_codex_oauth.h"
#include "core/models.h"

#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace pi::auth {

class AuthError : public std::runtime_error {
public:
  explicit AuthError(const std::string &message)
      : std::runtime_error(message) {}
};

// Process authentication aggregate. It owns request-resolution policy while
// CredentialStore remains an independently owned durable dependency.
class Authentication {
public:
  explicit Authentication(
      std::shared_ptr<const core::ModelCatalog> catalog = {});
  Authentication(
      std::shared_ptr<const core::ModelCatalog> catalog,
      CredentialStore &credential_store,
      std::shared_ptr<AuthenticationAdapterCollection> adapters = {});

  // TODO(taxonomy-phase-10): remove. This constructor preserves the
  // migration-era injection seam for callers that supplied an OAuth adapter.
  Authentication(std::shared_ptr<const core::ModelCatalog> catalog,
                 OpenAICodexOAuth oauth);

  std::optional<core::RequestAuth>
  resolve(std::string_view provider, std::string_view explicit_api_key = {},
          const std::stop_token &stop_tok = {}) const;

  void set_runtime_api_key(std::string_view provider, std::string key);
  void clear_runtime_api_key(std::string_view provider);
  AuthAvailability availability(std::string_view provider) const;

  void register_adapter(std::string adapter_id,
                        AuthenticationAdapterCollection::Adapter adapter);
  AuthenticationAdapterCollection::Adapter
  adapter(std::string_view adapter_id) const;
  CredentialStore &credential_store() const { return *credential_store_; }

private:
  static std::string canonical_provider(std::string_view provider);
  const core::Provider *provider_definition(std::string_view provider) const;
  std::optional<core::RequestAuth>
  resolve_api_key(std::string_view provider, const core::Provider *definition,
                  std::string_view explicit_api_key) const;
  AuthAvailability api_key_availability(std::string_view provider,
                                        const core::Provider *definition) const;

  std::shared_ptr<const core::ModelCatalog> catalog_;
  std::shared_ptr<CredentialStore> owned_credential_store_;
  CredentialStore *credential_store_{nullptr};
  std::shared_ptr<AuthenticationAdapterCollection> adapters_;
  std::map<std::string, std::string> runtime_api_keys_;
  std::map<std::string, std::optional<std::string>> configured_env_keys_;
};

} // namespace pi::auth
