#include "core/auth/authentication.h"

#include "core/auth/authentication_adapter.h"
#include "core/auth/credential_store.h"
#include "core/auth/openai_codex_oauth.h"
#include "core/auth_types.h"
#include "core/env_api_keys.h"
#include "core/models.h"

#include <cctype>
#include <cstdlib>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace pi::auth {

void AuthenticationAdapterCollection::register_adapter(std::string adapter_id,
                                                       Adapter adapter) {
  if (adapter_id.empty())
    throw std::invalid_argument("authentication adapter id must not be empty");
  if (!adapter)
    throw std::invalid_argument("authentication adapter must not be null");
  std::scoped_lock lock(mutex_);
  adapters_[std::move(adapter_id)] = std::move(adapter);
}

bool AuthenticationAdapterCollection::has_adapter(
    std::string_view adapter_id) const {
  std::scoped_lock lock(mutex_);
  return adapters_.contains(std::string(adapter_id));
}

AuthenticationAdapterCollection::Adapter
AuthenticationAdapterCollection::get_adapter(
    std::string_view adapter_id) const {
  std::scoped_lock lock(mutex_);
  const auto it = adapters_.find(std::string(adapter_id));
  return it == adapters_.end() ? nullptr : it->second;
}

std::string Authentication::canonical_provider(std::string_view provider) {
  std::string result(provider);
  for (char &c : result)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

Authentication::Authentication(
    std::shared_ptr<const core::ModelCatalog> catalog)
    : catalog_(std::move(catalog)),
      owned_credential_store_(std::make_shared<CredentialStore>()),
      credential_store_(owned_credential_store_.get()),
      adapters_(std::make_shared<AuthenticationAdapterCollection>()) {
  register_adapter("openai-codex-oauth",
                   std::make_shared<OpenAICodexOAuth>(*credential_store_));
  if (!catalog_)
    return;
  for (const auto &[id, definition] : catalog_->providers()) {
    if (!definition.api_key.env_var)
      continue;
    const char *value = std::getenv( // NOLINT(concurrency-mt-unsafe)
        definition.api_key.env_var->c_str());
    configured_env_keys_[id] = value != nullptr && *value != '\0'
                                   ? std::optional<std::string>(value)
                                   : std::nullopt;
  }
}

Authentication::Authentication(
    std::shared_ptr<const core::ModelCatalog> catalog,
    CredentialStore &credential_store,
    std::shared_ptr<AuthenticationAdapterCollection> adapters)
    : catalog_(std::move(catalog)), credential_store_(&credential_store),
      adapters_(std::move(adapters)) {
  if (!adapters_)
    adapters_ = std::make_shared<AuthenticationAdapterCollection>();
  if (!adapters_->has_adapter("openai-codex-oauth"))
    register_adapter("openai-codex-oauth",
                     std::make_shared<OpenAICodexOAuth>(credential_store));
  if (!catalog_)
    return;
  for (const auto &[id, definition] : catalog_->providers()) {
    if (!definition.api_key.env_var)
      continue;
    const char *value = std::getenv( // NOLINT(concurrency-mt-unsafe)
        definition.api_key.env_var->c_str());
    configured_env_keys_[id] = value != nullptr && *value != '\0'
                                   ? std::optional<std::string>(value)
                                   : std::nullopt;
  }
}

void Authentication::register_adapter(
    std::string adapter_id, AuthenticationAdapterCollection::Adapter adapter) {
  adapters_->register_adapter(std::move(adapter_id), std::move(adapter));
}

AuthenticationAdapterCollection::Adapter
Authentication::adapter(std::string_view adapter_id) const {
  return adapters_->get_adapter(adapter_id);
}

const core::Provider *
Authentication::provider_definition(std::string_view provider) const {
  return catalog_ ? catalog_->provider(canonical_provider(provider)) : nullptr;
}

void Authentication::set_runtime_api_key(std::string_view provider,
                                         std::string key) {
  if (!key.empty())
    runtime_api_keys_[canonical_provider(provider)] = std::move(key);
}

void Authentication::clear_runtime_api_key(std::string_view provider) {
  runtime_api_keys_.erase(canonical_provider(provider));
}

std::optional<core::RequestAuth>
Authentication::resolve_api_key(std::string_view provider,
                                const core::Provider *definition,
                                std::string_view explicit_api_key) const {
  const auto canonical = canonical_provider(provider);
  const auto policy = definition != nullptr
                          ? definition->auth
                          : core::ProviderAuthPolicy::optional;
  if (policy == core::ProviderAuthPolicy::none)
    return std::nullopt;
  if (!explicit_api_key.empty())
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = std::string(explicit_api_key),
                             .source = "runtime-provider-key"};
  if (const auto it = runtime_api_keys_.find(canonical);
      it != runtime_api_keys_.end())
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = it->second,
                             .source = "runtime-provider-key"};
  if (definition != nullptr && definition->api_key.literal &&
      !definition->api_key.literal->empty())
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = definition->api_key.literal,
                             .source = "config-literal"};
  if (const auto it = configured_env_keys_.find(canonical);
      it != configured_env_keys_.end() && it->second)
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = it->second,
                             .source = "config-environment"};
  if (auto key = core::get_env_api_key(canonical))
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = std::move(key),
                             .source = "environment"};
  if (policy == core::ProviderAuthPolicy::required)
    throw AuthError("missing authentication for provider '" + canonical +
                    "'; configure api_key, api_key_env, or its environment "
                    "credential");
  return std::nullopt;
}

AuthAvailability
Authentication::api_key_availability(std::string_view provider,
                                     const core::Provider *definition) const {
  const auto canonical = canonical_provider(provider);
  if (definition != nullptr &&
      definition->auth == core::ProviderAuthPolicy::none)
    return AuthAvailability::not_required;
  if (runtime_api_keys_.contains(canonical) ||
      (definition != nullptr && definition->api_key.literal &&
       !definition->api_key.literal->empty()) ||
      (configured_env_keys_.contains(canonical) &&
       !configured_env_keys_.at(canonical).value_or("").empty()) ||
      core::get_env_api_key(canonical))
    return AuthAvailability::configured;
  if (definition == nullptr ||
      definition->auth == core::ProviderAuthPolicy::optional)
    return AuthAvailability::not_required;
  return AuthAvailability::missing;
}

AuthAvailability Authentication::availability(std::string_view provider) const {
  const auto canonical = canonical_provider(provider);
  const auto *definition = provider_definition(canonical);
  if (definition == nullptr || definition->authentication.kind ==
                                   core::AuthenticationBindingKind::api_key)
    return api_key_availability(canonical, definition);
  if (definition->authentication.kind == core::AuthenticationBindingKind::none)
    return AuthAvailability::not_required;
  const auto selected = adapter(definition->authentication.adapter_id);
  if (selected == nullptr)
    return AuthAvailability::missing;
  try {
    return selected->availability(canonical);
  } catch (...) {
    return AuthAvailability::missing;
  }
}

std::optional<core::RequestAuth>
Authentication::resolve(std::string_view provider,
                        std::string_view explicit_api_key,
                        const std::stop_token &stop_tok) const {
  const auto canonical = canonical_provider(provider);
  const auto *definition = provider_definition(canonical);
  if (definition == nullptr || definition->authentication.kind ==
                                   core::AuthenticationBindingKind::api_key)
    return resolve_api_key(canonical, definition, explicit_api_key);
  if (definition->authentication.kind == core::AuthenticationBindingKind::none)
    return std::nullopt;
  const auto selected = adapter(definition->authentication.adapter_id);
  if (selected == nullptr)
    throw AuthError("unknown authentication adapter '" +
                    definition->authentication.adapter_id + "' for provider '" +
                    canonical + "'");
  try {
    std::string adapter_key;
    if (explicit_api_key.empty()) {
      if (const auto it = runtime_api_keys_.find(canonical);
          it != runtime_api_keys_.end())
        adapter_key = it->second;
    }
    const auto adapter_explicit_key = explicit_api_key.empty()
                                          ? std::string_view(adapter_key)
                                          : explicit_api_key;
    return selected->resolve(canonical, adapter_explicit_key, stop_tok);
  } catch (const AuthError &) {
    throw;
  } catch (const std::exception &error) {
    if (stop_tok.stop_requested())
      throw;
    throw AuthError("authentication failed for provider '" + canonical +
                    "': " + error.what());
  }
}

} // namespace pi::auth
