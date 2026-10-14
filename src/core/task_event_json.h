#pragma once

#include "core/agent_task.h"

#include <nlohmann/json.hpp>

namespace pi::core {

// Wire serialization of a TaskTree event, shared by the ACP event hub and the
// RPC `subscribe_tasks` stream so both frontends emit identical shapes.
nlohmann::json task_event_to_json(const AgentTaskEvent &event);

} // namespace pi::core
