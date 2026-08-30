#include "core/auth/credential_store.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <string_view>
#include <unistd.h>

TEST(CredentialStore, PersistsAndProtectsCredentials) {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("pici-credential-store-" + std::to_string(::getpid()) +
                     "-" + std::to_string(suffix));
  const auto path = root / "config" / "auth.json";
  std::filesystem::remove_all(root);

  try {
    pi::auth::CredentialStore store(path);
    EXPECT_TRUE(!store.read_oauth("openai-codex").has_value());

    const pi::auth::OAuthCredential expected{
        .access_token = "access-test",
        .refresh_token = "refresh-test",
        .expires_at_ms = 1785988860000,
        .account_id = "account-test",
    };
    auto saved = store.modify_oauth(
        "openai-codex",
        [&](const std::optional<pi::auth::OAuthCredential> &current)
            -> std::optional<pi::auth::OAuthCredential> {
          EXPECT_TRUE(!current.has_value());
          return expected;
        });
    EXPECT_TRUE(saved.has_value());
    EXPECT_EQ(saved->access_token, expected.access_token);

    auto loaded = store.read_oauth("openai-codex");
    EXPECT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->refresh_token, expected.refresh_token);
    EXPECT_EQ(loaded->expires_at_ms, expected.expires_at_ms);
    EXPECT_EQ(loaded->account_id, expected.account_id);

    auto entries = store.list();
    EXPECT_EQ(entries.size(), std::size_t{1});
    EXPECT_EQ(entries.front().provider, std::string("openai-codex"));
    EXPECT_EQ(entries.front().type, std::string("oauth"));

    store.modify_oauth("other-provider",
                       [](const std::optional<pi::auth::OAuthCredential> &) {
                         return std::optional<pi::auth::OAuthCredential>{
                             pi::auth::OAuthCredential{
                                 "a2", "r2", 1785988860001, "account-2"}};
                       });
    store.erase("openai-codex");
    EXPECT_TRUE(!store.read_oauth("openai-codex").has_value());
    EXPECT_TRUE(store.read_oauth("other-provider").has_value());

    std::error_code ec;
    const auto file_status = std::filesystem::status(path, ec);
    EXPECT_TRUE(!ec);
    EXPECT_TRUE(
        (file_status.permissions() & std::filesystem::perms::others_write) ==
        std::filesystem::perms::none);
    EXPECT_TRUE(
        (file_status.permissions() & std::filesystem::perms::group_write) ==
        std::filesystem::perms::none);
  } catch (const std::exception &error) {
    FAIL() << "unexpected exception: " << error.what();
  }

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
}
