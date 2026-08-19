#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace pi::auth {

struct OAuthCredential {
  std::string access_token;
  std::string refresh_token;
  std::int64_t expires_at_ms{0};
  std::string account_id;
};

struct CredentialInfo {
  std::string provider;
  std::string type;
  std::int64_t expires_at_ms{0};
};

class CredentialStore {
public:
  CredentialStore();
  explicit CredentialStore(std::filesystem::path path);

  const std::filesystem::path &path() const { return path_; }

  std::optional<OAuthCredential> read_oauth(std::string_view provider) const;
  std::vector<CredentialInfo> list() const;

  std::optional<OAuthCredential>
  modify_oauth(std::string_view provider,
               const std::function<std::optional<OAuthCredential>(
                   const std::optional<OAuthCredential> &)> &fn,
               const std::stop_token &stop_tok = {});

  void erase(std::string_view provider, const std::stop_token &stop_tok = {});

private:
  std::filesystem::path path_;
};

std::filesystem::path default_auth_file_path();

} // namespace pi::auth
