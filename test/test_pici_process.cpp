#include "core/process/pici_process.h"

#include "core/auth/authentication_adapter.h"
#include "core/llm_client.h"
#include "core/mailbox/mailbox_coordinator.h"
#include "core/mailbox/mailbox_types.h"
#include "core/models.h"
#include "core/session/session_record.h"
#include "support/gtest_helpers.h"

#include <gtest/gtest.h>

#include <chrono>
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

TEST(PiciProcess, EnsureMailboxIsLazyAndSharedAcrossSessions) {
  pi::test::TemporaryDirectory directory{"pici-process-mailbox"};
  std::filesystem::permissions(directory.path(),
                               std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  pi::core::PiciProcess process(two_provider_config());

  pi::core::MailboxOptions options;
  options.store.path = directory.path() / "mailbox.sqlite3";
  options.store.workspace_id = "workspace";
  options.store.workspace_path = directory.path().string();
  options.store.clock = [] { return pi::core::TimestampMs{1'000}; };
  options.store.id_generator = [] { return std::string("generated"); };
  options.process_id = "process-under-test";
  options.provider = "test";
  options.model_id = "test-model";
  options.heartbeat_interval = std::chrono::hours(1);
  options.stale_after = std::chrono::hours(2);

  const auto first = process.ensure_mailbox(options);
  const auto second = process.ensure_mailbox(options);
  ASSERT_NE(first, nullptr);
  EXPECT_EQ(first, second);

  // Two sessions attaching to the process's one Mailbox get independent
  // attachments: activating one's root never touches the other's.
  const auto key_a = first->attach();
  const auto key_b = first->attach();
  first->activate_root(key_a, "session-a");
  EXPECT_TRUE(first->status(key_a).root_active);
  EXPECT_TRUE(!first->status(key_b).root_active);
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
