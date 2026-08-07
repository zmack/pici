#pragma once

#include "core/message_types.h"

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

struct ProviderDefinition {
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
class ModelRegistry {
public:
  explicit ModelRegistry(
      const std::map<std::string, ProviderConfig> &configured = {});

  const std::vector<Model> &models() const { return models_; }
  const ProviderDefinition *provider(std::string_view id) const;
  const std::map<std::string, ProviderDefinition> &providers() const {
    return providers_;
  }
  const Model *exact(std::string_view provider,
                     std::string_view model_id) const;
  ModelResolution resolve(const ModelSelection &selection) const;
  std::vector<const Model *> search(std::string_view filter) const;

  // Call after all LLM clients have registered. Throws one actionable error
  // for the first effective model whose API ID is not registered.
  void validate_registered_apis() const;

  static std::vector<ProviderDefinition> builtin_providers();

private:
  std::vector<Model> models_;
  std::map<std::string, ProviderDefinition> providers_;
  std::map<std::pair<std::string, std::string>, std::size_t> indexes_;

  void add_or_replace(Model model);
};

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
