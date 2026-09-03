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
#include <format>
#include <fstream>
#include <ios>
#include <iterator>
#include <mutex>
#include <optional>
#include <random>
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
  if (j.contains("parentEntryAnchor") && j["parentEntryAnchor"].is_string()) {
    const auto anchor = j["parentEntryAnchor"].get<std::string>();
    if (anchor == "root") {
      hdr.parent_entry_anchor = ParentEntryAnchor::root;
    } else if (anchor == "entry") {
      hdr.parent_entry_anchor = ParentEntryAnchor::entry;
      if (j.contains("parentEntryId") && j["parentEntryId"].is_string())
        hdr.parent_entry_id = j["parentEntryId"].get<std::string>();
    }
    // Any other/unrecognized value is treated as absent (`none`) --
    // fail open to the legacy parent_offset path rather than throwing on a
    // forward-compat value this build doesn't understand.
  }
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
  // Absent "kind" (every compaction record written before this field
  // existed) means "summary" -- that's what they all were.
  record.kind = j.value("kind", std::string{"summary"});
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

// Walks parent pointers from `start_id` (default: nullopt, meaning the
// leaf — the last entry, in file order, matching pi's
// `leaf ??= entries[entries.length-1]`) to the root, collecting the ordered
// chain of node ids, then reverses it to root-to-leaf order. Throws on a
// cycle or a dangling parent reference rather than silently truncating the
// walk — this format's existing convention (see `parse_compaction_record`'s
// minVersion check) is to fail loudly on any corrupted/ambiguous session
// state. Phase 2's `branch()` reuses this to walk from an arbitrary
// explicit rewind target instead of only ever the file's own leaf; the
// caller is responsible for having already verified `start_id` resolves
// (branch() does, before calling in, so a bad caller-supplied id is
// reported as "branch target not found" rather than the generic
// dangling-parent message below).
std::vector<std::string>
walk_from_leaf(const SessionIndex &index, const std::filesystem::path &path,
               std::optional<std::string> start_id = std::nullopt) {
  if (index.nodes.empty())
    return {};

  std::vector<std::string> chain;
  std::set<std::string> seen;
  std::optional<std::string> current =
      start_id ? std::move(start_id)
               : std::optional<std::string>(index.nodes.back().id);

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

// Mirrors pi's `randomUUID().slice(0,8)` id-minting scheme: an 8-hex-digit
// random id, not a per-session monotonic counter. A counter is unsafe here
// because two processes touching the same session file (or a stale
// in-process counter after a restart) would mint colliding "e0"/"e1"/...
// ids, silently corrupting the id -> node map; a random id makes an actual
// collision astronomically unlikely, and mint_entry_locked below still
// checks and regenerates rather than relying on luck alone.
std::string random_entry_id() {
  static thread_local std::mt19937_64 rng(std::random_device{}());
  std::uniform_int_distribution<std::uint32_t> dist;
  return std::format("{:08x}", dist(rng));
}

// Result of minting a new entry id for a write: the id itself, the entry
// id to write as this record's "parentId" (nullopt == root), and the
// on-disk index it was checked against (handed back so branch() can also
// walk an arbitrary target from the very index it minted against, instead
// of re-reading the file a second time).
struct MintedEntry {
  std::string id;
  std::optional<std::string> parent;
  SessionIndex index;
};

// Must be called only while the caller holds SessionStore::mutex_ (reads
// the file directly, bypassing the public, self-locking `load()` — see the
// module-wide locking rule: no method that takes mutex_ may call another
// method that also takes it). Rebuilds `session_id`'s on-disk index fresh
// on every call and resolves the current leaf from THAT SAME index via
// `walk_from_leaf`, rather than trusting a previously-cached leaf id --
// deliberately, even though this process may already have a cached value
// in `leaf_ids`: the index is already being rebuilt for the id-collision
// check below (a counter would be unsafe against another process or a
// stale in-process view, per `random_entry_id`'s comment; a random id only
// makes collision astronomically unlikely, so this still checks rather
// than trusting luck), so re-deriving the leaf from it costs no extra I/O
// and stays correct even if something else appended to this session's file
// since `leaf_ids` was last set (a second process, or -- as this file's own
// hand-written-fixture tests do -- a raw append bypassing the store
// entirely). Callers still record the minted id into their own leaf_ids_
// cache after a successful write; that cache exists as this process's
// record of its own last write (for future callers that want a leaf
// without a disk read), not as an authority this function trusts over the
// disk.
MintedEntry mint_entry_locked(const std::filesystem::path &path,
                              const std::string &session_id) {
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("session file not found: " + path.string());
  SessionIndex index = index_entries(f, session_id, path);

  std::string id;
  do {
    id = random_entry_id();
  } while (index.id_to_index.contains(id));

  auto chain = walk_from_leaf(index, path);
  std::optional<std::string> parent =
      chain.empty() ? std::nullopt : std::optional<std::string>(chain.back());

  return MintedEntry{.id = std::move(id), .parent = std::move(parent),
                     .index = std::move(index)};
}

// Must be called only while the caller holds SessionStore::mutex_ (reads
// every other session file directly, same convention as
// `mint_entry_locked`). Scans the store's directory for children of
// `session_id` the way `has_children` does, but decides per-child whether
// it actually BLOCKS compaction rather than treating "has any child" as
// disqualifying:
//   - a child with no anchor recorded (`parentEntryAnchor` absent --
//     either a pre-Phase-3 fork, or one written by a pre-Phase-3 binary)
//     relies entirely on the exact message-count `parentOffset`, which a
//     wholesale compaction invalidates -- BLOCKS.
//   - a child anchored `root` never depended on a specific entry --
//     compacting only ever appends one new node whose parent is the
//     current leaf, so it never renumbers, deletes, or hides the "before
//     any entries" fork point -- does NOT block.
//   - a child anchored `entry` whose `parentEntryId` still resolves
//     against `session_id`'s CURRENT index does not block, for the same
//     reason: entries are permanent once written (compaction appends, it
//     never rewrites or removes an existing node), so the child can walk
//     to that exact ancestor entry forever, regardless of how many times
//     `session_id` itself is later compacted.
//   - a child anchored `entry` whose `parentEntryId` does NOT resolve
//     (corrupted/stale header) still BLOCKS -- a broken anchor must never
//     be treated as safe to compact past.
//
// IMPORTANT caveat this narrowing deliberately accepts, not an oversight:
// none of this lets a compacted parent shrink an entry-anchored child's
// inherited prefix. The child's inherited prefix is always computed by
// walking `session_id`'s tree to that specific old entry id (see
// `load_recursive`'s `ParentEntryAnchor::entry` case), never to
// `session_id`'s new post-compaction leaf -- so `session_id`'s on-disk
// file can never be pruned or deleted while such a child exists; it must
// keep replaying the full pre-compaction ancestor chain forever. That is
// the tradeoff this narrowing makes in exchange for no longer blanket-
// refusing to compact a session with any fork at all.
bool has_blocking_children_locked(const std::filesystem::path &base_dir,
                                  const std::string &session_id) {
  auto self_path = base_dir / (session_id + ".jsonl");
  std::ifstream self_f(self_path);
  SessionIndex self_index;
  bool have_self_index = false;
  if (self_f) {
    self_index = index_entries(self_f, session_id, self_path);
    have_self_index = true;
  }
  // If the session's own file can't be read/indexed here, fall through and
  // let append_compaction's own subsequent open surface the real error;
  // this scan just treats it as "can't verify resolvability", which the
  // `entry` branch below already handles by blocking.

  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(base_dir, ec)) {
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
    if (!(j.contains("parentId") && j["parentId"].is_string() &&
         j["parentId"].get<std::string>() == session_id))
      continue;

    auto child_hdr = parse_header_line(j, entry.path().stem().string());
    switch (child_hdr.parent_entry_anchor) {
    case ParentEntryAnchor::root:
      continue; // never blocks
    case ParentEntryAnchor::entry:
      if (!have_self_index || !child_hdr.parent_entry_id ||
          !self_index.id_to_index.contains(*child_hdr.parent_entry_id))
        return true; // unresolvable/malformed anchor -- must still block
      continue;       // resolvable -- does not block
    case ParentEntryAnchor::none:
      return true; // legacy count-slice fork -- still blocks
    }
  }
  return false;
}

// NOLINTNEXTLINE(misc-no-recursion)
//
// `target_is_leaf`/`target_id` select which node of THIS session's own
// chain to walk to and replay (not how it recurses into its parent, which
// is driven entirely by the loaded header's own parent_entry_anchor):
//   target_is_leaf == true  -- walk to the leaf (default; every top-level
//                              `load()` call and the legacy `none`-anchor
//                              parent recursion use this).
//   target_is_leaf == false, target_id set    -- walk to that specific
//                              entry id within this session's own index
//                              (used when recursing into a parent for an
//                              `entry`-anchored child).
//   target_is_leaf == false, target_id nullopt -- empty chain (root),
//                              mirroring SessionStore::branch's own
//                              nullopt-target handling. Not reached via the
//                              anchor==root path below (that path never
//                              recurses into the parent at all), kept for
//                              symmetry/future callers.
SessionRecord load_recursive(const std::filesystem::path &base_dir,
                             const std::string &session_id,
                             std::set<std::string> &visited,
                             std::optional<std::string> target_id = std::nullopt,
                             bool target_is_leaf = true) {
  if (!visited.insert(session_id).second)
    throw std::runtime_error("session cycle detected at: " + session_id);

  auto path = base_dir / (session_id + ".jsonl");
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("session file not found: " + path.string());

  SessionIndex index = index_entries(f, session_id, path);

  std::vector<std::string> chain;
  if (target_is_leaf) {
    chain = walk_from_leaf(index, path);
  } else if (target_id) {
    if (!index.id_to_index.contains(*target_id))
      throw std::runtime_error("corrupted fork: parent \"" + session_id +
                               "\" has no entry \"" + *target_id + "\"");
    chain = walk_from_leaf(index, path, target_id);
  }
  // else: target is root -- empty chain, no entries replayed.

  std::vector<Message> messages;
  std::vector<std::string> entry_ids;

  if (index.header.parent_id) {
    switch (index.header.parent_entry_anchor) {
    case ParentEntryAnchor::entry: {
      if (!index.header.parent_entry_id)
        throw std::runtime_error(
            "corrupted fork: session \"" + session_id +
            "\" has parentEntryAnchor \"entry\" but no parentEntryId");
      // Walk the PARENT's own tree to this specific entry (not the
      // parent's current leaf) -- entries are permanent once written
      // (compaction only ever appends a new node), so this resolves
      // correctly even if the parent has since gained new entries, been
      // branched elsewhere, or been compacted.
      auto parent = load_recursive(base_dir, *index.header.parent_id, visited,
                                   index.header.parent_entry_id,
                                   /*target_is_leaf=*/false);
      messages = std::move(parent.messages);
      entry_ids = std::move(parent.entry_ids);
      break;
    }
    case ParentEntryAnchor::root:
      // Fork point is before the parent's first entry -- no inherited
      // prefix at all (not even a recursive grandparent-inherited prefix):
      // matches SessionStore::branch's own nullopt-target ("root") case,
      // where the resulting history is genuinely empty.
      break;
    case ParentEntryAnchor::none: {
      // Legacy path, UNCHANGED: sessions forked by a pre-Phase-3 binary
      // (or predating this field) only ever recorded a message-count
      // offset into the parent's own replayed vector.
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
      break;
    }
    }
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
  switch (hdr.parent_entry_anchor) {
  case ParentEntryAnchor::root:
    j["parentEntryAnchor"] = "root";
    break;
  case ParentEntryAnchor::entry:
    j["parentEntryAnchor"] = "entry";
    j["parentEntryId"] = hdr.parent_entry_id ? nlohmann::json(*hdr.parent_entry_id)
                                              : nlohmann::json(nullptr);
    break;
  case ParentEntryAnchor::none:
    // Absent entirely -- matches how a pre-Phase-3 header looks, so an old
    // binary (and this build's own `none`-fallback parsing) treats it
    // identically to "no anchor recorded".
    break;
  }

  write_line_locked(sid, j);
  return sid;
}

void SessionStore::append_message(const std::string &session_id,
                                  const Message &msg) {
  auto line = json::to_jsonl_line(msg);
  nlohmann::json j = nlohmann::json::parse(line);
  std::scoped_lock lock(mutex_);
  auto minted = mint_entry_locked(session_path(session_id), session_id);
  j["id"] = minted.id;
  j["parentId"] =
      minted.parent ? nlohmann::json(*minted.parent) : nlohmann::json(nullptr);
  write_line_locked(session_id, j);
  leaf_ids_[session_id] = std::move(minted.id);
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
  nlohmann::json j = nlohmann::json::object();
  j["type"] = "compaction";
  j["minVersion"] = kSessionSchemaVersion;
  j["provider"] = record.provider;
  j["model"] = record.model;
  j["summary"] = record.summary;
  j["kind"] = record.kind;
  j["timestamp"] = record.timestamp;
  nlohmann::json messages = nlohmann::json::array();
  for (const auto &msg : record.messages)
    messages.push_back(nlohmann::json::parse(json::to_jsonl_line(msg)));
  j["messages"] = std::move(messages);

  std::scoped_lock lock(mutex_);
  // Narrowed (Phase 3) concurrency guard: refuse compaction only for
  // children that would actually be broken by it -- see
  // `has_blocking_children_locked`'s comment for the full reasoning and
  // the deliberate "parent file can never be pruned while an entry-
  // anchored child exists" tradeoff this narrowing accepts. Checked here,
  // under the lock, instead of via the old pre-lock `has_children()` call,
  // because it also needs to resolve candidate children's anchors against
  // THIS session's current index.
  if (has_blocking_children_locked(base_dir_, session_id))
    throw std::runtime_error(
        "cannot compact session \"" + session_id +
        "\": it has forked children on disk that still depend on a "
        "message-count offset; fork first, then compact the fork");

  auto minted = mint_entry_locked(session_path(session_id), session_id);
  j["id"] = minted.id;
  j["parentId"] =
      minted.parent ? nlohmann::json(*minted.parent) : nlohmann::json(nullptr);
  write_line_locked(session_id, j);
  leaf_ids_[session_id] = std::move(minted.id);
}

std::vector<Message>
SessionStore::branch(const std::string &session_id,
                     std::optional<std::string> target_entry_id) {
  std::scoped_lock lock(mutex_);
  auto path = session_path(session_id);
  auto minted = mint_entry_locked(path, session_id);

  std::vector<std::string> chain;
  if (target_entry_id) {
    if (!minted.index.id_to_index.contains(*target_entry_id))
      throw std::runtime_error("branch target not found: \"" +
                               *target_entry_id + "\" in session \"" +
                               session_id + "\"");
    chain = walk_from_leaf(minted.index, path, target_entry_id);
  }
  // else: target is the root -- an empty chain replays to an empty
  // message list below, exactly like any other session with no entries.

  // Replay the target's chain forward with the same per-node-type effects
  // load_recursive's own chain replay uses (push / resize / wholesale
  // replace) -- this is what makes the branch record's "messages" field
  // identical to what a load() to that same point would have produced. No
  // new node type is needed: a compaction node the walk lands on already
  // terminates ancestor lookup and wholesale-replaces, exactly like this
  // function's own output will once it's written and later replayed.
  std::vector<Message> messages;
  for (const auto &id : chain) {
    auto &node = minted.index.nodes[minted.index.id_to_index.at(id)];
    if (std::holds_alternative<Message>(node.payload)) {
      messages.push_back(std::get<Message>(node.payload));
    } else if (std::holds_alternative<std::size_t>(node.payload)) {
      messages.resize(
          std::min(std::get<std::size_t>(node.payload), messages.size()));
    } else {
      messages = std::get<SessionCompactionRecord>(node.payload).messages;
    }
  }

  nlohmann::json j = nlohmann::json::object();
  j["type"] = "compaction";
  j["id"] = minted.id;
  j["parentId"] =
      minted.parent ? nlohmann::json(*minted.parent) : nlohmann::json(nullptr);
  j["minVersion"] = kSessionSchemaVersion;
  j["kind"] = "branch";
  j["provider"] = minted.index.header.provider;
  j["model"] = minted.index.header.model;
  j["summary"] = std::string{};
  j["timestamp"] = static_cast<std::int64_t>(
      std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()));
  nlohmann::json messages_json = nlohmann::json::array();
  for (const auto &msg : messages)
    messages_json.push_back(nlohmann::json::parse(json::to_jsonl_line(msg)));
  j["messages"] = std::move(messages_json);

  write_line_locked(session_id, j);
  leaf_ids_[session_id] = std::move(minted.id);
  return messages;
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

SessionStore::LeafAnchor
SessionStore::current_leaf_anchor(const std::string &session_id) const {
  std::scoped_lock lock(mutex_);
  auto path = session_path(session_id);
  // Deliberately does NOT go through mint_entry_locked (that mints and
  // discards a throwaway random id for no reason here) or through the
  // public, self-locking load() (would deadlock against the lock already
  // held above -- no method that holds mutex_ may call another method
  // that also takes it). Reads and indexes the file directly instead, the
  // same way mint_entry_locked itself does.
  std::ifstream f(path);
  if (!f)
    throw std::runtime_error("session file not found: " + path.string());
  SessionIndex index = index_entries(f, session_id, path);

  if (index.nodes.empty())
    // No entries at all -- a child forking from this session anchors
    // "before any entries" (root), NOT "no anchor recorded" (none). This
    // is what fixes forking a fork that itself has zero own entries: such
    // a session's own file has no nodes of its own even though it
    // inherits a non-empty prefix from ITS parent, so `root` here
    // correctly means "no entries of *this* session's own", matching
    // `load_recursive`'s ParentEntryAnchor::root handling (no inherited
    // prefix at all, not even a further-inherited grandparent one).
    return LeafAnchor{.anchor = ParentEntryAnchor::root, .entry_id = std::nullopt};

  auto chain = walk_from_leaf(index, path);
  // index.nodes is non-empty, so the leaf's own chain always contains at
  // least itself -- chain can't be empty here.
  return LeafAnchor{.anchor = ParentEntryAnchor::entry,
                    .entry_id = std::optional<std::string>(chain.back())};
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
