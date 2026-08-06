#include "core/auth/credential_store.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <source_location>
#include <string_view>
#include <unistd.h>

namespace tests {
int passed{0};
int failed{0};
int total{0};

bool check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
  ++total;
  if (condition) {
    ++passed;
    return true;
  }
  ++failed;
  std::cout << "  FAIL " << location.file_name() << ":" << location.line()
            << " — " << expression << "\n";
  return false;
}
} // namespace tests

#define CHECK(expression) tests::check((expression), #expression)
#define CHECK_EQ(left, right) tests::check((left) == (right), #left " == " #right)

int main() {
  const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("pici-credential-store-" + std::to_string(::getpid()) +
                     "-" + std::to_string(suffix));
  const auto path = root / "config" / "auth.json";
  std::filesystem::remove_all(root);

  try {
    pi::auth::CredentialStore store(path);
    CHECK(!store.read_oauth("openai-codex").has_value());

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
          CHECK(!current.has_value());
          return expected;
        });
    CHECK(saved.has_value());
    CHECK_EQ(saved->access_token, expected.access_token);

    auto loaded = store.read_oauth("openai-codex");
    CHECK(loaded.has_value());
    CHECK_EQ(loaded->refresh_token, expected.refresh_token);
    CHECK_EQ(loaded->expires_at_ms, expected.expires_at_ms);
    CHECK_EQ(loaded->account_id, expected.account_id);

    auto entries = store.list();
    CHECK_EQ(entries.size(), std::size_t{1});
    CHECK_EQ(entries.front().provider, std::string("openai-codex"));
    CHECK_EQ(entries.front().type, std::string("oauth"));

    store.modify_oauth(
        "other-provider",
        [](const std::optional<pi::auth::OAuthCredential> &) {
          return std::optional<pi::auth::OAuthCredential>{
              pi::auth::OAuthCredential{"a2", "r2", 1785988860001,
                                        "account-2"}};
        });
    store.erase("openai-codex");
    CHECK(!store.read_oauth("openai-codex").has_value());
    CHECK(store.read_oauth("other-provider").has_value());

    std::error_code ec;
    const auto file_status = std::filesystem::status(path, ec);
    CHECK(!ec);
    CHECK((file_status.permissions() & std::filesystem::perms::others_write) ==
          std::filesystem::perms::none);
    CHECK((file_status.permissions() & std::filesystem::perms::group_write) ==
          std::filesystem::perms::none);
  } catch (const std::exception &error) {
    std::cout << "  FAIL unexpected exception: " << error.what() << "\n";
    ++tests::failed;
  }

  std::error_code ec;
  std::filesystem::remove_all(root, ec);
  std::cout << "\nTests: " << tests::total << " total, " << tests::passed
            << " passed, " << tests::failed << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
