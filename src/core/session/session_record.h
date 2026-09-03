#pragma once

#include "core/message_types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pi::core {

// Bumped whenever a new JSONL record type is introduced that an older
// loader would silently misinterpret if it fell through unrecognized
// records instead of failing loudly. 1 = the original message/truncate/meta
// format. 2 = adds the "compaction" record (see SessionCompactionRecord). 3 =
// SessionStore writes real, collision-checked "id"/"parentId" entry-tree
// pointers on every message/compaction record, and adds non-destructive
// rewind: SessionCompactionRecord's "kind" discriminator lets a
// "compaction"-typed record be either a real LLM-driven summarization
// ("summary", the default) or a rewind ("branch") that leaves the abandoned
// entries on disk and simply moves the leaf. Rewind is deliberately *not* a
// new record type -- it reuses "compaction"'s existing minVersion gate,
// since an old binary that doesn't parse a novel "type" at all could never
// be taught to respect a version field attached to it.
inline constexpr int kSessionSchemaVersion = 3;

struct SessionHeader {
  std::string id;
  std::optional<std::string> parent_id;
  std::optional<std::size_t> parent_offset;
  std::optional<std::string> name;
  std::int64_t created{0};
  std::string model;
  std::string provider;
  std::optional<std::string> sandbox_mode;
  // Highest schema version required to correctly replay this session's
  // records, accumulated from any "compaction" records seen. Absent (or 1)
  // means the session predates compaction and is fully compatible with
  // every pici build.
  int min_schema_version{1};
};

// A replayable "compaction" journal record: replaces the current in-memory
// message vector wholesale with `messages`. Carries no offset — forks and
// prior truncation already make a byte/message offset into a prior record
// ambiguous, so a compaction record always stands on its own as a complete
// replacement transcript.
struct SessionCompactionRecord {
  std::vector<Message> messages;
  std::string provider;
  std::string model;
  std::string summary; // e.g. "server" (remote) or "local" (fallback)
  std::int64_t timestamp{0};
  // Discriminates a real LLM-driven summarization ("summary", the default)
  // from a non-destructive rewind ("branch") -- both replay identically
  // (wholesale-replace), so this is purely informational for pici's own
  // code (and a future /tree UI) to tell them apart. A record with no
  // "kind" key (every compaction record written before this field existed)
  // parses as "summary", which is correct: that's what they all were.
  std::string kind{"summary"};
};

struct SessionRecord {
  SessionHeader header;
  std::vector<Message> messages;
  // Parallel to `messages`: the id of the entry-tree node that contributed
  // each message. A message contributed by a compaction (or, in the future,
  // a branch) record shares that record's single id with every other
  // message it produced wholesale. Populated by the chain-based loader in
  // session_store.cpp; see `index_entries`/`walk_from_leaf` there.
  std::vector<std::string> entry_ids;
  // The id of the entry-tree node the replay walk started from (the
  // session's current leaf), or nullopt if the session (and, when forked,
  // its inherited parent prefix) has no entries at all.
  std::optional<std::string> leaf_entry_id;
};

} // namespace pi::core
