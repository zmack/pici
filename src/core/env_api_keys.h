#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace pi::core {

std::optional<std::string> get_env_api_key(std::string_view provider);

} // namespace pi::core
