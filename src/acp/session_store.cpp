#include "acp/session_store.h"

namespace pi::acp {

std::vector<core::Message> SessionStore::load(const std::string &session_id) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto it = index_.find(session_id);
  if (it == index_.end()) return {};
  touch(it->second);
  return it->second->messages;
}

void SessionStore::save(const std::string &session_id,
                        std::vector<core::Message> messages) {
  std::lock_guard<std::mutex> lk(mutex_);
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
  lru_.push_front({session_id, std::move(messages)});
  index_[session_id] = lru_.begin();
}

void SessionStore::touch(std::list<Entry>::iterator it) {
  lru_.splice(lru_.begin(), lru_, it);
}

} // namespace pi::acp
