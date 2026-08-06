#include "core/auth/auth_resolver.h"

#include "core/auth_types.h"
#include "core/env_api_keys.h"

#include <exception>
#include <optional>
#include <stop_token>
#include <string_view>
#include <utility>

namespace pi::auth {

std::optional<core::RequestAuth>
AuthResolver::resolve(std::string_view provider,
                      std::string_view explicit_api_key,
                      std::stop_token stop_tok) const {
  if (provider == "openai-codex") {
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
  if (!explicit_api_key.empty())
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = std::string(explicit_api_key),
                             .source = "--api-key"};
  if (auto key = core::get_env_api_key(provider))
    return core::RequestAuth{.kind = core::AuthKind::api_key,
                             .bearer_token = std::move(key),
                             .source = "environment"};
  return std::nullopt;
}

} // namespace pi::auth
