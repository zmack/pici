#pragma once
#include "core/message_types.h"
#include <functional>
#include <string>
#include <vector>

namespace pi::core {

std::vector<Message>
transform_messages(const std::vector<Message> &messages, const Model &model,
                   const std::function<std::string(const std::string &id)>
                       &normalize_tool_call_id = {});

} // namespace pi::core
