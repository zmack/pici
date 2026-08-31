#include "core/process/pici_process.h"

#include "core/auth/authentication_adapter.h"
#include "core/llm_client.h"
#include "core/models.h"
#include "core/session/session_record.h"
#include "support/gtest_helpers.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void register_builtin_clients() {
  for (const auto &provider : pi::core::ModelCatalog::builtin_providers()) {
    pi::core::LLMClientRegistry::instance().register_client(
        provider.api, [] { return std::shared_ptr<pi::core::LLMClient>{}; });
  }
  pi::core::LLMClientRegistry::instance().register_client(
      "test-api", [] { return std::shared_ptr<pi::core::LLMClient>{}; });
}

pi::core::ProviderConfig make_provider(const std::string &id) {
  pi::core::ProviderConfig provider;
  provider.id = id;
  provider.api = "test-api";
  provider.base_url = "http://" + id + ".test/v1";
  provider.auth = pi::core::ProviderAuthPolicy::none;
  return provider;
}

pi::core::PiciProcess::Config two_provider_config(std::string session_dir = {}) {
  register_builtin_clients();
  pi::core::PiciProcess::Config config;
  config.providers = {
      {"alpha", make_provider("alpha")},
      {"beta", make_provider("beta")},
  };
  config.session_dir = std::move(session_dir);
  return config;
}

} // namespace

TEST(PiciProcess, ConstructsAccessorsNonNullAndInternallyConsistent) {
  pi::core::PiciProcess process(two_provider_config());

  ASSERT_NE(process.model_catalog(), nullptr);
  ASSERT_NE(process.authentication(), nullptr);
  ASSERT_NE(process.session_store(), nullptr);

  EXPECT_NE(process.model_catalog()->provider("alpha"), nullptr);
  EXPECT_NE(process.model_catalog()->provider("beta"), nullptr);
  EXPECT_EQ(process.authentication()->availability("alpha"),
            pi::auth::AuthAvailability::not_required);
  EXPECT_EQ(process.authentication()->availability("beta"),
            pi::auth::AuthAvailability::not_required);
}

TEST(PiciProcess, AccessorsAreStableAcrossRepeatedCalls) {
  pi::core::PiciProcess process(two_provider_config());

  EXPECT_EQ(process.model_catalog(), process.model_catalog());
  EXPECT_EQ(process.authentication(), process.authentication());
  EXPECT_EQ(process.session_store(), process.session_store());
}

TEST(PiciProcess, InvalidProviderThrowsAndLeavesNoUsableObject) {
  register_builtin_clients();
  pi::core::PiciProcess::Config config;
  pi::core::ProviderConfig invalid;
  invalid.id = "broken";
  invalid.api = "not-a-registered-api";
  invalid.base_url = "http://broken.test/v1";
  invalid.auth = pi::core::ProviderAuthPolicy::none;
  config.providers = {{"broken", invalid}};

  EXPECT_THROW({ pi::core::PiciProcess process(config); }, std::runtime_error);
}

TEST(PiciProcess, SessionStoreReflectsConfiguredSessionDirectory) {
  pi::test::TemporaryDirectory directory{"pici-process"};
  pi::core::PiciProcess process(
      two_provider_config(directory.path().string()));

  pi::core::SessionHeader header;
  header.id = "pici-process-test-session";
  header.model = "test-model";
  header.provider = "alpha";
  const auto session_id = process.session_store()->create(header);

  const auto expected_path = directory.path() / (session_id + ".jsonl");
  EXPECT_TRUE(std::filesystem::exists(expected_path));
  EXPECT_EQ(process.session_store()->session_path(session_id), expected_path);

  const auto loaded = process.session_store()->load(session_id);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->header.provider, // NOLINT(bugprone-unchecked-optional-access)
            "alpha");
}
