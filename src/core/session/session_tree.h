#pragma once

#include "core/session/session_record.h"
#include "core/session/session_store.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace pi::core {

struct SessionNode {
  SessionHeader header;
  std::size_t message_count{0};
  std::vector<SessionNode> children; // sorted oldest-first by header.created
};

struct SessionTreeLine {
  std::string text;       // rendered display string (may contain ANSI)
  std::string session_id; // which session this line represents
  int depth{0};
};

// Build display tree rooted at the oldest ancestor of current_session_id.
// Returns nullopt if current_session_id is not found in the store.
// Individual unreadable session files are silently skipped.
std::optional<SessionNode> build_session_tree(const SessionStore &store,
                                              const std::string &current_session_id);

std::vector<SessionTreeLine>
format_session_tree(const SessionNode &root,
                    const std::string &current_session_id);

} // namespace pi::core
