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
  void append_truncate(const std::string &session_id, std::size_t through);
  void set_name(const std::string &session_id, const std::string &name);
  void set_model(const std::string &session_id, std::string provider,
                 std::string model);
  void set_sandbox_mode(const std::string &session_id, std::string mode);

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

  // Both callers must hold mutex_.
  void write_line_locked(const std::string &session_id,
                         const nlohmann::json &j);
  std::ofstream &open_handle_locked(const std::string &session_id);
};

} // namespace pi::core
