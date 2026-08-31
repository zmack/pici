#include "core/llm_client.h"
#include "core/models.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <cstdint>
#include <map>
#include <memory>
#include <ranges>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

using namespace pi::core;

namespace {

ModelCatalogEntry project_model(const Model &model) {
  return ModelCatalogEntry{
      .key = {.provider_id = model.provider, .model_id = model.id},
      .display_name = model.name,
      .api = model.api,
      .reasoning = model.reasoning,
      .input_capabilities = model.input_capabilities,
      .context_window = model.context_window,
      .max_tokens = model.max_tokens,
      .cost = {.input_per_mtok = model.cost.input_per_mtok,
               .output_per_mtok = model.cost.output_per_mtok,
               .cache_read_per_mtok = model.cost.cache_read_per_mtok,
               .cache_write_per_mtok = model.cost.cache_write_per_mtok}};
}

} // namespace
static const ProviderRefreshStatus &status_for_fake(const std::vector<ProviderRefreshStatus> &statuses);

static_assert(!std::is_pointer_v<decltype(ModelCatalogEntry{}.key)>);
static_assert(!std::is_pointer_v<decltype(ModelCatalogEntry{}.display_name)>);
static_assert(!std::is_constructible_v<ModelCatalogEntry, const Model *>);
static_assert(std::is_same_v<decltype(ModelCatalogView{}.entries),
                             std::vector<ModelCatalogEntry>>);
static_assert(std::is_same_v<decltype(ModelSelectionRequest{}.key), ModelKey>);

TEST(ModelKey, PreservesOpaqueSlashContainingModelIds) {
  const ModelKey key{.provider_id = "openrouter",
                     .model_id = "accounts/company/models/coder"};

  EXPECT_EQ(key.provider_id, "openrouter");
  EXPECT_EQ(key.model_id, "accounts/company/models/coder");
  EXPECT_EQ(key, (ModelKey{"openrouter", "accounts/company/models/coder"}));
  EXPECT_LT((ModelKey{"openrouter", "a"}), key);
}

TEST(ModelCatalogEntry, IsIndependentFromEffectiveModelStorage) {
  Model source;
  source.provider = "local";
  source.id = "accounts/company/models/coder";
  source.name = "Coder";
  source.api = "openai-completions";
  source.reasoning = true;
  source.input_capabilities = {"text", "image"};
  source.context_window = 131072;
  source.max_tokens = 32768;
  source.cost.input_per_mtok = 0.5;

  const ModelCatalogEntry published = project_model(source);
  source.name = "mutated";
  source.input_capabilities.clear();
  source.cost.input_per_mtok = 9.0;

  EXPECT_EQ(published.key.model_id, "accounts/company/models/coder");
  EXPECT_EQ(published.display_name, "Coder");
  EXPECT_TRUE(published.reasoning);
  EXPECT_EQ(published.input_capabilities,
            (std::vector<std::string>{"text", "image"}));
  EXPECT_EQ(published.cost.input_per_mtok, 0.5);
}

TEST(ModelCatalogView, CarriesGenerationEntriesAndProviderStatus) {
  const auto observed = std::chrono::system_clock::now();
  const ModelCatalogEntry entry{
      .key = {.provider_id = "openai", .model_id = "gpt-4o"},
      .display_name = "GPT-4o",
      .api = "openai-completions"};
  const ProviderModelReport report{.provider_id = "openai",
                                   .models = {entry},
                                   .observed_at = observed,
                                   .source_revision = "catalog-revision-1"};
  const ModelCatalogView published{
      .generation = 7,
      .entries = report.models,
      .refresh_status = {{.provider_id = "openai",
                          .state = ProviderRefreshState::succeeded,
                          .diagnostic = ""}}};

  EXPECT_EQ(published.generation, 7U);
  ASSERT_EQ(published.entries.size(), 1U);
  EXPECT_EQ(published.entries.front().key, entry.key);
  ASSERT_EQ(published.refresh_status.size(), 1U);
  EXPECT_EQ(published.refresh_status.front().state,
            ProviderRefreshState::succeeded);
  EXPECT_EQ(published.refresh_status.front().provider_id, "openai");
  EXPECT_EQ(report.observed_at, observed);
  EXPECT_EQ(report.source_revision, "catalog-revision-1");

  const auto before = published;
  auto independent = before;
  independent.entries.front().display_name = "changed";
  EXPECT_EQ(before.entries.front().display_name, "GPT-4o");
}

TEST(ModelSelectionRequest, ContainsStableKeyAndOnlyValueOverrides) {
  const ModelSelectionRequest request{
      .key = {.provider_id = "openrouter",
              .model_id = "anthropic/claude-sonnet-4-5"},
      .base_url = "https://example.test/v1",
      .thinking_level = ThinkingLevel::high,
      .source = "native-picker"};

  EXPECT_EQ(request.key.provider_id, "openrouter");
  EXPECT_EQ(request.key.model_id, "anthropic/claude-sonnet-4-5");
  EXPECT_EQ(request.base_url, "https://example.test/v1");
  EXPECT_EQ(request.thinking_level, ThinkingLevel::high);
  EXPECT_EQ(request.source, "native-picker");
}

TEST(ModelCatalogCharacterization, ConfiguredPrecedenceAndExactResolution) {
  ProviderConfig local;
  local.id = "local";
  local.api = "registry-faux";
  local.base_url = "http://local.test/v1";
  local.auth = ProviderAuthPolicy::none;
  local.headers["X-Provider"] = "local";

  ConfiguredModel configured;
  configured.id = "same";
  configured.headers["X-Model"] = "yes";
  local.models.push_back(configured);

  ConfiguredModel slash;
  slash.id = "accounts/company/models/coder";
  local.models.push_back(slash);

  ProviderConfig openai;
  openai.id = "openai";
  openai.api = "registry-faux";
  openai.base_url = "http://configured-openai.test/v1";
  openai.auth = ProviderAuthPolicy::optional;
  openai.headers["X-Provider"] = "configured-openai";
  openai.model_overrides["gpt-4o"] = {
      .max_tokens = 4242,
  };

  ModelCatalog registry({{"local", local}, {"openai", openai}});

  const auto *merged = registry.exact("LOCAL", "same");
  ASSERT_NE(merged, nullptr);
  EXPECT_EQ(merged->base_url, "http://local.test/v1");
  EXPECT_EQ(merged->headers.at("X-Provider"), "local");
  EXPECT_EQ(merged->headers.at("X-Model"), "yes");

  const auto *overridden_builtin = registry.exact("openai", "gpt-4o");
  ASSERT_NE(overridden_builtin, nullptr);
  EXPECT_EQ(overridden_builtin->api, "registry-faux");
  EXPECT_EQ(overridden_builtin->base_url, "http://configured-openai.test/v1");
  EXPECT_EQ(overridden_builtin->headers.at("X-Provider"),
            "configured-openai");
  EXPECT_EQ(overridden_builtin->max_tokens, 4242U);

  const auto resolved =
      registry.resolve({.provider = "local",
                        .model = "accounts/company/models/coder",
                        .source = "characterization"});
  ASSERT_TRUE(resolved);
  ASSERT_TRUE(resolved.model.has_value());
  EXPECT_EQ(resolved.model->provider, "local");
  EXPECT_EQ(resolved.model->id, "accounts/company/models/coder");
}

TEST(ModelCatalogCharacterization, SearchPreservesCurrentProviderOrder) {
  const ModelCatalog registry;
  const auto matches = registry.search("gpt-4");

  ASSERT_GE(matches.size(), 5U);
  EXPECT_EQ(matches[0].key.provider_id + "/" + matches[0].key.model_id, "openai/gpt-4o");
  EXPECT_EQ(matches[1].key.provider_id + "/" + matches[1].key.model_id, "openai/gpt-4o-mini");
  EXPECT_EQ(matches[2].key.provider_id + "/" + matches[2].key.model_id, "openai/gpt-4.1");
  EXPECT_EQ(matches[3].key.provider_id + "/" + matches[3].key.model_id, "openai/gpt-4.1-mini");
  EXPECT_EQ(matches[4].key.provider_id + "/" + matches[4].key.model_id, "openai/gpt-4.1-nano");
}

TEST(ModelCatalogCharacterization, RegisteredApisAreValidated) {
  for (const auto &provider : ModelCatalog::builtin_providers()) {
    LLMClientRegistry::instance().register_client(
        provider.api, [] { return std::shared_ptr<LLMClient>{}; });
  }

  EXPECT_NO_THROW(ModelCatalog{}.validate_registered_apis());

  ProviderConfig invalid;
  invalid.id = "unregistered";
  invalid.api = "not-registered-for-characterization";
  invalid.base_url = "http://invalid.test/v1";
  invalid.auth = ProviderAuthPolicy::none;
  const ModelCatalog registry({{"unregistered", invalid}});
  EXPECT_THROW(registry.validate_registered_apis(), std::runtime_error);
}

TEST(ModelCatalog, PublishesImmutableGenerationOneView) {
  const ModelCatalog catalog;
  const auto first = catalog.view();
  const auto second = catalog.view();
  EXPECT_EQ(first.generation, 1U);
  EXPECT_EQ(first, second);
  ASSERT_FALSE(first.entries.empty());
  EXPECT_EQ(first.entries.front().key.provider_id, "openai");
  auto copy = first;
  copy.entries.front().display_name = "changed";
  EXPECT_NE(copy, first);
}

TEST(ModelCatalog, StableSearchAndKeyResolutionUseValues) {
  const ModelCatalog catalog;
  const auto matches = catalog.search("gpt-4o");
  ASSERT_FALSE(matches.empty());
  const auto key = matches.front().key;
  const auto entry = catalog.entry(key);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(*entry, matches.front());
  const auto resolved = catalog.resolve(key);
  ASSERT_TRUE(resolved);
  EXPECT_EQ(resolved.model->provider, key.provider_id);
  EXPECT_EQ(resolved.model->id, key.model_id);
}

class DeterministicDiscovery final : public ModelDiscoveryAdapter {
public:
  ProviderModelReport report;
  bool fail{false};
  std::atomic<int> calls{0};

  ProviderModelReport discover(const ProviderDiscoveryRequest &request,
                               std::stop_token stop_token) override {
    ++calls;
    if (stop_token.stop_requested())
      return {};
    if (fail)
      throw std::runtime_error("fake discovery failed");
    EXPECT_EQ(request.provider.id, report.provider_id);
    return report;
  }
};

static ModelCatalogEntry discovered_entry(std::string provider,
                                           std::string id,
                                           std::string name) {
  return {.key = {.provider_id = std::move(provider),
                   .model_id = std::move(id)},
          .display_name = std::move(name),
          .api = "fake-inference",
          .input_capabilities = {"text"},
          .context_window = 128000,
          .max_tokens = 4096};
}

static ProviderConfig fake_provider() {
  ProviderConfig config;
  config.id = "fake";
  config.api = "fake-inference";
  config.base_url = "http://fake.test/v1";
  config.auth = ProviderAuthPolicy::none;
  config.discovery_adapter = "deterministic";
  return config;
}

TEST(ModelCatalog, RefreshPublishesZeroOneAndManyReportedModels) {
  auto adapter = std::make_shared<DeterministicDiscovery>();
  adapter->report = {.provider_id = "fake",
                     .models = {discovered_entry("fake", "one", "One"),
                                discovered_entry("fake", "two", "Two"),
                                discovered_entry("fake", "three", "Three")},
                     .observed_at = std::chrono::system_clock::now()};
  auto adapters = std::make_shared<ModelDiscoveryAdapterCollection>();
  adapters->register_adapter("deterministic", adapter);
  ModelCatalog catalog({{"fake", fake_provider()}}, adapters);

  const auto succeeded = catalog.refresh();
  ASSERT_EQ(status_for_fake(succeeded).provider_id, "fake");
  EXPECT_EQ(status_for_fake(succeeded).state, ProviderRefreshState::succeeded);
  EXPECT_EQ(catalog.search("fake").size(), 3U);

  adapter->report.models.clear();
  catalog.refresh();
  EXPECT_TRUE(catalog.search("fake").empty());

  adapter->report.models.push_back(
      discovered_entry("fake", "one", "One"));
  catalog.refresh();
  EXPECT_EQ(catalog.search("fake").size(), 1U);
}

TEST(ModelCatalog, ConfiguredModelsBeatDiscoveryReports) {
  auto config = fake_provider();
  config.models.push_back(
      {.id = "same", .name = "Configured", .api = "fake-inference"});
  auto adapter = std::make_shared<DeterministicDiscovery>();
  adapter->report = {.provider_id = "fake",
                     .models = {discovered_entry("fake", "same", "Remote"),
                                discovered_entry("fake", "new", "New")},
                     .observed_at = std::chrono::system_clock::now()};
  auto adapters = std::make_shared<ModelDiscoveryAdapterCollection>();
  adapters->register_adapter("deterministic", adapter);
  ModelCatalog catalog({{"fake", config}}, adapters);

  catalog.refresh();
  const auto configured = catalog.entry({"fake", "same"});
  ASSERT_TRUE(configured.has_value());
  EXPECT_EQ(configured->display_name, "Configured");
  EXPECT_TRUE(catalog.entry({"fake", "new"}).has_value());
}

TEST(ModelCatalog, PartialFailureRetainsLastGoodProviderModels) {
  auto adapter = std::make_shared<DeterministicDiscovery>();
  adapter->report = {.provider_id = "fake",
                     .models = {discovered_entry("fake", "good", "Good")},
                     .observed_at = std::chrono::system_clock::now()};
  auto adapters = std::make_shared<ModelDiscoveryAdapterCollection>();
  adapters->register_adapter("deterministic", adapter);
  ModelCatalog catalog({{"fake", fake_provider()}}, adapters);

  catalog.refresh();
  const auto before = catalog.view();
  adapter->fail = true;
  const auto statuses = catalog.refresh();
  const auto after = catalog.view();

  ASSERT_EQ(status_for_fake(statuses).provider_id, "fake");
  ASSERT_EQ(status_for_fake(statuses).state, ProviderRefreshState::failed);
  EXPECT_TRUE(catalog.entry({"fake", "good"}).has_value());
  EXPECT_EQ(after.generation, before.generation + 1);
  EXPECT_EQ(after.entries, before.entries);
}

TEST(ModelCatalog, CancellationDoesNotPublishGeneration) {
  auto adapter = std::make_shared<DeterministicDiscovery>();
  adapter->report = {.provider_id = "fake",
                     .models = {discovered_entry("fake", "cancelled", "No")},
                     .observed_at = std::chrono::system_clock::now()};
  auto adapters = std::make_shared<ModelDiscoveryAdapterCollection>();
  adapters->register_adapter("deterministic", adapter);
  ModelCatalog catalog({{"fake", fake_provider()}}, adapters);
  const auto before = catalog.view();

  std::stop_source source;
  source.request_stop();
  catalog.refresh({}, source.get_token());

  const auto after = catalog.view();
  EXPECT_EQ(after, before);
  EXPECT_FALSE(catalog.entry({"fake", "cancelled"}).has_value());
}

TEST(ModelCatalog, UnknownDiscoveryBindingIsProviderQualified) {
  auto config = fake_provider();
  auto adapters = std::make_shared<ModelDiscoveryAdapterCollection>();
  EXPECT_THROW(ModelCatalog({{"fake", config}}, adapters), std::runtime_error);
  try {
    ModelCatalog catalog({{"fake", config}}, adapters);
  } catch (const std::runtime_error &error) {
    EXPECT_THAT(error.what(), testing::HasSubstr("provider fake"));
    EXPECT_THAT(error.what(), testing::HasSubstr("deterministic"));
  }
}

TEST(ModelCatalog, ProvidersWithoutDiscoveryRemainUsable) {
  auto config = fake_provider();
  config.discovery_adapter.reset();
  config.models.push_back(
      {.id = "local", .name = "Local", .api = "fake-inference"});
  auto adapters = std::make_shared<ModelDiscoveryAdapterCollection>();
  ModelCatalog catalog({{"fake", config}}, adapters);

  EXPECT_TRUE(catalog.entry({"fake", "local"}).has_value());
  const auto idle = catalog.refresh();
  ASSERT_EQ(status_for_fake(idle).provider_id, "fake");
  EXPECT_EQ(status_for_fake(idle).state, ProviderRefreshState::idle);
  EXPECT_TRUE(catalog.entry({"fake", "local"}).has_value());
}

TEST(ModelCatalog, ConcurrentReadersObserveCompleteViews) {
  auto adapter = std::make_shared<DeterministicDiscovery>();
  adapter->report = {.provider_id = "fake",
                     .models = {discovered_entry("fake", "one", "One")},
                     .observed_at = std::chrono::system_clock::now()};
  auto adapters = std::make_shared<ModelDiscoveryAdapterCollection>();
  adapters->register_adapter("deterministic", adapter);
  ModelCatalog catalog({{"fake", fake_provider()}}, adapters);

  std::atomic<bool> stop{false};
  std::atomic<bool> valid{true};
  std::vector<std::thread> readers;
  for (int index = 0; index < 4; ++index) {
    readers.emplace_back([&] {
      while (!stop.load()) {
        const auto view = catalog.view();
        std::set<ModelKey> keys;
        for (const auto &entry : view.entries)
          if (!keys.insert(entry.key).second)
            valid = false;
      }
    });
  }
  for (int index = 0; index < 20; ++index) {
    adapter->report.models.resize(index % 2 == 0 ? 1U : 2U);
    if (adapter->report.models.size() == 2U)
      adapter->report.models[1] = discovered_entry("fake", "two", "Two");
    catalog.refresh();
  }
  stop = true;
  for (auto &reader : readers)
    reader.join();
  EXPECT_TRUE(valid);
}
static const ProviderRefreshStatus &
status_for_fake(const std::vector<ProviderRefreshStatus> &statuses) {
  const auto it = std::ranges::find(statuses, "fake",
                                    &ProviderRefreshStatus::provider_id);
  EXPECT_NE(it, statuses.end());
  return *it;
}
