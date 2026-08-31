#pragma once

#include "acp/server.h"
#include "acp/task_events.h"
#include "acp/types.h"
#include "core/session/session_store.h"

#include <httplib.h>
#include <memory>

namespace pi::acp {

// Register all ACP routes on the server.
void register_routes(httplib::Server &svr, const ServerConfig &cfg,
                     const std::shared_ptr<core::SessionStore> &sessions,
                     const std::shared_ptr<core::TaskTree> &tasks,
                     const std::shared_ptr<TaskEventHub> &task_events);

} // namespace pi::acp
