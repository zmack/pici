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
// format. 2 = adds the "compaction" record (see SessionCompactionRecord).
inline constexpr int kSessionSchemaVersion = 2;

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
};

struct SessionRecord {
  SessionHeader header;
  std::vector<Message> messages;
};

} // namespace pi::core
