#pragma once

#include "core/message_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pi::core {

struct SessionHeader {
  std::string id;
  std::optional<std::string> parent_id;
  std::optional<std::size_t> parent_offset;
  std::optional<std::string> name;
  std::int64_t created{0};
  std::string model;
  std::string provider;
  std::optional<std::string> sandbox_mode;
};

struct SessionRecord {
  SessionHeader header;
  std::vector<Message> messages;
};

} // namespace pi::core
