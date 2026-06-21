#pragma once

#include "core/session/session_tree.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pi::cli {

struct TreeSelectorResult {
  bool cancelled{false};
  std::string selected_session_id; // empty when cancelled
};

// Run the interactive full-screen session-tree selector.
// Returns the chosen session_id, or cancelled=true on Esc/q.
TreeSelectorResult run_tree_selector(
    const std::vector<core::SessionTreeLine> &lines,
    const std::string &current_session_id,
    std::size_t initial_cursor);

} // namespace pi::cli
