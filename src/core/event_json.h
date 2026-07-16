#pragma once

#include "core/event_types.h"

#include <nlohmann/json.hpp>

namespace pi::core {

// Canonical machine-facing representation of an agent event. Frontends
// (JSONL RPC, ACP, and future addon bridges) should consume this instead of
// maintaining their own event serializers.
nlohmann::json event_to_json(const AgentEvent &event);

} // namespace pi::core
