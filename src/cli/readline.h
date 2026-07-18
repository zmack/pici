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

enum class ControlAction {
  scroll_line_up,
  scroll_line_down,
  scroll_page_up,
  scroll_page_down,
  scroll_top,
  scroll_bottom,
};

using ControlFn = std::function<void(ControlAction)>;

// Read one line from stdin with optional tab completion.
//
// - When stdin is a TTY, enters raw mode so Tab is intercepted before the
//   terminal driver sees it.  Single match: replace buffer + trailing space.
//   Multiple matches: complete to common prefix; if already there, print list.
//   No matches: bell.
// - When stdin is not a TTY (pipe/script), falls back to std::getline so
//   automated input works normally.
//
// Returns nullopt on EOF (Ctrl+D on empty input) or an interrupted read.
// Idle Ctrl+C therefore exits the interactive loop; active work consumes it
// through the agent interrupt path.
std::optional<std::string> readline(std::string_view prompt,
                                    const CompleteFn &complete_fn = {},
                                    const ControlFn &control_fn = {},
                                    std::string_view status_line = {});

} // namespace pi::cli
