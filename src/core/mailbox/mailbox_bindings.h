#pragma once

#include "core/lua_tool.h"
#include "core/mailbox/mailbox_coordinator.h"
#include <nlohmann/json.hpp>

#include <functional>
#include <memory>

namespace pi::core {

LuaHooks::MailboxBindings
make_mailbox_bindings(std::weak_ptr<Mailbox> coordinator);

} // namespace pi::core
