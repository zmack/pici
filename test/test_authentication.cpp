#include "core/auth/authentication.h"

#include "core/models.h"
#include "support/gtest_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace {

class RecordingAuthenticationAdapter final
    : public pi::auth::AuthenticationAdapter {
public:
  std::optional<pi::core::RequestAuth>
  resolve(std::string_view provider, std::string_view explicit_api_key,
          const std::stop_token &) const override {
    last_provider = std::string(provider);
    last_explicit_key = std::string(explicit_api_key);
    return pi::core::RequestAuth{.kind = pi::core::AuthKind::api_key,
                                 .bearer_token = "binding-token",
                                 .source = "test-binding"};
  }

  pi::auth::AuthAvailability
  availability(std::string_view) const override {
    return pi::auth::AuthAvailability::configured;
  }

  mutable std::string last_provider;
  mutable std::string last_explicit_key;
};

class RecordingDiscoveryAdapter final
    : public pi::core::ModelDiscoveryAdapter {
public:
  pi::core::ProviderModelReport
  discover(const pi::core::ProviderDiscoveryRequest &request,
           std::stop_token) override {
    auth = request.auth;
    return {.provider_id = request.provider.id,
            .models = {},
            .observed_at = std::chrono::system_clock::now(),
            .source_revision = "test"};
  }

  std::optional<pi::core::RequestAuth> auth;
};

std::shared_ptr<const pi::core::ModelCatalog> catalog_for_binding() {
  pi::core::ProviderConfig provider;
  provider.id = "bound-provider";
  provider.api = "bound-api";
  provider.base_url = "http://bound.test/v1";
  provider.auth = pi::core::ProviderAuthPolicy::required;
  provider.authentication_adapter = "bound-auth";
  return std::make_shared<const pi::core::ModelCatalog>(
      std::map<std::string, pi::core::ProviderConfig>{{"bound-provider",
                                                        provider}});
}

} // namespace

TEST(Authentication, ResolvesThroughProviderBinding) {
  auto catalog = catalog_for_binding();
  auto adapter = std::make_shared<RecordingAuthenticationAdapter>();
  pi::auth::Authentication authentication(catalog);
  authentication.register_adapter("bound-auth", adapter);

  const auto resolved = authentication.resolve("bound-provider", "runtime");
  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(resolved->bearer_token, std::optional<std::string>{"binding-token"});
  EXPECT_EQ(adapter->last_provider, "bound-provider");
  EXPECT_EQ(adapter->last_explicit_key, "runtime");
}

TEST(Authentication, DiscoveryAndInferenceUseTheSameBinding) {
  auto discovery = std::make_shared<RecordingDiscoveryAdapter>();
  auto discoveries = std::make_shared<pi::core::ModelDiscoveryAdapterCollection>();
  discoveries->register_adapter("bound-discovery", discovery);
  pi::core::ProviderConfig provider;
  provider.id = "bound-provider";
  provider.api = "bound-api";
  provider.base_url = "http://bound.test/v1";
  provider.auth = pi::core::ProviderAuthPolicy::required;
  provider.authentication_adapter = "bound-auth";
  provider.discovery_adapter = "bound-discovery";
  auto catalog = std::make_shared<pi::core::ModelCatalog>(
      std::map<std::string, pi::core::ProviderConfig>{{"bound-provider",
                                                        provider}},
      discoveries);
  auto adapter = std::make_shared<RecordingAuthenticationAdapter>();
  auto authentication = std::make_shared<pi::auth::Authentication>(catalog);
  authentication->register_adapter("bound-auth", adapter);
  catalog->set_request_auth_resolver(
      [authentication](std::string_view id, const std::stop_token &stop) {
        return authentication->resolve(id, {}, stop);
      });

  ASSERT_EQ(catalog->refresh({"bound-provider"}).size(), std::size_t{12});
  ASSERT_TRUE(discovery->auth.has_value());
  const auto inference = authentication->resolve("bound-provider");
  ASSERT_TRUE(inference.has_value());
  EXPECT_EQ(discovery->auth->bearer_token, inference->bearer_token);
  EXPECT_EQ(discovery->auth->source, inference->source);
}

TEST(Authentication, OAuthAdapterUsesAggregateCredentialStore) {
  pi::test::TemporaryDirectory directory{"pici-authentication"};
  pi::auth::CredentialStore store(directory.path() / "auth.json");
  auto catalog = std::make_shared<const pi::core::ModelCatalog>();
  pi::auth::Authentication authentication(catalog, store);
  const auto adapter = authentication.adapter("openai-codex-oauth");
  const auto oauth =
      std::dynamic_pointer_cast<pi::auth::OpenAICodexOAuth>(adapter);
  ASSERT_NE(oauth, nullptr);
  EXPECT_EQ(&oauth->store(), &store);
}
