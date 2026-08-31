#pragma once

#include "core/message_types.h"

#include <chrono>
#include <compare>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pi::core {

enum class ProviderAuthPolicy { required, optional, none, oauth };

struct ApiKeyConfig {
  std::optional<std::string> literal;
  std::optional<std::string> env_var;
};

struct ConfiguredCost {
  std::optional<double> input_per_mtok;
  std::optional<double> output_per_mtok;
  std::optional<double> cache_read_per_mtok;
  std::optional<double> cache_write_per_mtok;
};

struct ConfiguredModel {
  std::string id;
  std::optional<std::string> name;
  std::optional<std::string> api;
  std::optional<std::string> base_url;
  std::optional<bool> reasoning;
  std::optional<std::vector<std::string>> input_capabilities;
  std::optional<std::uint64_t> context_window;
  std::optional<std::uint64_t> max_tokens;
  std::map<std::string, std::string> headers;
  ConfiguredCost cost;
  std::optional<std::map<std::string, std::optional<std::string>>>
      thinking_level_map;
};

struct ProviderConfig {
  std::string id;
  std::optional<std::string> api;
  std::optional<std::string> base_url;
  ApiKeyConfig api_key;
  std::optional<ProviderAuthPolicy> auth;
  std::map<std::string, std::string> headers;
  std::vector<ConfiguredModel> models;
  std::map<std::string, ConfiguredModel> model_overrides;
};

// A stable domain identity for one provider/model pair. Model IDs are opaque
// strings: they may contain slashes (for example, OpenRouter model IDs), so
// callers must never reconstruct this value by splitting a display string.
struct ModelKey {
  std::string provider_id;
  std::string model_id;

  bool operator==(const ModelKey &) const = default;
  auto operator<=>(const ModelKey &) const = default;
};

// The immutable, provider-neutral model data that a catalog can publish to
// UI and protocol adapters. It deliberately contains no Model pointer,
// terminal state, JSON, or Lua value.
struct ModelCatalogEntry {
  ModelKey key;
  std::string display_name;
  std::string api;
  bool reasoning{false};
  std::vector<std::string> input_capabilities;
  std::uint64_t context_window{0};
  std::uint64_t max_tokens{0};

  struct Cost {
    double input_per_mtok{0};
    double output_per_mtok{0};
    double cache_read_per_mtok{0};
    double cache_write_per_mtok{0};

    bool operator==(const Cost &) const = default;
  } cost{};

  bool operator==(const ModelCatalogEntry &) const = default;
};

enum class ProviderRefreshState {
  idle,
  refreshing,
  succeeded,
  failed,
};

// Provider-scoped refresh state. A failed refresh retains the prior catalog
// view; diagnostic is intended to tell an operator how to recover.
struct ProviderRefreshStatus {
  std::string provider_id;
  ProviderRefreshState state{ProviderRefreshState::idle};
  std::string diagnostic;

  bool operator==(const ProviderRefreshStatus &) const = default;
};

// A discovery adapter reports values, never mutable catalog objects. The
// timestamp and source revision let the catalog expose freshness without
// leaking a provider wire DTO into the domain.
struct ProviderModelReport {
  std::string provider_id;
  std::vector<ModelCatalogEntry> models;
  std::chrono::system_clock::time_point observed_at;
  std::string source_revision;

  bool operator==(const ProviderModelReport &) const = default;
};

struct ModelCatalogView {
  std::uint64_t generation{0};
  std::vector<ModelCatalogEntry> entries;
  std::vector<ProviderRefreshStatus> refresh_status;

  bool operator==(const ModelCatalogView &) const = default;
};

// A request crossing the model-selection boundary. Overrides are deliberately
// request values rather than a mutable Model, so native and future Lua
// pickers can share this contract.
struct ModelSelectionRequest {
  ModelKey key;
  std::optional<std::string> base_url;
  std::optional<ThinkingLevel> thinking_level;
  std::string source;
};
struct Provider {
  std::string id;
  std::string api;
  std::string base_url;
  ProviderAuthPolicy auth{ProviderAuthPolicy::required};
  ApiKeyConfig api_key;
  std::map<std::string, std::string> headers;
};

struct ThinkingLevelResolution {
  ThinkingLevel level{ThinkingLevel::off};
  std::optional<std::string> warning;
};

ThinkingLevelResolution resolve_thinking_level(const Model &model,
                                               ThinkingLevel requested);

struct ModelSelection {
  std::optional<std::string> provider;
  std::string model;
  std::optional<std::string> base_url;
  std::string source;
};

struct ModelResolution {
  std::optional<Model> model;
  std::string error;

  explicit operator bool() const { return model.has_value(); }
};

// The immutable, effective provider/model catalog used by every runtime
// surface. Models are owned by this object so search results remain stable.
class ModelCatalog {
public:
  explicit ModelCatalog(
      const std::map<std::string, ProviderConfig> &configured = {});

  const std::vector<Model> &models() const { return models_; }
  // Returns a complete immutable projection for one catalog generation.
  // Copies are intentional: consumers never borrow catalog-owned Model data.
  ModelCatalogView view() const { return view_; }
  const Provider *provider(std::string_view id) const;
  const std::map<std::string, Provider> &providers() const {
    return providers_;
  }
  const Model *exact(std::string_view provider,
                     std::string_view model_id) const;
  std::optional<ModelCatalogEntry> entry(const ModelKey &key) const;
  ModelResolution resolve(const ModelKey &key,
                          std::optional<std::string> base_url = {},
                          std::string source = "selection") const;
  ModelResolution resolve(const ModelSelection &selection) const;
  std::vector<ModelCatalogEntry> search(std::string_view filter) const;

  // TODO(taxonomy-phase-10): remove. Pointer-returning search is a
  // migration seam for the pre-catalog picker and will be replaced in Phase
  // 9 by a value-based picker boundary.
  std::vector<const Model *> search_models(std::string_view filter) const;

  // Call after all LLM clients have registered. Throws one actionable error
  // for the first effective model whose API ID is not registered.
  void validate_registered_apis() const;

  static std::vector<Provider> builtin_providers();

private:
  std::vector<Model> models_;
  std::map<std::string, Provider> providers_;
  std::map<std::pair<std::string, std::string>, std::size_t> indexes_;
  ModelCatalogView view_;

  void add_or_replace(Model model);
};

// TODO(taxonomy-phase-10): remove. This alias keeps downstream integrations
// source-compatible while all repository code uses the target aggregate name.
using ModelRegistry = ModelCatalog;

// TODO(taxonomy-phase-10): remove. ProviderDefinition was the migration-era
// name for the catalog-owned Provider value.
using ProviderDefinition = Provider;

// All models in the built-in registry.
const std::vector<Model> &all_models();

// Find a model by ID or "provider/id" spec.
// provider_hint is used when the spec has no "/" prefix.
std::optional<Model> find_model(std::string_view spec,
                                std::string_view provider_hint = "");

// Return models whose id or provider contains filter (case-insensitive).
// Empty filter returns all models.
std::vector<const Model *> search_models(std::string_view filter);

} // namespace pi::core
