#include "core/auth/auth_resolver.h"

#include "core/models.h"
#include "support/gtest_helpers.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <map>
#include <memory>

using namespace pi;
using namespace testing;

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

  auto registry = std::make_shared<const core::ModelRegistry>(
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
