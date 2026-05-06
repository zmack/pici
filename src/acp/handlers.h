#pragma once

#include "acp/server.h"
#include "acp/session_store.h"
#include "acp/types.h"

#include <httplib.h>
#include <memory>

namespace pi::acp {

// Register all ACP routes on the server.
void register_routes(httplib::Server &svr,
                     const ServerConfig &cfg,
                     std::shared_ptr<SessionStore> sessions);

} // namespace pi::acp
