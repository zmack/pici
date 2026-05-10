#pragma once

#include "core/message_types.h"

#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pi::acp {

// Thread-safe in-memory store mapping session_id → message history.
// Caps at kMaxSessions entries using LRU eviction.
class SessionStore {
public:
  static constexpr std::size_t kMaxSessions = 100;

  // Get existing session messages, or return empty vector for a new session.
  std::vector<core::Message> load(const std::string &session_id);

  // Persist updated messages back to a session.
  void save(const std::string &session_id, std::vector<core::Message> messages);

private:
  struct Entry {
    std::string id;
    std::vector<core::Message> messages;
  };

  std::list<Entry> lru_;
  std::unordered_map<std::string, std::list<Entry>::iterator> index_;
  std::mutex mutex_;

  // Move entry to front of LRU list (must hold mutex_)
  void touch(std::list<Entry>::iterator it);
};

} // namespace pi::acp
