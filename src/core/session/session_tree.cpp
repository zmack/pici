#include "core/session/session_tree.h"

#include "core/session/session_record.h"
#include "core/session/session_store.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pi::core {

namespace {

// Walk the parent chain from start_id, returning all ancestor IDs in order
// (start_id first, root last). Returns empty if start_id is not in headers.
std::vector<std::string>
ancestor_chain(const std::unordered_map<std::string, SessionHeader> &headers,
               const std::string &start_id) {
  std::vector<std::string> chain;
  std::set<std::string> seen;
  std::string cur = start_id;
  while (true) {
    if (!seen.insert(cur).second)
      break; // cycle guard
    auto it = headers.find(cur);
    if (it == headers.end())
      break;
    chain.push_back(cur);
    if (!it->second.parent_id)
      break;
    cur = *it->second.parent_id;
  }
  return chain;
}

// Count messages in a loaded record (the materialized full list).
std::size_t count_messages(const SessionStore &store, const std::string &id) {
  auto rec = store.load(id);
  if (!rec)
    return 0;
  return rec->messages.size();
}

// Recursively build a SessionNode from the parent→children map.
// NOLINTNEXTLINE(misc-no-recursion)
SessionNode
build_node(const std::unordered_map<std::string, SessionHeader> &headers,
           const std::map<std::string, std::vector<std::string>> &children_map,
           const SessionStore &store, const std::string &id) {
  SessionNode node;
  node.header = headers.at(id);
  node.message_count = count_messages(store, id);
  auto it = children_map.find(id);
  if (it != children_map.end()) {
    for (const auto &child_id : it->second) {
      node.children.push_back(
          build_node(headers, children_map, store, child_id));
    }
  }
  return node;
}

// Recursively format tree lines with ASCII connectors.
// NOLINTNEXTLINE(misc-no-recursion)
void format_node(const SessionNode &node, const std::string &current_session_id,
                 const std::string &prefix, bool is_last, bool is_root,
                 std::vector<SessionTreeLine> &out) {
  std::string connector;
  std::string child_prefix;
  if (is_root) {
    connector = "";
    child_prefix = "   ";
  } else if (is_last) {
    connector = "└─ "; // └─
    child_prefix = "   ";
  } else {
    connector = "├─ ";    // ├─
    child_prefix = "│  "; // │
  }

  const bool is_current = (node.header.id == current_session_id);
  const std::string active_marker = is_current ? "●  " : "   "; // ●

  // Timestamp: "Mon DD HH:MM"
  std::string ts;
  {
    auto t = static_cast<std::time_t>(node.header.created);
    std::tm tm_local{};
    localtime_r(&t, &tm_local);
    std::ostringstream oss;
    oss << std::put_time(&tm_local, "%b %d %H:%M");
    ts = oss.str();
  }

  // ID display (bold for current session)
  std::string id_str;
  if (is_current)
    id_str = "\033[1m" + node.header.id + "\033[0m";
  else
    id_str = node.header.id;

  // Name field
  std::string name_str;
  if (node.header.name)
    name_str = "  \"" + *node.header.name + "\"";

  std::string line_text = prefix + active_marker + connector + id_str +
                          name_str + "  " + std::to_string(node.message_count) +
                          " msgs   " + ts;

  SessionTreeLine tl;
  tl.text = std::move(line_text);
  tl.session_id = node.header.id;
  tl.depth =
      static_cast<int>(std::count(prefix.begin(), prefix.end(), ' ') / 3);
  out.push_back(std::move(tl));

  const std::string next_prefix = prefix + child_prefix;
  for (std::size_t i = 0; i < node.children.size(); ++i) {
    const bool last_child = (i + 1 == node.children.size());
    format_node(node.children[i], current_session_id, next_prefix, last_child,
                false, out);
  }
}

} // namespace

std::optional<SessionNode>
build_session_tree(const SessionStore &store,
                   const std::string &current_session_id) {
  // Load all headers
  auto all_headers_vec = store.list();
  if (all_headers_vec.empty())
    return std::nullopt;

  std::unordered_map<std::string, SessionHeader> headers;
  for (auto &hdr : all_headers_vec)
    headers[hdr.id] = std::move(hdr);

  if (!headers.contains(current_session_id))
    return std::nullopt;

  // Find the full ancestor chain of the current session
  auto ancestors = ancestor_chain(headers, current_session_id);
  if (ancestors.empty())
    return std::nullopt;

  // Collect the family: ancestors + any session whose ancestor chain
  // includes any ancestor of the current session.
  std::set<std::string> family_ids(ancestors.begin(), ancestors.end());
  const std::string root_id = ancestors.back();

  for (const auto &[id, hdr] : headers) {
    if (family_ids.contains(id))
      continue;
    // Walk this session's parents; if we hit any family member, include it.
    auto chain = ancestor_chain(headers, id);
    for (const auto &aid : chain) {
      if (family_ids.contains(aid)) {
        for (const auto &cid : chain)
          family_ids.insert(cid);
        break;
      }
    }
  }

  // Build parent→children map (within the family)
  std::map<std::string, std::vector<std::string>> children_map;
  for (const auto &id : family_ids) {
    auto it = headers.find(id);
    if (it == headers.end())
      continue;
    const auto &hdr = it->second;
    if (hdr.parent_id && family_ids.contains(*hdr.parent_id))
      children_map[*hdr.parent_id].push_back(id);
    else if (!hdr.parent_id)
      children_map["__root__"].push_back(
          id); // shouldn't happen if root_id found
  }

  // Sort children oldest-first
  for (auto &[parent, kids] : children_map) {
    std::ranges::sort(
        kids, [&headers](const std::string &a, const std::string &b) {
          const auto ia = headers.find(a);
          const auto ib = headers.find(b);
          const std::int64_t ca = ia != headers.end() ? ia->second.created : 0;
          const std::int64_t cb = ib != headers.end() ? ib->second.created : 0;
          return ca < cb;
        });
  }

  return build_node(headers, children_map, store, root_id);
}

std::vector<SessionTreeLine>
format_session_tree(const SessionNode &root,
                    const std::string &current_session_id) {
  std::vector<SessionTreeLine> lines;
  format_node(root, current_session_id, "", true, true, lines);
  return lines;
}

} // namespace pi::core
