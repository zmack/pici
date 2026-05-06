#pragma once

#include <string>
#include <string_view>

namespace pi::core {

// Render Markdown to an ANSI-escaped string suitable for terminal output.
// Uses GFM dialect: tables, strikethrough, task lists.
std::string render_markdown_ansi(std::string_view input);

// Strip all Markdown formatting, return plain text.
std::string render_markdown_plain(std::string_view input);

} // namespace pi::core
