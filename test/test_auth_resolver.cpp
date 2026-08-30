#include "core/auth/auth_resolver.h"

#include "core/models.h"
#include <gtest/gtest.h>

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string_view>

using namespace pi;

TEST(AuthResolver, ProviderScopedCredentialsAndHeaders) {
  const char *previous_auth_key = ::getenv("PICI_AUTH_TEST_A");
  ::setenv("PICI_AUTH_TEST_A", "provider-a-key", 1);

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

  auto registry = std::make_shared<const core::ModelRegistry>(
      std::map<std::string, core::ProviderConfig>{{"provider-a", provider_a},
                                                  {"provider-b", provider_b}});
  auth::AuthResolver resolver(registry);
  const auto a = resolver.resolve("provider-a");
  ASSERT_TRUE(a);
  ASSERT_TRUE(a->bearer_token);
  EXPECT_EQ(*a->bearer_token, "provider-a-key");
  EXPECT_EQ(resolver.availability("provider-a"),
            auth::AuthAvailability::configured);
  EXPECT_EQ(resolver.availability("provider-b"),
            auth::AuthAvailability::missing);

  resolver.set_runtime_api_key("provider-a", "runtime-a");
  const auto runtime = resolver.resolve("provider-a");
  ASSERT_TRUE(runtime);
  ASSERT_TRUE(runtime->bearer_token);
  EXPECT_EQ(*runtime->bearer_token, "runtime-a");
  try {
    (void)resolver.resolve("provider-b");
    FAIL() << "missing provider credential did not throw";
  } catch (const auth::AuthError &) {
  }

  std::map<std::string, std::string> headers{{"authorization", "bad"},
                                             {"X-Test", "old"}};
  core::merge_headers_case_insensitive(
      headers, {{"Authorization", "good"}, {"x-test", "new"}});
  EXPECT_EQ(headers.size(), 2U);
  EXPECT_EQ(headers.at("Authorization"), "good");
  EXPECT_EQ(headers.at("x-test"), "new");
  if (previous_auth_key)
    ::setenv("PICI_AUTH_TEST_A", previous_auth_key, 1);
  else
    ::unsetenv("PICI_AUTH_TEST_A");
}
