#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pi::cli {

// Called with the current buffer when Tab is pressed.
// Returns candidate completion strings.
using CompleteFn = std::function<std::vector<std::string>(std::string_view)>;

// Read one line from stdin with optional tab completion.
//
// - When stdin is a TTY, enters raw mode so Tab is intercepted before the
//   terminal driver sees it.  Single match: replace buffer + trailing space.
//   Multiple matches: complete to common prefix; if already there, print list.
//   No matches: bell.
// - When stdin is not a TTY (pipe/script), falls back to std::getline so
//   automated input works normally.
//
// Returns nullopt on EOF (Ctrl+D on empty input) or read error.
// Ctrl+C exits the process via SIGINT (same as before raw mode).
std::optional<std::string> readline(std::string_view prompt,
                                    CompleteFn complete_fn = {});

} // namespace pi::cli
