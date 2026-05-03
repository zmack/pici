#pragma once
#include <functional>
#include <string>
#include <vector>
#include "core/message_types.h"

namespace pi::core {

std::vector<Message> transform_messages(
    const std::vector<Message>& messages,
    const Model& model,
    std::function<std::string(const std::string& id)> normalize_tool_call_id = {});

} // namespace pi::core
