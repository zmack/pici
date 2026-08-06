#pragma once

#include "core/auth/openai_codex_oauth.h"

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

class AuthResolver {
public:
  explicit AuthResolver(OpenAICodexOAuth oauth = OpenAICodexOAuth())
      : oauth_(std::move(oauth)) {}

  std::optional<core::RequestAuth>
  resolve(std::string_view provider, std::string_view explicit_api_key = {},
          std::stop_token stop_tok = {}) const;

  const OpenAICodexOAuth &oauth() const { return oauth_; }

private:
  mutable OpenAICodexOAuth oauth_;
};

} // namespace pi::auth
