#include "core/session/session_store.h"
#include "core/session/session_id.h"
#include "core/message_types.h"
#include "core/session/session_record.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace pi::core {

namespace {

SessionHeader parse_header_line(const nlohmann::json &j, // NOLINT(misc-include-cleaner)
                                const std::string &fallback_id) {
  SessionHeader hdr;
  hdr.id = j.value("id", fallback_id);
  hdr.created = j.value("created", std::int64_t{0});
  hdr.model = j.value("model", std::string{});
  hdr.provider = j.value("provider", std::string{});
  if (j.contains("parentId") && j["parentId"].is_string())
    hdr.parent_id = j["parentId"].get<std::string>();
  if (j.contains("parentOffset") && j["parentOffset"].is_number_unsigned())
    hdr.parent_offset = j["parentOffset"].get<std::size_t>();
  if (j.contains("name") && j["name"].is_string())
    hdr.name = j["name"].get<std::string>();
  return hdr;
}

// NOLINTNEXTLINE(misc-no-recursion)
SessionRecord load_recursive(const std::filesystem::path &base_dir,
                             const std::string &session_id,
                             std::set<std::string> &visited) {
  if (!visited.insert(session_id).second)
    throw std::runtime_error("session cycle detected at: " + session_id);

  auto path = base_dir / (session_id + ".jsonl");
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("session file not found: " + path.string());

  SessionHeader header;
  std::vector<Message> messages;
  bool first = true;
  std::string line;

  while (std::getline(f, line)) {
    if (line.empty())
      continue;
    nlohmann::json j;
    try {
      j = nlohmann::json::parse(line);
    } catch (...) {
      continue;
    }

    if (first) {
      first = false;
      if (j.value("type", std::string{}) != "session")
        throw std::runtime_error("invalid session file: first line type != "
                                 "session in " +
                                 path.string());
      header = parse_header_line(j, session_id);
      continue;
    }

    if (j.contains("role")) {
      if (auto msg = json::from_json(line))
        messages.push_back(std::move(*msg));
    } else if (j.value("type", std::string{}) == "meta" &&
               j.contains("name") && j["name"].is_string()) {
      header.name = j["name"].get<std::string>();
    }
  }

  if (first)
    throw std::runtime_error("empty or unreadable session file: " +
                             path.string());

  if (header.parent_id) {
    auto parent = load_recursive(base_dir, *header.parent_id, visited);
    std::size_t offset = header.parent_offset.value_or(parent.messages.size());
    if (offset > parent.messages.size())
      throw std::runtime_error(
          "corrupted fork: parent \"" + *header.parent_id +
          "\" has fewer messages than parentOffset=" + std::to_string(offset));

    std::vector<Message> full;
    full.reserve(offset + messages.size());
    full.insert(full.end(), parent.messages.begin(),
                std::next(parent.messages.begin(),
                          static_cast<std::ptrdiff_t>(offset)));
    full.insert(full.end(), messages.begin(), messages.end());
    messages = std::move(full);
  }

  return SessionRecord{.header = std::move(header),
                       .messages = std::move(messages)};
}

} // namespace

// Called once during CLI startup / SessionStore construction, before any
// worker threads exist, so these std::getenv() calls never race.
std::filesystem::path SessionStore::default_sessions_dir() {
  std::filesystem::path base;
  if (const char *xdg = std::getenv("XDG_DATA_HOME"); // NOLINT(concurrency-mt-unsafe)
      xdg != nullptr && *xdg != '\0') {
    base = xdg;
  } else if (const char *home = std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe)
             home != nullptr && *home != '\0') {
    base = std::filesystem::path(home) / ".local" / "share";
  } else {
    base = ".";
  }
  return base / "pici" / "sessions";
}

std::filesystem::path
SessionStore::session_path(const std::string &session_id) const {
  return base_dir_ / (session_id + ".jsonl");
}

SessionStore::SessionStore(std::filesystem::path base_dir) {
  base_dir_ = base_dir.empty() ? default_sessions_dir() : std::move(base_dir);
  std::error_code ec;
  std::filesystem::create_directories(base_dir_, ec);
}

std::ofstream &SessionStore::open_handle_locked(const std::string &session_id) {
  auto it = handles_.find(session_id);
  if (it != handles_.end())
    return it->second;

  auto path = session_path(session_id);
  auto &stream = handles_[session_id];
  stream.open(path, std::ios::app);
  if (!stream)
    throw std::runtime_error("failed to open session file: " + path.string());
  return stream;
}

void SessionStore::write_line_locked(const std::string &session_id,
                                     const nlohmann::json &j) {
  auto &stream = open_handle_locked(session_id);
  stream << j.dump() << '\n';
  stream.flush();
}

std::string SessionStore::create(const SessionHeader &hdr) {
  std::string sid = hdr.id;
  while (std::filesystem::exists(session_path(sid)))
    sid = generate_session_id();

  nlohmann::json j = nlohmann::json::object();
  j["type"] = "session";
  j["id"] = sid;
  j["parentId"] =
      hdr.parent_id ? nlohmann::json(*hdr.parent_id) : nlohmann::json(nullptr);
  j["parentOffset"] = hdr.parent_offset ? nlohmann::json(*hdr.parent_offset)
                                        : nlohmann::json(nullptr);
  j["name"] = hdr.name ? nlohmann::json(*hdr.name) : nlohmann::json(nullptr);
  j["created"] = hdr.created;
  j["model"] = hdr.model;
  j["provider"] = hdr.provider;

  std::scoped_lock lock(mutex_);
  write_line_locked(sid, j);
  return sid;
}

void SessionStore::append_message(const std::string &session_id,
                                  const Message &msg) {
  auto line = json::to_jsonl_line(msg);
  nlohmann::json j = nlohmann::json::parse(line);
  std::scoped_lock lock(mutex_);
  write_line_locked(session_id, j);
}

void SessionStore::set_name(const std::string &session_id,
                            const std::string &name) {
  auto now =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  nlohmann::json j = nlohmann::json::object();
  j["type"] = "meta";
  j["timestamp"] = static_cast<std::int64_t>(now);
  j["name"] = name;

  std::scoped_lock lock(mutex_);
  write_line_locked(session_id, j);
}

std::optional<SessionRecord>
SessionStore::load(const std::string &session_id) const {
  std::error_code ec;
  if (!std::filesystem::exists(session_path(session_id), ec))
    return std::nullopt;
  std::set<std::string> visited;
  return load_recursive(base_dir_, session_id, visited);
}

std::optional<std::string> SessionStore::latest_session_id() const {
  std::filesystem::file_time_type latest_time;
  std::string latest_id;
  bool found = false;

  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(base_dir_, ec)) {
    if (ec)
      break;
    if (entry.path().extension() != ".jsonl")
      continue;
    auto mtime = entry.last_write_time();
    if (!found || mtime > latest_time) {
      latest_time = mtime;
      latest_id = entry.path().stem().string();
      found = true;
    }
  }

  return found ? std::optional<std::string>(std::move(latest_id))
               : std::nullopt;
}

std::vector<SessionHeader> SessionStore::list() const {
  using Entry = std::pair<std::filesystem::file_time_type, SessionHeader>;
  std::vector<Entry> entries;

  std::error_code ec;
  for (const auto &dir_entry :
       std::filesystem::directory_iterator(base_dir_, ec)) {
    if (ec)
      break;
    if (dir_entry.path().extension() != ".jsonl")
      continue;

    try {
      std::ifstream f(dir_entry.path());
      if (!f)
        continue;
      std::string line;
      if (!std::getline(f, line) || line.empty())
        continue;

      auto j = nlohmann::json::parse(line);
      if (j.value("type", std::string{}) != "session")
        continue;

      auto stem = dir_entry.path().stem().string();
      entries.emplace_back(dir_entry.last_write_time(),
                           parse_header_line(j, stem));
    } catch (...) {
      continue;
    }
  }

  std::ranges::sort(entries, [](const Entry &a, const Entry &b) {
    return a.first > b.first;
  });

  std::vector<SessionHeader> result;
  result.reserve(entries.size());
  for (auto &[time, hdr] : entries)
    result.push_back(std::move(hdr));
  return result;
}

std::vector<SessionHeader>
SessionStore::find_by_prefix(std::string_view prefix) const {
  auto all = list();
  std::vector<SessionHeader> result;
  for (auto &hdr : all)
    if (std::string_view(hdr.id).starts_with(prefix))
      result.push_back(std::move(hdr));
  return result;
}

} // namespace pi::core
