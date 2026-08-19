#pragma once

#include "core/auth/credential_store.h"
#include "core/auth_types.h"

#include <chrono>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>

namespace pi::auth {

// Cross-checked against vendor/codex commit
// 7a0e974e08c798d1e8d59d407aeb6e24db1313af. Keep these values isolated so a
// future vendored auth-contract change is easy to review.
struct OpenAICodexOAuthEndpoints {
  std::string client_id{"app_EMoamEEZ73f0CkXaXp7hrann"};
  std::string authorize_url{"https://auth.openai.com/oauth/authorize"};
  std::string token_url{"https://auth.openai.com/oauth/token"};
  std::string redirect_uri{"http://localhost:1455/auth/callback"};
  std::string device_user_code_url{
      "https://auth.openai.com/api/accounts/deviceauth/usercode"};
  std::string device_token_url{
      "https://auth.openai.com/api/accounts/deviceauth/token"};
  std::string device_verification_uri{"https://auth.openai.com/codex/device"};
  std::string device_redirect_uri{
      "https://auth.openai.com/deviceauth/callback"};
  std::string scope{"openid profile email offline_access api.connectors.read "
                    "api.connectors.invoke"};
};

struct PkcePair {
  std::string verifier;
  std::string challenge;
};

enum class OpenAICodexLoginMode {
  browser,
  device,
};

struct OpenAICodexLoginOptions {
  OpenAICodexLoginMode mode{OpenAICodexLoginMode::browser};
  std::function<void(std::string_view)> notify;
  std::chrono::seconds timeout{std::chrono::minutes(15)};
};

std::string base64url_encode(std::string_view bytes);
std::optional<std::string> base64url_decode(std::string_view encoded);
std::string pkce_challenge(std::string_view verifier);
PkcePair generate_pkce();
std::string generate_oauth_state();
std::optional<std::string> extract_chatgpt_account_id(std::string_view jwt);

class OpenAICodexOAuth {
public:
  explicit OpenAICodexOAuth(CredentialStore store = CredentialStore(),
                            OpenAICodexOAuthEndpoints endpoints = {});

  const CredentialStore &store() const { return store_; }
  CredentialStore &store() { return store_; }
  const OpenAICodexOAuthEndpoints &endpoints() const { return endpoints_; }

  OAuthCredential login(const OpenAICodexLoginOptions &options,
                        const std::stop_token &stop_tok = {}) const;
  OAuthCredential refresh(const OAuthCredential &credential,
                          std::stop_token stop_tok = {}) const;
  std::optional<core::RequestAuth> resolve(std::stop_token stop_tok = {}) const;

private:
  OAuthCredential exchange_code(std::string_view code,
                                std::string_view verifier,
                                std::string_view redirect_uri,
                                const std::stop_token &stop_tok) const;

  OAuthCredential login_browser(const OpenAICodexLoginOptions &options,
                                const std::stop_token &stop_tok) const;
  OAuthCredential login_device(const OpenAICodexLoginOptions &options,
                               const std::stop_token &stop_tok) const;

  mutable CredentialStore store_;
  OpenAICodexOAuthEndpoints endpoints_;
};

} // namespace pi::auth
