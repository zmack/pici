#pragma once

#include <string>
#include <string_view>

namespace pi::core {

// Returns ANSI-highlighted code for a supported language. Falls back to the
// original code when the language is unknown or parsing/highlighting fails.
std::string highlight_code_ansi(std::string_view code, std::string_view language);

// Whether the fenced language tag is currently supported by the highlighter.
bool supports_code_language(std::string_view language);

} // namespace pi::core
