#pragma once

#include "core/message_types.h"
#include "core/session/session_record.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pi::core {

class SessionStore {
public:
  explicit SessionStore(std::filesystem::path base_dir = {});

  ~SessionStore() = default;
  SessionStore(const SessionStore &) = delete;
  SessionStore &operator=(const SessionStore &) = delete;
  SessionStore(SessionStore &&) = delete;
  SessionStore &operator=(SessionStore &&) = delete;

  std::string create(const SessionHeader &hdr);
  void append_message(const std::string &session_id, const Message &msg);
  void set_name(const std::string &session_id, const std::string &name);
  void set_model(const std::string &session_id, std::string provider,
                 std::string model);
  void set_sandbox_mode(const std::string &session_id, std::string mode);

  // Appends a replayable full-transcript-replacement record. Throws if
  // `session_id` has children on disk (a session with live forks must be
  // forked-then-compacted rather than compacted directly — compacting it
  // in place would silently invalidate every child's parentOffset).
  void append_compaction(const std::string &session_id,
                         const SessionCompactionRecord &record);

  // Non-destructive rewind. If `target_entry_id` is set, it must resolve
  // against this session's current on-disk entry tree (throws
  // std::runtime_error, "branch target not found", otherwise -- a
  // caller-supplied id is never trusted blindly); if nullopt, the target is
  // the tree's root (empty history). Appends a "compaction"-shaped record
  // (kind == "branch") whose messages are the target's flat walk, so the
  // abandoned entries between the old leaf and the target remain on disk
  // forever, replayable, just no longer part of the live chain -- then
  // advances the cached leaf to this new record. Returns the flat message
  // vector it just computed, so callers (SessionRuntime::
  // truncate_active_session) can install the exact same vector in memory
  // that was just persisted, rather than deriving a second, possibly
  // divergent one.
  std::vector<Message> branch(const std::string &session_id,
                              std::optional<std::string> target_entry_id);

  // True if any session file in this store has `parentId == session_id`.
  bool has_children(const std::string &session_id) const;

  // Tri-state description of `session_id`'s CURRENT leaf, for a caller
  // about to fork FROM it (see ParentEntryAnchor in session_record.h).
  // Read-only: builds a fresh index of `session_id` for this one call and
  // does NOT touch the write-path `leaf_ids_` cache. Never returns `none`
  // -- `none` is purely a header-parsing/backward-compat state for OLD
  // session files that predate this field; a live session being forked
  // FROM always has a concrete answer: `root` (no entries at all yet -- so
  // a child forking from it correctly anchors "before any entries", not
  // "no anchor recorded") or `entry` (the id of its current leaf).
  struct LeafAnchor {
    ParentEntryAnchor anchor{ParentEntryAnchor::root};
    std::optional<std::string> entry_id;
  };
  LeafAnchor current_leaf_anchor(const std::string &session_id) const;

  std::optional<SessionRecord> load(const std::string &session_id) const;
  std::optional<std::string> latest_session_id() const;
  std::vector<SessionHeader> list() const;
  std::vector<SessionHeader> find_by_prefix(std::string_view prefix) const;

  static std::filesystem::path default_sessions_dir();
  std::filesystem::path session_path(const std::string &session_id) const;

private:
  std::filesystem::path base_dir_;
  mutable std::mutex mutex_;
  std::unordered_map<std::string, std::ofstream> handles_;
  // This process's record of each touched session's current leaf entry id
  // (nullopt == root / no entries yet), set after every successful write.
  // NOT consulted for correctness during minting -- mint_entry_locked
  // always re-derives the leaf from a fresh on-disk index (it's already
  // rebuilding one for the id-collision check), so this cache staying
  // correct is a side effect of that, not a shortcut this class relies on.
  // Kept as a member for future callers that want a leaf without a disk
  // read; see mint_entry_locked's own comment in session_store.cpp for why
  // that shortcut isn't taken internally.
  std::unordered_map<std::string, std::optional<std::string>> leaf_ids_;

  // Both callers must hold mutex_.
  void write_line_locked(const std::string &session_id,
                         const nlohmann::json &j);
  std::ofstream &open_handle_locked(const std::string &session_id);
};

} // namespace pi::core
