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

ModelSelectorResult
run_model_selector(const std::vector<const core::Model *> &models,
                   std::string_view current_provider,
                   std::string_view current_model,
                   const ModelAvailability &availability = {});

} // namespace pi::cli
