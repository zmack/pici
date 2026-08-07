#include "core/auth/auth_resolver.h"
#include "core/models.h"

#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <string_view>

using namespace pi;

int main() {
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
  if (!a || !a->bearer_token || *a->bearer_token != "provider-a-key")
    return 1;
  if (resolver.availability("provider-a") !=
      auth::AuthAvailability::configured)
    return 1;
  if (resolver.availability("provider-b") != auth::AuthAvailability::missing)
    return 1;

  resolver.set_runtime_api_key("provider-a", "runtime-a");
  const auto runtime = resolver.resolve("provider-a");
  if (!runtime || !runtime->bearer_token || *runtime->bearer_token != "runtime-a")
    return 1;
  try {
    (void)resolver.resolve("provider-b");
    return 1;
  } catch (const auth::AuthError &) {
  }

  std::map<std::string, std::string> headers{{"authorization", "bad"},
                                              {"X-Test", "old"}};
  core::merge_headers_case_insensitive(
      headers, {{"Authorization", "good"}, {"x-test", "new"}});
  if (headers.size() != 2 || headers.at("Authorization") != "good" ||
      headers.at("x-test") != "new")
    return 1;

  std::cout << "auth resolver: provider-scoped credentials and headers passed\n";
  return 0;
}
