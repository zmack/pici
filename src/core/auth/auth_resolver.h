#pragma once

#include "core/auth/openai_codex_oauth.h"
#include "core/models.h"

#include <map>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>

namespace pi::auth {

class AuthError : public std::runtime_error {
public:
  explicit AuthError(const std::string &message)
      : std::runtime_error(message) {}
};

enum class AuthAvailability {
  configured,
  not_required,
  missing,
  expired_or_refresh_needed,
};

class AuthResolver {
public:
  explicit AuthResolver(
      std::shared_ptr<const core::ModelRegistry> registry = {},
      OpenAICodexOAuth oauth = OpenAICodexOAuth());

  std::optional<core::RequestAuth>
  resolve(std::string_view provider, std::string_view explicit_api_key = {},
          std::stop_token stop_tok = {}) const;

  void set_runtime_api_key(std::string_view provider, std::string key);
  void clear_runtime_api_key(std::string_view provider);
  AuthAvailability availability(std::string_view provider) const;

  const OpenAICodexOAuth &oauth() const { return oauth_; }

private:
  static std::string canonical_provider(std::string_view provider);

  std::shared_ptr<const core::ModelRegistry> registry_;
  mutable OpenAICodexOAuth oauth_;
  std::map<std::string, std::string> runtime_api_keys_;
  std::map<std::string, std::optional<std::string>> configured_env_keys_;
};

} // namespace pi::auth
