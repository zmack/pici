#pragma once

#include "acp/server.h"
#include "acp/types.h"
#include "core/session/session_store.h"

#include <httplib.h>
#include <memory>

namespace pi::acp {

// Register all ACP routes on the server.
void register_routes(httplib::Server &svr, const ServerConfig &cfg,
                     const std::shared_ptr<core::SessionStore> &sessions);

} // namespace pi::acp
