#pragma once

#include "core/message_types.h"

#include <optional>
#include <string_view>
#include <vector>

namespace pi::core {

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
