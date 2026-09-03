#include "core/session/session_store.h"
#include "core/message_types.h"
#include "core/session/session_id.h"
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
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace pi::core {

namespace {

SessionHeader
parse_header_line(const nlohmann::json &j, // NOLINT(misc-include-cleaner)
                  const std::string &fallback_id) {
  SessionHeader hdr;
  hdr.id = j.value("id", fallback_id);
  hdr.created = j.value("created", std::int64_t{0});
  hdr.model = j.value("model", std::string{});
  hdr.provider = j.value("provider", std::string{});
  if (j.contains("sandboxMode") && j["sandboxMode"].is_string())
    hdr.sandbox_mode = j["sandboxMode"].get<std::string>();
  if (j.contains("parentId") && j["parentId"].is_string())
    hdr.parent_id = j["parentId"].get<std::string>();
  if (j.contains("parentOffset") && j["parentOffset"].is_number_unsigned())
    hdr.parent_offset = j["parentOffset"].get<std::size_t>();
  if (j.contains("name") && j["name"].is_string())
    hdr.name = j["name"].get<std::string>();
  return hdr;
}

SessionCompactionRecord
parse_compaction_record(const nlohmann::json &j,
                        const std::filesystem::path &path) {
  if (!j.contains("messages") || !j["messages"].is_array())
    throw std::runtime_error(
        "malformed compaction record (missing \"messages\" array) in " +
        path.string());
  if (!j.contains("minVersion") || !j["minVersion"].is_number_integer())
    throw std::runtime_error(
        "malformed compaction record (missing \"minVersion\") in " +
        path.string());
  const auto required_version = j["minVersion"].get<int>();
  if (required_version > kSessionSchemaVersion)
    throw std::runtime_error(
        "session \"" + path.string() + "\" requires schema version " +
        std::to_string(required_version) + "; this pici build supports up to " +
        std::to_string(kSessionSchemaVersion) +
        " — upgrade pici to open this session");

  SessionCompactionRecord record;
  record.provider = j.value("provider", std::string{});
  record.model = j.value("model", std::string{});
  record.summary = j.value("summary", std::string{});
  record.timestamp = j.value("timestamp", std::int64_t{0});
  record.messages.reserve(j["messages"].size());
  for (const auto &raw : j["messages"]) {
    auto msg = json::from_json(raw.dump());
    if (!msg)
      throw std::runtime_error(
          "malformed compaction record (unparseable message) in " +
          path.string());
    record.messages.push_back(std::move(*msg));
  }
  return record;
}

// One indexed entry-tree node: a message, a legacy truncate (the "through"
// count it carries), or a compaction record. Meta and unparseable/empty
// lines never become nodes (see `index_entries`).
struct SessionEntryNode {
  std::string id;
  std::optional<std::string> parent_id;
  std::variant<Message, std::size_t, SessionCompactionRecord> payload;
};

struct SessionIndex {
  SessionHeader header;
  // File order.
  std::vector<SessionEntryNode> nodes;
  std::unordered_map<std::string, std::size_t> id_to_index;
};

// Single forward pass over a session file, building an id + parent-pointer
// tree out of every message/legacy-truncate/compaction line (the same
// line-skipping/parse-failure tolerance as the pre-chain loader).
//
// Legacy id scheme: a line with no "id" key gets the permanent, on-disk-
// referenceable id "legacy-<1-based physical line number>". The physical
// line number is assigned before parsing decides whether the line is a
// message/truncate/meta/garbage line — i.e. it counts every `getline` call,
// including blank and unparseable lines — so this id is stable even if a
// future change alters which record types count as entries. The header is
// always line 1, so the first possible entry candidate is line 2.
//
// Similarly, a line with no "parentId" key defaults its parent to "the most
// recently assigned entry id", which by construction skips meta lines
// exactly as the old separate header-mutation pass already did. A line MAY
// carry an explicit "id"/"parentId" pair (silently ignored by pre-chain
// code, per message_types.cpp's tolerant parsing) — the walk below resolves
// those the same way it resolves legacy ids, which is what lets malformed
// hand-written fixtures exercise cycle/dangling-parent detection even
// though nothing in this phase *writes* real ids yet.
SessionIndex index_entries(std::ifstream &f, const std::string &session_id,
                           const std::filesystem::path &path) {
  SessionIndex index;
  bool first = true;
  std::string line;
  std::size_t line_number = 0;
  std::optional<std::string> last_entry_id;

  auto entry_id = [&](const nlohmann::json &j) -> std::string {
    if (j.contains("id") && j["id"].is_string())
      return j["id"].get<std::string>();
    return "legacy-" + std::to_string(line_number);
  };
  auto entry_parent =
      [&](const nlohmann::json &j) -> std::optional<std::string> {
    if (j.contains("parentId")) {
      if (j["parentId"].is_string())
        return j["parentId"].get<std::string>();
      return std::nullopt; // explicit null => root
    }
    return last_entry_id;
  };
  auto add_node =
      [&](const nlohmann::json &j,
          std::variant<Message, std::size_t, SessionCompactionRecord>
              payload) {
        std::string id = entry_id(j);
        SessionEntryNode node{
            .id = id, .parent_id = entry_parent(j), .payload = std::move(payload)};
        index.nodes.push_back(std::move(node));
        index.id_to_index[id] = index.nodes.size() - 1;
        last_entry_id = std::move(id);
      };

  while (std::getline(f, line)) {
    ++line_number;
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
      index.header = parse_header_line(j, session_id);
      continue;
    }

    if (j.value("type", std::string{}) == "compaction") {
      // Malformed compaction records must throw, not be silently skipped:
      // falling through here would silently restore the pre-compaction
      // transcript, which is exactly the "corrupted session" outcome this
      // format must not produce without telling the caller.
      auto record = parse_compaction_record(j, path);
      index.header.min_schema_version = std::max(
          index.header.min_schema_version, j["minVersion"].get<int>());
      add_node(j, std::move(record));
    } else if (j.contains("role")) {
      if (auto msg = json::from_json(line))
        add_node(j, std::move(*msg));
    } else if (j.value("type", std::string{}) == "truncate" &&
               j.contains("through") && j["through"].is_number_unsigned()) {
      add_node(j, j["through"].get<std::size_t>());
    } else if (j.value("type", std::string{}) == "meta") {
      if (j.contains("name") && j["name"].is_string())
        index.header.name = j["name"].get<std::string>();
      if (j.contains("sandboxMode") && j["sandboxMode"].is_string())
        index.header.sandbox_mode = j["sandboxMode"].get<std::string>();
      if (j.contains("provider") && j["provider"].is_string() &&
          j.contains("model") && j["model"].is_string()) {
        index.header.provider = j["provider"].get<std::string>();
        index.header.model = j["model"].get<std::string>();
      }
    }
  }

  if (first)
    throw std::runtime_error("empty or unreadable session file: " +
                             path.string());

  return index;
}

// Walks parent pointers from the leaf (default: the last entry, in file
// order — matching pi's `leaf ??= entries[entries.length-1]`) to the root,
// collecting the ordered chain of node ids, then reverses it to root-to-leaf
// order. Throws on a cycle or a dangling parent reference rather than
// silently truncating the walk — this format's existing convention (see
// `parse_compaction_record`'s minVersion check) is to fail loudly on any
// corrupted/ambiguous session state.
std::vector<std::string> walk_from_leaf(const SessionIndex &index,
                                        const std::filesystem::path &path) {
  if (index.nodes.empty())
    return {};

  std::vector<std::string> chain;
  std::set<std::string> seen;
  std::optional<std::string> current = index.nodes.back().id;

  while (current) {
    if (!seen.insert(*current).second)
      throw std::runtime_error("session entry cycle detected in " +
                               path.string() + " at " + *current);
    auto it = index.id_to_index.find(*current);
    if (it == index.id_to_index.end())
      throw std::runtime_error("session entry references unknown parent " +
                               *current + " in " + path.string());
    const auto &node = index.nodes[it->second];
    chain.push_back(node.id);
    current = node.parent_id;
  }

  std::ranges::reverse(chain);
  return chain;
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

  SessionIndex index = index_entries(f, session_id, path);
  std::vector<std::string> chain = walk_from_leaf(index, path);

  std::vector<Message> messages;
  std::vector<std::string> entry_ids;

  if (index.header.parent_id) {
    auto parent = load_recursive(base_dir, *index.header.parent_id, visited);
    std::size_t offset =
        index.header.parent_offset.value_or(parent.messages.size());
    if (offset > parent.messages.size())
      throw std::runtime_error(
          "corrupted fork: parent \"" + *index.header.parent_id +
          "\" has fewer messages than parentOffset=" + std::to_string(offset));

    messages.reserve(offset + chain.size());
    entry_ids.reserve(offset + chain.size());
    messages.insert(messages.end(), parent.messages.begin(),
                    std::next(parent.messages.begin(),
                              static_cast<std::ptrdiff_t>(offset)));
    entry_ids.insert(entry_ids.end(), parent.entry_ids.begin(),
                     std::next(parent.entry_ids.begin(),
                               static_cast<std::ptrdiff_t>(offset)));
  }

  // Replay the reconstructed chain forward, applying exactly the same
  // per-node-type effects the pre-chain imperative loop used: push
  // (message), resize (legacy truncate), wholesale-replace (compaction).
  // For any purely-linear (pre-migration) file this is provably identical
  // to today's behavior, because the chain *is* file order when nothing has
  // ever branched.
  for (const auto &id : chain) {
    auto &node = index.nodes[index.id_to_index.at(id)];
    if (std::holds_alternative<Message>(node.payload)) {
      messages.push_back(std::move(std::get<Message>(node.payload)));
      entry_ids.push_back(id);
    } else if (std::holds_alternative<std::size_t>(node.payload)) {
      std::size_t new_size =
          std::min(std::get<std::size_t>(node.payload), messages.size());
      messages.resize(new_size);
      entry_ids.resize(new_size);
    } else {
      // Compaction record: wholesale replace, discarding everything before
      // it (including any parent-inherited prefix). Subsequent chain nodes
      // append after the replacement. Every message it contributes shares
      // this node's single id.
      messages = std::move(std::get<SessionCompactionRecord>(node.payload).messages);
      entry_ids.assign(messages.size(), id);
    }
  }

  return SessionRecord{
      .header = std::move(index.header),
      .messages = std::move(messages),
      .entry_ids = std::move(entry_ids),
      .leaf_entry_id = chain.empty() ? std::nullopt
                                     : std::optional<std::string>(chain.back())};
}

} // namespace

// Called once during CLI startup / SessionStore construction, before any
// worker threads exist, so these std::getenv() calls never race.
std::filesystem::path SessionStore::default_sessions_dir() {
  std::filesystem::path base;
  if (const char *xdg =
          std::getenv("XDG_DATA_HOME"); // NOLINT(concurrency-mt-unsafe)
      xdg != nullptr && *xdg != '\0') {
    base = xdg;
  } else if (const char *home =
                 std::getenv("HOME"); // NOLINT(concurrency-mt-unsafe)
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
  std::scoped_lock lock(mutex_);

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
  j["sandboxMode"] = hdr.sandbox_mode ? nlohmann::json(*hdr.sandbox_mode)
                                      : nlohmann::json(nullptr);

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

void SessionStore::append_truncate(const std::string &session_id,
                                   std::size_t through) {
  nlohmann::json j = {{"type", "truncate"}, {"through", through}};
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

void SessionStore::set_model(const std::string &session_id,
                             std::string provider, std::string model) {
  auto now =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  nlohmann::json j = nlohmann::json::object();
  j["type"] = "meta";
  j["timestamp"] = static_cast<std::int64_t>(now);
  j["provider"] = std::move(provider);
  j["model"] = std::move(model);

  std::scoped_lock lock(mutex_);
  write_line_locked(session_id, j);
}

void SessionStore::append_compaction(const std::string &session_id,
                                     const SessionCompactionRecord &record) {
  // V1 concurrency policy: refuse compaction on any session that has
  // children on disk rather than silently invalidating every child's
  // parentOffset (which is a raw message-count index into this session's
  // replayed vector — a wholesale replacement changes what that index
  // means). Fork the session first, then compact the fork.
  if (has_children(session_id))
    throw std::runtime_error(
        "cannot compact session \"" + session_id +
        "\": it has forked children on disk; fork first, then compact the "
        "fork");

  nlohmann::json j = nlohmann::json::object();
  j["type"] = "compaction";
  j["minVersion"] = kSessionSchemaVersion;
  j["provider"] = record.provider;
  j["model"] = record.model;
  j["summary"] = record.summary;
  j["timestamp"] = record.timestamp;
  nlohmann::json messages = nlohmann::json::array();
  for (const auto &msg : record.messages)
    messages.push_back(nlohmann::json::parse(json::to_jsonl_line(msg)));
  j["messages"] = std::move(messages);

  std::scoped_lock lock(mutex_);
  write_line_locked(session_id, j);
}

bool SessionStore::has_children(const std::string &session_id) const {
  std::scoped_lock lock(mutex_);
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(base_dir_, ec)) {
    if (ec)
      break;
    if (entry.path().extension() != ".jsonl")
      continue;
    if (entry.path().stem().string() == session_id)
      continue;
    std::ifstream f(entry.path());
    if (!f)
      continue;
    std::string line;
    if (!std::getline(f, line) || line.empty())
      continue;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || j.value("type", std::string{}) != "session")
      continue;
    if (j.contains("parentId") && j["parentId"].is_string() &&
        j["parentId"].get<std::string>() == session_id)
      return true;
  }
  return false;
}

void SessionStore::set_sandbox_mode(const std::string &session_id,
                                    std::string mode) {
  auto now =
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  nlohmann::json j = nlohmann::json::object();
  j["type"] = "meta";
  j["timestamp"] = static_cast<std::int64_t>(now);
  j["sandboxMode"] = std::move(mode);

  std::scoped_lock lock(mutex_);
  write_line_locked(session_id, j);
}
std::optional<SessionRecord>
SessionStore::load(const std::string &session_id) const {
  std::scoped_lock lock(mutex_);
  std::error_code ec;
  if (!std::filesystem::exists(session_path(session_id), ec))
    return std::nullopt;
  std::set<std::string> visited;
  return load_recursive(base_dir_, session_id, visited);
}

std::optional<std::string> SessionStore::latest_session_id() const {
  std::scoped_lock lock(mutex_);
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
  std::scoped_lock lock(mutex_);
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

      std::optional<std::pair<std::string, std::string>> fallback_model;
      auto stem = dir_entry.path().stem().string();
      auto header = parse_header_line(j, stem);
      while (std::getline(f, line)) {
        if (line.empty())
          continue;
        auto meta = nlohmann::json::parse(line, nullptr, false);
        if (meta.is_discarded())
          continue;
        if (meta.value("type", std::string{}) != "meta") {
          auto message = json::from_json(line);
          if (message && std::holds_alternative<AssistantMessage>(*message)) {
            const auto &assistant = std::get<AssistantMessage>(*message);
            if (!assistant.provider.empty() && !assistant.model.empty()) {
              fallback_model =
                  std::make_pair(assistant.provider, assistant.model);
            }
          }
          continue;
        }
        if (meta.contains("name") && meta["name"].is_string())
          header.name = meta["name"].get<std::string>();
        if (meta.contains("sandboxMode") && meta["sandboxMode"].is_string())
          header.sandbox_mode = meta["sandboxMode"].get<std::string>();
        if (meta.contains("provider") && meta["provider"].is_string() &&
            meta.contains("model") && meta["model"].is_string()) {
          header.provider = meta["provider"].get<std::string>();
          header.model = meta["model"].get<std::string>();
        }
      }
      if (header.provider.empty() && fallback_model)
        header.provider = fallback_model->first;
      if (header.model.empty() && fallback_model)
        header.model = fallback_model->second;
      entries.emplace_back(dir_entry.last_write_time(), std::move(header));
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
