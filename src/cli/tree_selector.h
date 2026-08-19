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
//
// `caller_owns_alt_screen` must be true when the active renderer already
// holds a persistent alternate-screen session (e.g. --render region). In
// that case the selector draws directly into the existing screen instead of
// nesting its own `\033[?1049h`/`\033[?1049l` pair — nesting toggles the
// terminal back to the primary buffer the alt-screen renderer never wrote
// to, corrupting the whole display. The caller must repaint (e.g. via
// Renderer::force_full_repaint()) once this returns.
TreeSelectorResult
run_tree_selector(const std::vector<core::SessionTreeLine> &lines,
                  const std::string &current_session_id,
                  std::size_t initial_cursor, bool caller_owns_alt_screen);

} // namespace pi::cli
