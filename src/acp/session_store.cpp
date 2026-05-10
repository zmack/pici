#include "acp/session_store.h"
#include "core/message_types.h"
#include <list>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace pi::acp {

std::vector<core::Message> SessionStore::load(const std::string &session_id) {
  std::scoped_lock lk(mutex_);
  auto it = index_.find(session_id);
  if (it == index_.end())
    return {};
  touch(it->second);
  return it->second->messages;
}

void SessionStore::save(const std::string &session_id,
                        std::vector<core::Message> messages) {
  std::scoped_lock lk(mutex_);
  auto it = index_.find(session_id);
  if (it != index_.end()) {
    it->second->messages = std::move(messages);
    touch(it->second);
    return;
  }
  // Evict LRU entry if at capacity
  if (lru_.size() >= kMaxSessions) {
    index_.erase(lru_.back().id);
    lru_.pop_back();
  }
  lru_.push_front({.id = session_id, .messages = std::move(messages)});
  index_[session_id] = lru_.begin();
}

void SessionStore::touch(std::list<Entry>::iterator it) {
  lru_.splice(lru_.begin(), lru_, it);
}

} // namespace pi::acp
