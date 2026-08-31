#include "core/auth/auth_resolver.h"

#include "core/models.h"
#include "support/gtest_helpers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <stop_token>
#include <string>

using namespace pi;
using namespace testing;

namespace {

std::shared_ptr<const core::ModelCatalog>
registry_with_auth(core::ProviderAuthPolicy policy,
                   std::optional<std::string> env_var = std::nullopt) {
  core::ProviderConfig provider;
  provider.id = "test-provider";
  provider.api = "auth-test";
  provider.base_url = "http://auth.test/v1";
  provider.auth = policy;
  provider.api_key.env_var = std::move(env_var);
  return std::make_shared<const core::ModelCatalog>(
      std::map<std::string, core::ProviderConfig>{{"test-provider", provider}});
}

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

TEST(AuthResolver, ProviderScopedCredentialsAndHeaders) {
  pi::test::ScopedEnvironmentVariable auth_key("PICI_AUTH_TEST_A",
                                               "provider-a-key");

  core::ProviderConfig provider_a;
  provider_a.id = "provider-a";
  provider_a.api = "openai-completions";
  provider_a.base_url = "http://a.test/v1";
  provider_a.auth = core::ProviderAuthPolicy::required;
  provider_a.api_key.env_var = "PICI_AUTH_TEST_A";

  core::ProviderConfig provider_b;
  provider_b.id = "provider-b";
  provider_b.api = "openai-completions";
  provider_b.base_url = "http://b.test/v1";
  provider_b.auth = core::ProviderAuthPolicy::required;

  auto registry = std::make_shared<const core::ModelCatalog>(
      std::map<std::string, core::ProviderConfig>{{"provider-a", provider_a},
                                                  {"provider-b", provider_b}});
  auth::AuthResolver resolver(registry);
  const auto a = resolver.resolve("provider-a");
  ASSERT_THAT(a, Optional(Field(&core::RequestAuth::bearer_token,
                                Optional(StrEq("provider-a-key")))));
  EXPECT_THAT(resolver.availability("provider-a"),
              Eq(auth::AuthAvailability::configured));
  EXPECT_THAT(resolver.availability("provider-b"),
              Eq(auth::AuthAvailability::missing));

  resolver.set_runtime_api_key("provider-a", "runtime-a");
  const auto runtime = resolver.resolve("provider-a");
  ASSERT_THAT(runtime, Optional(Field(&core::RequestAuth::bearer_token,
                                      Optional(StrEq("runtime-a")))));
  try {
    (void)resolver.resolve("provider-b");
    FAIL() << "missing provider credential did not throw";
  } catch (const auth::AuthError &) {
  }

  std::map<std::string, std::string> headers{{"authorization", "bad"},
                                             {"X-Test", "old"}};
  core::merge_headers_case_insensitive(
      headers, {{"Authorization", "good"}, {"x-test", "new"}});
  EXPECT_THAT(headers, ElementsAre(Pair("Authorization", "good"),
                                   Pair("x-test", "new")));
}

TEST(AuthResolver, NoAuthPolicyReturnsNoRequestCredential) {
  auto registry = registry_with_auth(core::ProviderAuthPolicy::none);
  auth::AuthResolver resolver(registry);

  EXPECT_FALSE(resolver.resolve("test-provider").has_value());
  EXPECT_EQ(resolver.availability("test-provider"),
            auth::AuthAvailability::not_required);
}

TEST(AuthResolver, OptionalAuthWithoutCredentialIsNotRequired) {
  auto registry = registry_with_auth(core::ProviderAuthPolicy::optional);
  auth::AuthResolver resolver(registry);

  EXPECT_FALSE(resolver.resolve("test-provider").has_value());
  EXPECT_EQ(resolver.availability("test-provider"),
            auth::AuthAvailability::not_required);
}

TEST(AuthResolver, RequiredAuthWithoutCredentialIsMissing) {
  auto registry = registry_with_auth(core::ProviderAuthPolicy::required);
  auth::AuthResolver resolver(registry);

  EXPECT_EQ(resolver.availability("test-provider"),
            auth::AuthAvailability::missing);
  EXPECT_THROW(resolver.resolve("test-provider"), auth::AuthError);
}

TEST(AuthResolver, EnvironmentCredentialIsSnapshottedAtConstruction) {
  pi::test::ScopedEnvironmentVariable environment("PICI_AUTH_SNAPSHOT",
                                                  "captured-value");
  auto registry = registry_with_auth(core::ProviderAuthPolicy::required,
                                     "PICI_AUTH_SNAPSHOT");
  auth::AuthResolver resolver(registry);
  ::unsetenv("PICI_AUTH_SNAPSHOT");

  const auto resolved = resolver.resolve("test-provider");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->bearer_token,
            std::optional<std::string>{"captured-value"});
  EXPECT_EQ(resolved->source, "config-environment");
}

TEST(AuthResolver, OAuthAvailabilityIsProviderScoped) {
  pi::test::TemporaryDirectory directory{"pici-auth-resolver"};
  auth::CredentialStore store(directory.path() / "auth.json");
  store.modify_oauth(
      "openai-codex", [](const std::optional<auth::OAuthCredential> &) {
        return std::optional<auth::OAuthCredential>{
            auth::OAuthCredential{.access_token = "access",
                                  .refresh_token = "refresh",
                                  .expires_at_ms = now_ms() + 60 * 60 * 1000,
                                  .account_id = "account"}};
      });

  auth::OpenAICodexOAuth oauth(std::move(store));
  auth::AuthResolver resolver(std::make_shared<const core::ModelCatalog>(),
                              std::move(oauth));

  EXPECT_EQ(resolver.availability("openai-codex"),
            auth::AuthAvailability::configured);
  EXPECT_EQ(resolver.availability("other-provider"),
            auth::AuthAvailability::not_required);
  const auto resolved = resolver.resolve("openai-codex");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->kind, core::AuthKind::oauth);
  EXPECT_EQ(resolved->bearer_token, std::optional<std::string>{"access"});
}

TEST(AuthResolver, OAuthRefreshHonorsCancellation) {
  pi::test::TemporaryDirectory directory{"pici-auth-resolver-cancel"};
  auth::CredentialStore store(directory.path() / "auth.json");
  store.modify_oauth(
      "openai-codex", [](const std::optional<auth::OAuthCredential> &) {
        return std::optional<auth::OAuthCredential>{
            auth::OAuthCredential{.access_token = "expired-access",
                                  .refresh_token = "expired-refresh",
                                  .expires_at_ms = now_ms() - 1,
                                  .account_id = "account"}};
      });

  auth::OpenAICodexOAuth oauth(std::move(store));
  auth::AuthResolver resolver(std::make_shared<const core::ModelCatalog>(),
                              std::move(oauth));
  std::stop_source stop_source;
  stop_source.request_stop();

  EXPECT_THROW(resolver.resolve("openai-codex", {}, stop_source.get_token()),
               std::runtime_error);
}
