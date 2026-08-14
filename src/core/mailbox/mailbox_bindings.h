#pragma once

#include "core/lua_tool.h"
#include "core/mailbox/mailbox_coordinator.h"
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>

namespace pi::core {

using MailboxActorProvider =
    std::function<std::optional<AgentRuntimeIdentity>()>;

LuaHooks::MailboxBindings
make_mailbox_bindings(std::weak_ptr<MailboxCoordinator> coordinator,
                      const MailboxActorProvider &actor_provider = {});

} // namespace pi::core
