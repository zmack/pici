#include "core/auth/auth_resolver.h"

#include "core/auth/credential_store.h"
#include "core/auth/openai_codex_oauth.h"
#include "core/auth_types.h"
#include "core/env_api_keys.h"
#include "core/models.h"

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>

namespace pi::auth {

std::string AuthResolver::canonical_provider(std::string_view provider) {
  std::string result(provider);
  for (char &c : result)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

AuthResolver::AuthResolver(std::shared_ptr<const core::ModelRegistry> registry,
                           OpenAICodexOAuth oauth)
    : registry_(std::move(registry)), oauth_(std::move(oauth)) {
  if (!registry_)
    return;
  for (const auto &[id, definition] : registry_->providers()) {
    if (!definition.api_key.env_var)
      continue;
    // Only called from this constructor, during single-threaded startup,
    // before any worker thread could concurrently call setenv()/putenv().
    const char *value = std::getenv( // NOLINT(concurrency-mt-unsafe)
        definition.api_key.env_var->c_str());
    if (value != nullptr && *value != '\0')
      configured_env_keys_[id] = std::string(value);
    else
      configured_env_keys_[id] = std::nullopt;
  }
}

void AuthResolver::set_runtime_api_key(std::string_view provider,
                                       std::string key) {
  if (!key.empty())
    runtime_api_keys_[canonical_provider(provider)] = std::move(key);
}

void AuthResolver::clear_runtime_api_key(std::string_view provider) {
  runtime_api_keys_.erase(canonical_provider(provider));
}

AuthAvailability AuthResolver::availability(std::string_view provider) const {
  const auto canonical = canonical_provider(provider);
  if (canonical == "openai-codex") {
    std::optional<OAuthCredential> credential;
    try {
      credential = oauth_.store().read_oauth("openai-codex");
    } catch (...) {
      return AuthAvailability::missing;
    }
    if (!credential)
      return AuthAvailability::missing;
    const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return credential->expires_at_ms > now
               ? AuthAvailability::configured
               : AuthAvailability::expired_or_refresh_needed;
  }

  const auto *definition = registry_ ? registry_->provider(canonical) : nullptr;
  if (definition != nullptr &&
      definition->auth == core::ProviderAuthPolicy::none)
    return AuthAvailability::not_required;
  const auto configured_env = configured_env_keys_.find(canonical);
  if (runtime_api_keys_.contains(canonical) ||
      (definition != nullptr && definition->api_key.literal &&
       !definition->api_key.literal->empty()) ||
      (configured_env != configured_env_keys_.end() &&
       !configured_env->second.value_or("").empty()) ||
      core::get_env_api_key(canonical))
    return AuthAvailability::configured;
  if (definition == nullptr ||
      definition->auth == core::ProviderAuthPolicy::optional)
    return AuthAvailability::not_required;
  return AuthAvailability::missing;
}

std::optional<core::RequestAuth>
AuthResolver::resolve(std::string_view provider,
                      std::string_view explicit_api_key,
                      const std::stop_token &stop_tok) const {
  const auto canonical = canonical_provider(provider);
  if (canonical == "openai-codex") {
    if (!explicit_api_key.empty())
      throw AuthError(
          "openai-codex accepts OAuth only; --api-key is not allowed");
    try {
      if (auto auth = oauth_.resolve(stop_tok))
        return auth;
      throw AuthError(
          "Not logged in to openai-codex; run pi-cli auth login openai-codex");
    } catch (const AuthError &) {
      throw;
    } catch (const std::exception &error) {
      if (stop_tok.stop_requested())
        throw;
      throw AuthError(
          "OpenAI login expired or was revoked; run pi-cli auth login "
          "openai-codex (provider: " +
          std::string(error.what()) + ")");
    }
  }

  const core::ProviderDefinition *definition =
      registry_ != nullptr ? registry_->provider(canonical) : nullptr;
  const auto policy = (definition != nullptr)
                          ? definition->auth
                          : core::ProviderAuthPolicy::optional;
  if (policy == core::ProviderAuthPolicy::none)
    return std::nullopt;

  if (!explicit_api_key.empty()) {
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = std::string(explicit_api_key),
                             .source = "runtime-provider-key"};
  }
  if (auto it = runtime_api_keys_.find(canonical);
      it != runtime_api_keys_.end()) {
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = it->second,
                             .source = "runtime-provider-key"};
  }
  if (definition != nullptr && definition->api_key.literal &&
      !definition->api_key.literal->empty()) {
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = definition->api_key.literal,
                             .source = "config-literal"};
  }
  if (auto it = configured_env_keys_.find(canonical);
      it != configured_env_keys_.end() && it->second) {
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = it->second,
                             .source = "config-environment"};
  }
  if (auto key = core::get_env_api_key(canonical)) {
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = std::move(key),
                             .source = "environment"};
  }
  if (policy == core::ProviderAuthPolicy::required) {
    throw AuthError("missing authentication for provider '" + canonical +
                    "'; configure api_key, api_key_env, or its environment "
                    "credential");
  }
  return std::nullopt;
}

} // namespace pi::auth
