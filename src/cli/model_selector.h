#pragma once

#include "core/models.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pi::cli {

struct ModelSelectorResult {
  bool cancelled{true};
  std::optional<core::Model> model;
};

using ModelAvailability = std::function<std::string(const core::Model &)>;

// `caller_owns_alt_screen` must be true when the active renderer already
// holds a persistent alternate-screen session (e.g. --render region); see
// tree_selector.h for why nesting a second `1049h`/`1049l` pair corrupts the
// display in that case. The caller must repaint (e.g. via
// Renderer::force_full_repaint()) once this returns.
ModelSelectorResult
run_model_selector(const std::vector<const core::Model *> &models,
                   std::string_view current_provider,
                   std::string_view current_model,
                   const ModelAvailability &availability = {},
                   bool caller_owns_alt_screen = false);

} // namespace pi::cli
