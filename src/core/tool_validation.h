#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "core/message_types.h"

namespace pi::core {

// Validates tool call arguments against a JSON schema string.
// Applies TypeBox-style coercion (mirroring
// validation.ts::coerceWithJsonSchema) before structural validation. Mutates
// `arguments` in place. Thread-safe; compiled validators are cached by schema
// string.
class ToolValidator {
public:
  static std::optional<std::string> validate(std::string_view tool_name,
                                             std::string_view schema_json,
                                             ToolArguments &arguments);
};

// Convenience: pulls schema from tool->schema().serialize().
std::optional<std::string>
validate_tool_arguments(const ToolDefinition &tool, const ToolCall &tool_call,
                        ToolArguments &mutable_arguments);

} // namespace pi::core
