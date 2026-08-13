#include "core/mailbox/mailbox_store.h"
#include "core/mailbox/mailbox_types.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace pi::core {

namespace {

using json = nlohmann::json; // NOLINT(misc-include-cleaner)
constexpr std::int64_t kSchemaVersion = 1;
constexpr std::size_t kMaxTextBytes = static_cast<std::size_t>(64) * 1024;

TimestampMs system_now() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string random_id() {
  static thread_local std::mt19937_64 generator(std::random_device{}());
  std::ostringstream result;
  result << std::hex; // NOLINT(misc-include-cleaner)
  for (int index = 0; index < 4; ++index)
    result << generator();
  return result.str();
}

std::string sqlite_message(sqlite3 *database, int result) {
  return "SQLite error (" + std::to_string(result) +
         "): " + sqlite3_errmsg(database);
}

[[noreturn]] void throw_sqlite(sqlite3 *database, int result,
                               std::string_view operation) {
  const auto code = result == SQLITE_BUSY || result == SQLITE_LOCKED
                        ? MailboxErrorCode::busy
                        : MailboxErrorCode::internal;
  throw MailboxError(code, std::string(operation) + ": " +
                               sqlite_message(database, result));
}

void check_sqlite(sqlite3 *database, int result, std::string_view operation) {
  if (result != SQLITE_OK && result != SQLITE_DONE && result != SQLITE_ROW)
    throw_sqlite(database, result, operation);
}

void exec(sqlite3 *database, std::string_view sql) {
  char *error = nullptr;
  const std::string sql_string(sql);
  const int result =
      sqlite3_exec(database, sql_string.c_str(), nullptr, nullptr, &error);
  if (result != SQLITE_OK) {
    const std::string message =
        error != nullptr ? error : sqlite3_errmsg(database);
    sqlite3_free(error);
    const auto code = result == SQLITE_BUSY || result == SQLITE_LOCKED
                          ? MailboxErrorCode::busy
                          : MailboxErrorCode::internal;
    throw MailboxError(code, message);
  }
}

class Statement {
public:
  Statement(sqlite3 *database, std::string_view sql) : database_(database) {
    const std::string sql_string(sql);
    const int result = sqlite3_prepare_v2(database_, sql_string.c_str(), -1,
                                          &statement_, nullptr);
    check_sqlite(database_, result, "prepare statement");
  }

  ~Statement() {
    if (statement_ != nullptr)
      sqlite3_finalize(statement_);
  }

  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;

  sqlite3_stmt *get() { return statement_; }
  void reset() {
    check_sqlite(database_, sqlite3_reset(statement_), "reset statement");
    check_sqlite(database_, sqlite3_clear_bindings(statement_),
                 "clear statement bindings");
  }

private:
  sqlite3 *database_;
  sqlite3_stmt *statement_{nullptr};
};

void bind_text(sqlite3_stmt *statement, int index, std::string_view value) {
  check_sqlite(sqlite3_db_handle(statement),
               sqlite3_bind_text(statement, index, value.data(),
                                 static_cast<int>(value.size()),
                                 SQLITE_TRANSIENT),
               "bind text");
}

void bind_optional_text(sqlite3_stmt *statement, int index,
                        const std::optional<std::string> &value) {
  if (value)
    bind_text(statement, index, *value);
  else
    check_sqlite(sqlite3_db_handle(statement),
                 sqlite3_bind_null(statement, index), "bind null");
}

void bind_integer(sqlite3_stmt *statement, int index, std::int64_t value) {
  check_sqlite(sqlite3_db_handle(statement),
               sqlite3_bind_int64(statement, index, value), "bind integer");
}

void bind_optional_integer(sqlite3_stmt *statement, int index,
                           const std::optional<TimestampMs> &value) {
  if (value)
    bind_integer(statement, index, *value);
  else
    check_sqlite(sqlite3_db_handle(statement),
                 sqlite3_bind_null(statement, index), "bind null");
}

std::string column_text(sqlite3_stmt *statement, int index) {
  const auto *value = sqlite3_column_text(statement, index);
  return value == nullptr
             ? std::string{}
             : std::string(
                   // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                   reinterpret_cast<const char *>(value),
                   static_cast<std::size_t>(
                       sqlite3_column_bytes(statement, index)));
}

std::optional<std::string> optional_column_text(sqlite3_stmt *statement,
                                                int index) {
  if (sqlite3_column_type(statement, index) == SQLITE_NULL)
    return std::nullopt;
  return column_text(statement, index);
}

std::optional<TimestampMs> optional_column_integer(sqlite3_stmt *statement,
                                                   int index) {
  if (sqlite3_column_type(statement, index) == SQLITE_NULL)
    return std::nullopt;
  return sqlite3_column_int64(statement, index);
}

json body_json(const MailboxBody &body) {
  json metadata = json::object();
  for (const auto &[key, value] : body.metadata)
    metadata[key] = value;
  return json{{"version", 1}, {"text", body.text}, {"metadata", metadata}};
}

MailboxBody parse_body(std::string_view serialized) {
  try {
    const auto body = json::parse(serialized);
    if (!body.is_object() || body.value("version", 0) != 1 ||
        !body.contains("text") || !body["text"].is_string())
      throw std::runtime_error("invalid body envelope");
    MailboxBody result{.text = body["text"].get<std::string>()};
    if (const auto metadata = body.find("metadata");
        metadata != body.end() && metadata->is_object()) {
      for (auto item = metadata->begin(); item != metadata->end(); ++item) {
        if (item.value().is_string())
          result.metadata.emplace_back(item.key(),
                                       item.value().get<std::string>());
      }
    }
    return result;
  } catch (const std::exception &error) {
    throw MailboxError(MailboxErrorCode::invalid_message,
                       std::string("invalid mailbox body: ") + error.what());
  }
}

bool contains_kind(const std::vector<MailboxMessageKind> &kinds,
                   MailboxMessageKind kind) {
  return kinds.empty() || std::ranges::find(kinds, kind) != kinds.end();
}

MailboxMessage read_message(sqlite3_stmt *statement) {
  MailboxMessage message{
      .message_id = column_text(statement, 0),
      .sender_agent_id = column_text(statement, 1),
      .sender_session_id = column_text(statement, 2),
      .recipient_session_id = column_text(statement, 3),
      .recipient_agent_id = optional_column_text(statement, 4),
      .workspace_id = column_text(statement, 5),
      .kind = mailbox_message_kind_from_string(column_text(statement, 6))
                  .value_or(MailboxMessageKind::note),
      .body = parse_body(column_text(statement, 7)),
      .reply_to_message_id = optional_column_text(statement, 8),
      .created_at_ms = sqlite3_column_int64(statement, 9),
      .available_at_ms = sqlite3_column_int64(statement, 10),
      .claim_agent_id = optional_column_text(statement, 11),
      .claim_token = optional_column_text(statement, 12),
      .claim_expires_at_ms = optional_column_integer(statement, 13),
      .delivered_at_ms = optional_column_integer(statement, 14),
      .acknowledged_at_ms = optional_column_integer(statement, 15),
      .failed_at_ms = optional_column_integer(statement, 16),
      .failure = optional_column_text(statement, 17),
  };
  return message;
}

constexpr std::string_view kMessageColumns =
    "message_id, sender_agent_id, sender_session_id, recipient_session_id, "
    "recipient_agent_id, workspace_id, kind, body_json, reply_to_message_id, "
    "created_at_ms, available_at_ms, claim_agent_id, claim_token, "
    "claim_expires_at_ms, delivered_at_ms, acknowledged_at_ms, failed_at_ms, "
    "failure";

void insert_event(sqlite3 *database, std::string_view workspace_id,
                  std::string_view event_type, std::string_view subject_id,
                  TimestampMs created_at_ms) {
  Statement event(
      database,
      "INSERT INTO "
      "mailbox_events(workspace_id,event_type,subject_id,created_at_ms) "
      "VALUES (?, ?, ?, ?)");
  bind_text(event.get(), 1, workspace_id);
  bind_text(event.get(), 2, event_type);
  bind_text(event.get(), 3, subject_id);
  bind_integer(event.get(), 4, created_at_ms);
  check_sqlite(database, sqlite3_step(event.get()), "record mailbox event");
}

} // namespace

MailboxStore::MailboxStore(MailboxStoreOptions options)
    : options_(std::move(options)) {
  if (options_.path.empty() || options_.workspace_id.empty() ||
      options_.workspace_path.empty())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "mailbox path and workspace identity are required");
  if (!options_.clock)
    options_.clock = system_now;
  if (!options_.id_generator)
    options_.id_generator = random_id;
  open();
  configure_connection();
  migrate();
}

MailboxStore::~MailboxStore() {
  std::scoped_lock lock(mutex_);
  if (database_ != nullptr)
    sqlite3_close(database_);
}

void MailboxStore::open() {
  const auto parent = options_.path.parent_path();
  std::error_code error;
  if (!parent.empty()) {
    std::filesystem::create_directories(parent, error);
    if (error)
      throw MailboxError(MailboxErrorCode::permission_denied,
                         "failed to create mailbox directory: " +
                             error.message());
    std::filesystem::permissions(parent,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace, error);
    if (error)
      throw MailboxError(MailboxErrorCode::permission_denied,
                         "failed to secure mailbox directory: " +
                             error.message());
  }
  struct stat path_stat{};
  if (::lstat(options_.path.c_str(), &path_stat) == 0 &&
      S_ISLNK(path_stat.st_mode))
    throw MailboxError(MailboxErrorCode::permission_denied,
                       "mailbox path must not be a symbolic link");
  const int result =
      sqlite3_open_v2(options_.path.c_str(), &database_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
  if (result != SQLITE_OK) {
    const std::string message = database_ == nullptr
                                    ? "failed to open mailbox"
                                    : sqlite3_errmsg(database_);
    if (database_ != nullptr)
      sqlite3_close(database_);
    database_ = nullptr;
    throw MailboxError(MailboxErrorCode::internal, message);
  }
  std::filesystem::permissions(options_.path,
                               std::filesystem::perms::owner_read |
                                   std::filesystem::perms::owner_write,
                               std::filesystem::perm_options::replace, error);
  if (error)
    throw MailboxError(MailboxErrorCode::permission_denied,
                       "failed to secure mailbox database: " + error.message());
}

void MailboxStore::configure_connection() {
  std::scoped_lock lock(mutex_);
  exec(database_, "PRAGMA foreign_keys = ON");
  exec(database_, "PRAGMA journal_mode = WAL");
  exec(database_, "PRAGMA synchronous = NORMAL");
  exec(database_, "PRAGMA busy_timeout = 5000");
}

void MailboxStore::migrate() {
  std::scoped_lock lock(mutex_);
  Statement version(database_, "PRAGMA user_version");
  const int result = sqlite3_step(version.get());
  check_sqlite(database_, result, "read mailbox schema version");
  const auto current = sqlite3_column_int64(version.get(), 0);
  if (current > kSchemaVersion)
    throw MailboxError(MailboxErrorCode::incompatible_schema,
                       "mailbox database schema is newer than this binary");
  if (current == kSchemaVersion)
    return;

  exec(database_, "BEGIN IMMEDIATE");
  try {
    exec(database_, R"sql(
      CREATE TABLE IF NOT EXISTS processes (
        process_id TEXT PRIMARY KEY,
        workspace_id TEXT NOT NULL,
        workspace_path TEXT NOT NULL,
        pid INTEGER NOT NULL,
        hostname TEXT NOT NULL,
        protocol_version INTEGER NOT NULL,
        capabilities_json TEXT NOT NULL,
        started_at_ms INTEGER NOT NULL,
        last_seen_at_ms INTEGER NOT NULL,
        lease_expires_at_ms INTEGER NOT NULL,
        closed_at_ms INTEGER
      );
      CREATE TABLE IF NOT EXISTS agents (
        agent_id TEXT PRIMARY KEY,
        process_id TEXT NOT NULL REFERENCES processes(process_id),
        kind TEXT NOT NULL CHECK(kind IN ('root', 'subagent')),
        owner_agent_id TEXT REFERENCES agents(agent_id),
        session_id TEXT NOT NULL,
        session_name TEXT,
        task_id TEXT,
        task_path TEXT,
        provider TEXT NOT NULL,
        model_id TEXT NOT NULL,
        status TEXT NOT NULL,
        started_at_ms INTEGER NOT NULL,
        closed_at_ms INTEGER
      );
      CREATE INDEX IF NOT EXISTS processes_workspace_live
        ON processes(workspace_id, lease_expires_at_ms);
      CREATE INDEX IF NOT EXISTS agents_session_live
        ON agents(session_id, status);
      CREATE INDEX IF NOT EXISTS agents_process
        ON agents(process_id, status);
      CREATE TABLE IF NOT EXISTS messages (
        message_id TEXT PRIMARY KEY,
        sender_agent_id TEXT NOT NULL,
        sender_session_id TEXT NOT NULL,
        recipient_session_id TEXT NOT NULL,
        recipient_agent_id TEXT,
        workspace_id TEXT NOT NULL,
        kind TEXT NOT NULL CHECK(kind IN ('steer', 'note', 'request', 'reply')),
        body_json TEXT NOT NULL,
        reply_to_message_id TEXT,
        created_at_ms INTEGER NOT NULL,
        available_at_ms INTEGER NOT NULL,
        claim_agent_id TEXT,
        claim_token TEXT,
        claim_expires_at_ms INTEGER,
        delivered_at_ms INTEGER,
        acknowledged_at_ms INTEGER,
        failed_at_ms INTEGER,
        failure TEXT
      );
      CREATE INDEX IF NOT EXISTS messages_recipient_pending
        ON messages(recipient_session_id, acknowledged_at_ms, available_at_ms);
      CREATE INDEX IF NOT EXISTS messages_workspace_created
        ON messages(workspace_id, created_at_ms);
      CREATE TABLE IF NOT EXISTS mailbox_events (
        generation INTEGER PRIMARY KEY AUTOINCREMENT,
        workspace_id TEXT NOT NULL,
        event_type TEXT NOT NULL,
        subject_id TEXT NOT NULL,
        created_at_ms INTEGER NOT NULL
      );
    )sql");
    exec(database_, "PRAGMA user_version = 1");
    exec(database_, "COMMIT");
  } catch (...) {
    exec(database_, "ROLLBACK");
    throw;
  }
}

void MailboxStore::check_workspace(std::string_view workspace_id) const {
  if (workspace_id.empty() || (options_.scope == MailboxScope::workspace &&
                               workspace_id != options_.workspace_id))
    throw MailboxError(MailboxErrorCode::permission_denied,
                       "mailbox operation is outside the configured workspace");
}

void MailboxStore::register_process(const ProcessRecord &process) {
  std::scoped_lock lock(mutex_);
  check_workspace(process.workspace_id);
  exec(database_, "BEGIN IMMEDIATE");
  try {
    Statement statement(database_, R"sql(
    INSERT INTO processes(process_id, workspace_id, workspace_path, pid,
      hostname, protocol_version, capabilities_json, started_at_ms,
      last_seen_at_ms, lease_expires_at_ms, closed_at_ms)
    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    ON CONFLICT(process_id) DO UPDATE SET workspace_id=excluded.workspace_id,
      workspace_path=excluded.workspace_path, pid=excluded.pid,
      hostname=excluded.hostname, protocol_version=excluded.protocol_version,
      capabilities_json=excluded.capabilities_json,
      started_at_ms=excluded.started_at_ms,
      last_seen_at_ms=excluded.last_seen_at_ms,
      lease_expires_at_ms=excluded.lease_expires_at_ms,
      closed_at_ms=excluded.closed_at_ms
    )sql");
    bind_text(statement.get(), 1, process.process_id);
    bind_text(statement.get(), 2, process.workspace_id);
    bind_text(statement.get(), 3, process.workspace_path);
    bind_integer(statement.get(), 4, process.pid);
    bind_text(statement.get(), 5, process.hostname);
    bind_integer(statement.get(), 6, process.protocol_version);
    bind_text(statement.get(), 7, process.capabilities_json);
    bind_integer(statement.get(), 8, process.started_at_ms);
    bind_integer(statement.get(), 9, process.last_seen_at_ms);
    bind_integer(statement.get(), 10, process.lease_expires_at_ms);
    bind_optional_integer(statement.get(), 11, process.closed_at_ms);
    check_sqlite(database_, sqlite3_step(statement.get()), "register process");
    insert_event(database_, process.workspace_id, "process_registered",
                 process.process_id, process.started_at_ms);
    exec(database_, "COMMIT");
  } catch (...) {
    exec(database_, "ROLLBACK");
    throw;
  }
}

void MailboxStore::heartbeat_process(std::string_view process_id,
                                     TimestampMs now,
                                     TimestampMs lease_expires_at) {
  std::scoped_lock lock(mutex_);
  const auto *const workspace_sql =
      options_.scope == MailboxScope::global ? "" : " AND workspace_id=?";
  Statement statement(
      database_,
      "UPDATE processes SET last_seen_at_ms=?, lease_expires_at_ms=?, "
      "closed_at_ms=NULL WHERE process_id=?" +
          std::string(workspace_sql));
  bind_integer(statement.get(), 1, now);
  bind_integer(statement.get(), 2, lease_expires_at);
  bind_text(statement.get(), 3, process_id);
  if (options_.scope == MailboxScope::workspace)
    bind_text(statement.get(), 4, options_.workspace_id);
  check_sqlite(database_, sqlite3_step(statement.get()), "heartbeat process");
  if (sqlite3_changes(database_) == 0)
    throw MailboxError(MailboxErrorCode::not_found,
                       "mailbox process not found");
}

void MailboxStore::close_process(std::string_view process_id, TimestampMs now) {
  std::scoped_lock lock(mutex_);
  exec(database_, "BEGIN IMMEDIATE");
  try {
    Statement workspace(
        database_, "SELECT workspace_id FROM processes WHERE "
                   "process_id=?" +
                       std::string(options_.scope == MailboxScope::workspace
                                       ? " AND workspace_id=?"
                                       : ""));
    bind_text(workspace.get(), 1, process_id);
    if (options_.scope == MailboxScope::workspace)
      bind_text(workspace.get(), 2, options_.workspace_id);
    const int workspace_step = sqlite3_step(workspace.get());
    check_sqlite(database_, workspace_step, "find mailbox process");
    if (workspace_step != SQLITE_ROW)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox process not found");
    const auto process_workspace = column_text(workspace.get(), 0);
    Statement agents(database_,
                     "UPDATE agents SET closed_at_ms=? WHERE process_id=? AND "
                     "closed_at_ms IS NULL");
    bind_integer(agents.get(), 1, now);
    bind_text(agents.get(), 2, process_id);
    check_sqlite(database_, sqlite3_step(agents.get()), "close process agents");
    Statement process(database_, "UPDATE processes SET closed_at_ms=?, "
                                 "lease_expires_at_ms=? WHERE process_id=?");
    bind_integer(process.get(), 1, now);
    bind_integer(process.get(), 2, now);
    bind_text(process.get(), 3, process_id);
    check_sqlite(database_, sqlite3_step(process.get()), "close process");
    if (sqlite3_changes(database_) == 0)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox process not found");
    insert_event(database_, process_workspace, "process_closed", process_id,
                 now);
    Statement closed_agents(database_,
                            "SELECT agent_id FROM agents WHERE process_id=? "
                            "AND closed_at_ms=?");
    bind_text(closed_agents.get(), 1, process_id);
    bind_integer(closed_agents.get(), 2, now);
    while (true) {
      const int step = sqlite3_step(closed_agents.get());
      if (step == SQLITE_DONE)
        break;
      check_sqlite(database_, step, "list closed process agents");
      insert_event(database_, process_workspace, "agent_closed",
                   column_text(closed_agents.get(), 0), now);
    }
    exec(database_, "COMMIT");
  } catch (...) {
    exec(database_, "ROLLBACK");
    throw;
  }
}

void MailboxStore::register_agent(const AgentRecord &agent) {
  std::scoped_lock lock(mutex_);
  exec(database_, "BEGIN IMMEDIATE");
  try {
    Statement process(database_,
                      "SELECT workspace_id FROM processes WHERE "
                      "process_id=?" +
                          std::string(options_.scope == MailboxScope::workspace
                                          ? " AND workspace_id=?"
                                          : "") +
                          " AND lease_expires_at_ms > ?");
    bind_text(process.get(), 1, agent.process_id);
    int process_index = 2;
    if (options_.scope == MailboxScope::workspace)
      bind_text(process.get(), process_index++, options_.workspace_id);
    bind_integer(process.get(), process_index, agent.started_at_ms);
    const int process_step = sqlite3_step(process.get());
    check_sqlite(database_, process_step, "find owning mailbox process");
    if (process_step != SQLITE_ROW)
      throw MailboxError(MailboxErrorCode::not_found,
                         "owning mailbox process is not live");
    const auto workspace = column_text(process.get(), 0);
    Statement statement(database_, R"sql(
      INSERT INTO agents(agent_id,process_id,kind,owner_agent_id,session_id,
        session_name,task_id,task_path,provider,model_id,status,started_at_ms,closed_at_ms)
      VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
    )sql");
    bind_text(statement.get(), 1, agent.agent_id);
    bind_text(statement.get(), 2, agent.process_id);
    bind_text(statement.get(), 3, agent.kind);
    bind_optional_text(statement.get(), 4, agent.owner_agent_id);
    bind_text(statement.get(), 5, agent.session_id);
    bind_optional_text(statement.get(), 6, agent.session_name);
    bind_optional_text(statement.get(), 7, agent.task_id);
    bind_optional_text(statement.get(), 8, agent.task_path);
    bind_text(statement.get(), 9, agent.provider);
    bind_text(statement.get(), 10, agent.model_id);
    bind_text(statement.get(), 11, agent.status);
    bind_integer(statement.get(), 12, agent.started_at_ms);
    bind_optional_integer(statement.get(), 13, agent.closed_at_ms);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_CONSTRAINT)
      throw MailboxError(MailboxErrorCode::invalid_message,
                         "mailbox agent ID already exists or owner is invalid");
    check_sqlite(database_, result, "register agent");
    insert_event(database_, workspace, "agent_registered", agent.agent_id,
                 agent.started_at_ms);
    exec(database_, "COMMIT");
  } catch (...) {
    exec(database_, "ROLLBACK");
    throw;
  }
}

void MailboxStore::update_agent(const AgentUpdate &update) {
  std::scoped_lock lock(mutex_);
  if (update.workspace_id)
    check_workspace(*update.workspace_id);
  exec(database_, "BEGIN IMMEDIATE");
  try {
    const bool constrain_workspace =
        options_.scope == MailboxScope::workspace || update.workspace_id;
    Statement owner(
        database_,
        "SELECT p.workspace_id FROM agents a JOIN "
        "processes p ON p.process_id=a.process_id WHERE "
        "a.agent_id=?" +
            std::string(constrain_workspace ? " AND p.workspace_id=?" : ""));
    bind_text(owner.get(), 1, update.agent_id);
    if (constrain_workspace)
      bind_text(owner.get(), 2,
                update.workspace_id.value_or(options_.workspace_id));
    const int owner_step = sqlite3_step(owner.get());
    check_sqlite(database_, owner_step, "find mailbox agent");
    if (owner_step != SQLITE_ROW)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox agent not found");
    const auto workspace = column_text(owner.get(), 0);

    std::string sql =
        "UPDATE agents SET session_name=COALESCE(?,session_name), "
        "provider=COALESCE(?,provider), model_id=COALESCE(?,model_id), "
        "status=COALESCE(?,status)";
    if (update.closed_at_ms)
      sql += ", closed_at_ms=?";
    sql += " WHERE agent_id=?";
    Statement statement(database_, sql);
    int index = 1;
    bind_optional_text(statement.get(), index++, update.session_name);
    bind_optional_text(statement.get(), index++, update.provider);
    bind_optional_text(statement.get(), index++, update.model_id);
    bind_optional_text(statement.get(), index++, update.status);
    if (update.closed_at_ms)
      bind_optional_integer(statement.get(), index++, *update.closed_at_ms);
    bind_text(statement.get(), index, update.agent_id);
    check_sqlite(database_, sqlite3_step(statement.get()), "update agent");
    if (sqlite3_changes(database_) == 0)
      throw MailboxError(MailboxErrorCode::not_found,
                         "mailbox agent not found");
    const auto *const event_type = update.status && *update.status == "closed"
                                       ? "agent_closed"
                                       : "agent_updated";
    insert_event(database_, workspace, event_type, update.agent_id,
                 options_.clock());
    exec(database_, "COMMIT");
  } catch (...) {
    exec(database_, "ROLLBACK");
    throw;
  }
}

void MailboxStore::close_agent(std::string_view agent_id, TimestampMs now) {
  update_agent(AgentUpdate{.agent_id = std::string(agent_id),
                           .status = std::string("closed"),
                           .closed_at_ms = std::optional<TimestampMs>{now}});
}

std::vector<AgentRecord> MailboxStore::list_agents(const AgentQuery &query) {
  std::scoped_lock lock(mutex_);
  const auto workspace = query.workspace_id.value_or(options_.workspace_id);
  check_workspace(workspace);
  const auto now = query.now_ms == 0 ? options_.clock() : query.now_ms;
  Statement statement(database_, R"sql(
    SELECT a.agent_id,a.process_id,a.kind,a.owner_agent_id,a.session_id,
      a.session_name,a.task_id,a.task_path,a.provider,a.model_id,a.status,
      a.started_at_ms,a.closed_at_ms,p.workspace_id,p.workspace_path,
      p.last_seen_at_ms,p.lease_expires_at_ms
    FROM agents a JOIN processes p ON p.process_id=a.process_id
    WHERE p.workspace_id=? AND (? OR p.lease_expires_at_ms > ?)
      AND (? OR a.closed_at_ms IS NULL)
      AND (? OR a.session_id=?) AND (? OR a.agent_id=?)
    ORDER BY a.started_at_ms,a.agent_id LIMIT ?
  )sql");
  bind_text(statement.get(), 1, workspace);
  bind_integer(statement.get(), 2, query.include_stale ? 1 : 0);
  bind_integer(statement.get(), 3, now);
  bind_integer(statement.get(), 4, query.include_closed ? 1 : 0);
  bind_integer(statement.get(), 5, query.session_id ? 0 : 1);
  bind_optional_text(statement.get(), 6, query.session_id);
  bind_integer(statement.get(), 7, query.agent_id ? 0 : 1);
  bind_optional_text(statement.get(), 8, query.agent_id);
  bind_integer(statement.get(), 9, static_cast<std::int64_t>(query.limit));
  std::vector<AgentRecord> result;
  while (true) {
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE)
      break;
    check_sqlite(database_, step, "list agents");
    result.push_back(AgentRecord{
        .agent_id = column_text(statement.get(), 0),
        .process_id = column_text(statement.get(), 1),
        .kind = column_text(statement.get(), 2),
        .owner_agent_id = optional_column_text(statement.get(), 3),
        .session_id = column_text(statement.get(), 4),
        .session_name = optional_column_text(statement.get(), 5),
        .task_id = optional_column_text(statement.get(), 6),
        .task_path = optional_column_text(statement.get(), 7),
        .provider = column_text(statement.get(), 8),
        .model_id = column_text(statement.get(), 9),
        .status = column_text(statement.get(), 10),
        .started_at_ms = sqlite3_column_int64(statement.get(), 11),
        .closed_at_ms = optional_column_integer(statement.get(), 12),
        .workspace_id = column_text(statement.get(), 13),
        .workspace_path = column_text(statement.get(), 14),
        .last_seen_at_ms = sqlite3_column_int64(statement.get(), 15),
        .lease_expires_at_ms = sqlite3_column_int64(statement.get(), 16),
    });
  }
  return result;
}

SendReceipt MailboxStore::send(const SendRequest &request) {
  std::scoped_lock lock(mutex_);
  check_workspace(request.workspace_id);
  if (request.sender_agent_id.empty() || request.sender_session_id.empty() ||
      request.body.text.empty() || request.body.text.size() > kMaxTextBytes ||
      request.target.session_id.has_value() ==
          request.target.agent_id.has_value())
    throw MailboxError(MailboxErrorCode::invalid_message,
                       "invalid mailbox send request");
  const auto now =
      request.created_at_ms == 0 ? options_.clock() : request.created_at_ms;
  const auto available =
      request.available_at_ms == 0 ? now : request.available_at_ms;
  exec(database_, "BEGIN IMMEDIATE");
  try {
    std::string recipient_session;
    std::optional<std::string> recipient_agent;
    if (request.target.agent_id) {
      Statement target(database_, R"sql(
      SELECT a.session_id FROM agents a JOIN processes p ON p.process_id=a.process_id
      WHERE a.agent_id=? AND p.workspace_id=? AND a.closed_at_ms IS NULL
        AND p.lease_expires_at_ms > ?
    )sql");
      bind_text(target.get(), 1, *request.target.agent_id);
      bind_text(target.get(), 2, request.workspace_id);
      bind_integer(target.get(), 3, now);
      const int target_step = sqlite3_step(target.get());
      check_sqlite(database_, target_step, "find recipient agent");
      if (target_step != SQLITE_ROW)
        throw MailboxError(MailboxErrorCode::not_found,
                           "recipient agent is not live");
      recipient_session = column_text(target.get(), 0);
      recipient_agent = request.target.agent_id;
    } else {
      recipient_session = *request.target.session_id;
      Statement target(database_, R"sql(
      SELECT a.agent_id FROM agents a JOIN processes p ON p.process_id=a.process_id
      WHERE a.session_id=? AND p.workspace_id=? AND a.closed_at_ms IS NULL
        AND p.lease_expires_at_ms > ? ORDER BY a.agent_id
    )sql");
      bind_text(target.get(), 1, recipient_session);
      bind_text(target.get(), 2, request.workspace_id);
      bind_integer(target.get(), 3, now);
      std::vector<std::string> live;
      while (true) {
        const int step = sqlite3_step(target.get());
        if (step == SQLITE_DONE)
          break;
        check_sqlite(database_, step, "find recipient session");
        live.push_back(column_text(target.get(), 0));
      }
      if (live.size() > 1)
        throw MailboxError(
            MailboxErrorCode::ambiguous_target,
            "session has multiple live activations; specify agent_id");
      if (live.empty()) {
        Statement known(
            database_,
            "SELECT 1 FROM agents WHERE session_id=? AND process_id IN "
            "(SELECT process_id FROM processes WHERE workspace_id=?) LIMIT 1");
        bind_text(known.get(), 1, recipient_session);
        bind_text(known.get(), 2, request.workspace_id);
        const int known_step = sqlite3_step(known.get());
        check_sqlite(database_, known_step, "find known recipient session");
        if (known_step != SQLITE_ROW)
          throw MailboxError(MailboxErrorCode::not_found,
                             "recipient session is unknown");
      }
    }
    const std::string message_id =
        request.message_id.value_or(options_.id_generator());
    const std::string body = body_json(request.body).dump();
    Statement statement(database_, R"sql(
    INSERT INTO messages(message_id,sender_agent_id,sender_session_id,
      recipient_session_id,recipient_agent_id,workspace_id,kind,body_json,
      reply_to_message_id,created_at_ms,available_at_ms)
    VALUES (?,?,?,?,?,?,?,?,?,?,?)
  )sql");
    bind_text(statement.get(), 1, message_id);
    bind_text(statement.get(), 2, request.sender_agent_id);
    bind_text(statement.get(), 3, request.sender_session_id);
    bind_text(statement.get(), 4, recipient_session);
    bind_optional_text(statement.get(), 5, recipient_agent);
    bind_text(statement.get(), 6, request.workspace_id);
    bind_text(statement.get(), 7, mailbox_message_kind_to_string(request.kind));
    bind_text(statement.get(), 8, body);
    bind_optional_text(statement.get(), 9, request.reply_to_message_id);
    bind_integer(statement.get(), 10, now);
    bind_integer(statement.get(), 11, available);
    const int result = sqlite3_step(statement.get());
    if (result == SQLITE_CONSTRAINT)
      throw MailboxError(MailboxErrorCode::invalid_message,
                         "mailbox message ID already exists");
    check_sqlite(database_, result, "send mailbox message");
    insert_event(database_, request.workspace_id, "message", message_id, now);
    exec(database_, "COMMIT");
    return SendReceipt{.message_id = message_id,
                       .recipient_session_id = recipient_session,
                       .recipient_agent_id = recipient_agent,
                       .created_at_ms = now};
  } catch (...) {
    exec(database_, "ROLLBACK");
    throw;
  }
}

std::vector<MailboxMessage> MailboxStore::inspect(const InboxQuery &query) {
  std::scoped_lock lock(mutex_);
  const auto workspace = query.workspace_id.value_or(options_.workspace_id);
  check_workspace(workspace);
  const auto now = query.now_ms == 0 ? options_.clock() : query.now_ms;
  Statement statement(
      database_,
      "SELECT " + std::string(kMessageColumns) +
          " FROM messages WHERE recipient_session_id=? AND workspace_id=? "
          "AND available_at_ms<=? AND (? OR acknowledged_at_ms IS NULL) "
          "AND (? OR message_id=?) ORDER BY created_at_ms,message_id LIMIT ?");
  bind_text(statement.get(), 1, query.session_id);
  bind_text(statement.get(), 2, workspace);
  bind_integer(statement.get(), 3, now);
  bind_integer(statement.get(), 4, query.include_acknowledged ? 1 : 0);
  bind_integer(statement.get(), 5, query.message_id ? 0 : 1);
  bind_optional_text(statement.get(), 6, query.message_id);
  bind_integer(statement.get(), 7, static_cast<std::int64_t>(query.limit));
  std::vector<MailboxMessage> result;
  while (true) {
    const int step = sqlite3_step(statement.get());
    if (step == SQLITE_DONE)
      break;
    check_sqlite(database_, step, "inspect mailbox");
    auto message = read_message(statement.get());
    if ((!query.agent_id || !message.recipient_agent_id ||
         *query.agent_id == *message.recipient_agent_id) &&
        contains_kind(query.kinds, message.kind))
      result.push_back(std::move(message));
  }
  return result;
}

ClaimResult MailboxStore::claim(const ClaimRequest &request) {
  std::scoped_lock lock(mutex_);
  const auto workspace = request.workspace_id.value_or(options_.workspace_id);
  check_workspace(workspace);
  const auto now = request.now_ms == 0 ? options_.clock() : request.now_ms;
  const auto lease =
      request.lease_ms <= 0 ? options_.claim_lease_ms : request.lease_ms;
  exec(database_, "BEGIN IMMEDIATE");
  try {
    Statement agent(database_,
                    "SELECT 1 FROM agents a JOIN processes p ON "
                    "p.process_id=a.process_id WHERE a.agent_id=? "
                    "AND a.session_id=? AND p.workspace_id=? AND "
                    "a.closed_at_ms IS NULL AND p.lease_expires_at_ms>?");
    bind_text(agent.get(), 1, request.agent_id);
    bind_text(agent.get(), 2, request.session_id);
    bind_text(agent.get(), 3, workspace);
    bind_integer(agent.get(), 4, now);
    const int agent_step = sqlite3_step(agent.get());
    check_sqlite(database_, agent_step, "validate claiming mailbox agent");
    if (agent_step != SQLITE_ROW)
      throw MailboxError(MailboxErrorCode::not_found,
                         "claiming mailbox agent is not live");
    Statement query(
        database_,
        "SELECT " + std::string(kMessageColumns) +
            " FROM messages WHERE recipient_session_id=? AND workspace_id=? "
            "AND available_at_ms<=? AND acknowledged_at_ms IS NULL "
            "AND (claim_expires_at_ms IS NULL OR claim_expires_at_ms<=?) "
            "ORDER BY created_at_ms,message_id");
    bind_text(query.get(), 1, request.session_id);
    bind_text(query.get(), 2, workspace);
    bind_integer(query.get(), 3, now);
    bind_integer(query.get(), 4, now);
    std::vector<MailboxMessage> candidates;
    while (true) {
      const int step = sqlite3_step(query.get());
      if (step == SQLITE_DONE)
        break;
      check_sqlite(database_, step, "find claimable mailbox messages");
      auto message = read_message(query.get());
      if ((!message.recipient_agent_id ||
           *message.recipient_agent_id == request.agent_id) &&
          contains_kind(request.kinds, message.kind))
        candidates.push_back(std::move(message));
      if (candidates.size() >= request.limit)
        break;
    }
    ClaimResult result;
    for (auto &message : candidates) {
      const std::string token = options_.id_generator();
      Statement update(database_,
                       "UPDATE messages SET claim_agent_id=?,claim_token=?,"
                       "claim_expires_at_ms=? WHERE message_id=? AND "
                       "acknowledged_at_ms IS NULL AND (claim_expires_at_ms IS "
                       "NULL OR claim_expires_at_ms<=?) AND workspace_id=?");
      bind_text(update.get(), 1, request.agent_id);
      bind_text(update.get(), 2, token);
      bind_integer(update.get(), 3, now + lease);
      bind_text(update.get(), 4, message.message_id);
      bind_integer(update.get(), 5, now);
      bind_text(update.get(), 6, workspace);
      check_sqlite(database_, sqlite3_step(update.get()),
                   "claim mailbox message");
      if (sqlite3_changes(database_) == 0)
        continue;
      message.claim_agent_id = request.agent_id;
      message.claim_token = token;
      message.claim_expires_at_ms = now + lease;
      insert_event(database_, workspace, "message_claimed", message.message_id,
                   now);
      result.messages.push_back(std::move(message));
    }
    exec(database_, "COMMIT");
    return result;
  } catch (...) {
    exec(database_, "ROLLBACK");
    throw;
  }
}

void MailboxStore::acknowledge(const AcknowledgeRequest &request) {
  std::scoped_lock lock(mutex_);
  const auto workspace = request.workspace_id.value_or(options_.workspace_id);
  check_workspace(workspace);
  exec(database_, "BEGIN IMMEDIATE");
  try {
    Statement statement(
        database_,
        "UPDATE messages SET acknowledged_at_ms=? WHERE message_id=? "
        "AND claim_agent_id=? AND claim_token=? AND workspace_id=? "
        "AND acknowledged_at_ms IS NULL AND (claim_expires_at_ms IS NULL OR "
        "claim_expires_at_ms>?)");
    const auto now = request.now_ms == 0 ? options_.clock() : request.now_ms;
    bind_integer(statement.get(), 1, now);
    bind_text(statement.get(), 2, request.message_id);
    bind_text(statement.get(), 3, request.agent_id);
    bind_text(statement.get(), 4, request.claim_token);
    bind_text(statement.get(), 5, workspace);
    bind_integer(statement.get(), 6, now);
    check_sqlite(database_, sqlite3_step(statement.get()),
                 "acknowledge mailbox message");
    if (sqlite3_changes(database_) != 0) {
      insert_event(database_, workspace, "message_acknowledged",
                   request.message_id, now);
      exec(database_, "COMMIT");
      return;
    }
    Statement already(
        database_,
        "SELECT acknowledged_at_ms FROM messages WHERE message_id=? AND "
        "claim_agent_id=? AND claim_token=? AND workspace_id=?");
    bind_text(already.get(), 1, request.message_id);
    bind_text(already.get(), 2, request.agent_id);
    bind_text(already.get(), 3, request.claim_token);
    bind_text(already.get(), 4, workspace);
    const int already_step = sqlite3_step(already.get());
    check_sqlite(database_, already_step, "check acknowledged mailbox message");
    if (already_step == SQLITE_ROW &&
        sqlite3_column_type(already.get(), 0) != SQLITE_NULL) {
      exec(database_, "COMMIT");
      return;
    }
    throw MailboxError(MailboxErrorCode::invalid_claim,
                       "mailbox claim token is invalid or expired");
  } catch (...) {
    exec(database_, "ROLLBACK");
    throw;
  }
}

WaitResult MailboxStore::wait_for_change(
    const WaitRequest &request,
    std::stop_token
        stop_token) { // NOLINT(performance-unnecessary-value-param,misc-include-cleaner)
  std::int64_t timeout =
      std::clamp<std::int64_t>(request.timeout_ms, 0, 60'000);
  const auto interval =
      std::clamp<std::int64_t>(request.poll_interval_ms, 1, 5'000);
  const auto start = options_.clock();
  while (true) {
    {
      std::scoped_lock lock(mutex_);
      check_workspace(request.workspace_id);
      Statement statement(
          database_,
          "SELECT generation,event_type FROM mailbox_events "
          "WHERE workspace_id=? AND generation>? ORDER BY generation");
      bind_text(statement.get(), 1, request.workspace_id);
      bind_integer(statement.get(), 2, request.after_generation);
      std::int64_t generation = request.after_generation;
      bool presence_changed = false;
      bool messages_changed = false;
      while (true) {
        const int step = sqlite3_step(statement.get());
        if (step == SQLITE_DONE)
          break;
        check_sqlite(database_, step, "read mailbox events");
        generation = sqlite3_column_int64(statement.get(), 0);
        const auto event_type = column_text(statement.get(), 1);
        presence_changed = presence_changed ||
                           event_type.starts_with("process_") ||
                           event_type.starts_with("agent_");
        messages_changed = messages_changed ||
                           event_type.starts_with("message") ||
                           event_type == "cleanup";
      }
      if (generation > request.after_generation)
        return WaitResult{.generation = generation,
                          .presence_changed = presence_changed,
                          .messages_changed = messages_changed};
    }
    const auto elapsed = options_.clock() - start;
    if (stop_token.stop_requested() || elapsed >= timeout)
      return WaitResult{.timed_out = true};
    std::this_thread::sleep_for(std::chrono::milliseconds(
        std::min<std::int64_t>(interval, timeout - elapsed)));
  }
}

MailboxStatus MailboxStore::status(const StatusRequest &request) {
  std::scoped_lock lock(mutex_);
  check_workspace(request.workspace_id);
  Statement version(database_, "PRAGMA user_version");
  check_sqlite(database_, sqlite3_step(version.get()), "read schema version");
  Statement agents(database_, "SELECT COUNT(*) FROM agents a JOIN processes p "
                              "ON p.process_id=a.process_id "
                              "WHERE p.workspace_id=? AND a.closed_at_ms IS "
                              "NULL AND p.lease_expires_at_ms>?");
  bind_text(agents.get(), 1, request.workspace_id);
  bind_integer(agents.get(), 2, options_.clock());
  check_sqlite(database_, sqlite3_step(agents.get()), "read agent status");
  Statement messages(database_,
                     "SELECT COUNT(*) FROM messages WHERE workspace_id=? AND "
                     "acknowledged_at_ms IS NULL");
  bind_text(messages.get(), 1, request.workspace_id);
  check_sqlite(database_, sqlite3_step(messages.get()), "read message status");
  Statement generation(database_, "SELECT COALESCE(MAX(generation),0) FROM "
                                  "mailbox_events WHERE workspace_id=?");
  bind_text(generation.get(), 1, request.workspace_id);
  check_sqlite(database_, sqlite3_step(generation.get()), "read event status");
  return MailboxStatus{
      .schema_version = sqlite3_column_int64(version.get(), 0),
      .workspace_id = request.workspace_id,
      .live_agents =
          static_cast<std::size_t>(sqlite3_column_int64(agents.get(), 0)),
      .unread_messages =
          static_cast<std::size_t>(sqlite3_column_int64(messages.get(), 0)),
      .latest_generation = sqlite3_column_int64(generation.get(), 0)};
}

CleanupResult MailboxStore::cleanup(const CleanupRequest &request) {
  std::scoped_lock lock(mutex_);
  const auto workspace = request.workspace_id.value_or(options_.workspace_id);
  check_workspace(workspace);
  const auto now = request.now_ms == 0 ? options_.clock() : request.now_ms;
  const auto message_cutoff = now - request.acknowledged_retention_ms;
  const auto stale_cutoff = now - request.stale_retention_ms;
  CleanupResult result;
  exec(database_, "BEGIN IMMEDIATE");
  try {
    Statement messages(database_,
                       "DELETE FROM messages WHERE acknowledged_at_ms "
                       "IS NOT NULL AND acknowledged_at_ms<? AND "
                       "workspace_id=?");
    bind_integer(messages.get(), 1, message_cutoff);
    bind_text(messages.get(), 2, workspace);
    check_sqlite(database_, sqlite3_step(messages.get()), "cleanup messages");
    result.messages_removed =
        static_cast<std::size_t>(sqlite3_changes(database_));
    while (true) {
      Statement agent(database_,
                      "SELECT a.agent_id FROM agents a JOIN processes p "
                      "ON p.process_id=a.process_id WHERE "
                      "p.workspace_id=? AND a.closed_at_ms IS NOT NULL "
                      "AND a.closed_at_ms<? AND NOT EXISTS (SELECT 1 "
                      "FROM agents child WHERE child.owner_agent_id="
                      "a.agent_id) LIMIT 1");
      bind_text(agent.get(), 1, workspace);
      bind_integer(agent.get(), 2, stale_cutoff);
      const int agent_step = sqlite3_step(agent.get());
      check_sqlite(database_, agent_step, "find cleanup agent");
      if (agent_step == SQLITE_DONE)
        break;
      const auto agent_id = column_text(agent.get(), 0);
      Statement remove(database_, "DELETE FROM agents WHERE agent_id=?");
      bind_text(remove.get(), 1, agent_id);
      check_sqlite(database_, sqlite3_step(remove.get()), "cleanup agent");
      result.agents_removed +=
          static_cast<std::size_t>(sqlite3_changes(database_));
    }
    while (true) {
      Statement process(database_,
                        "SELECT p.process_id FROM processes p WHERE "
                        "p.workspace_id=? AND p.closed_at_ms IS NOT NULL "
                        "AND p.closed_at_ms<? AND NOT EXISTS (SELECT 1 "
                        "FROM agents a WHERE a.process_id=p.process_id) "
                        "LIMIT 1");
      bind_text(process.get(), 1, workspace);
      bind_integer(process.get(), 2, stale_cutoff);
      const int process_step = sqlite3_step(process.get());
      check_sqlite(database_, process_step, "find cleanup process");
      if (process_step == SQLITE_DONE)
        break;
      const auto process_id = column_text(process.get(), 0);
      Statement remove(database_, "DELETE FROM processes WHERE process_id=?");
      bind_text(remove.get(), 1, process_id);
      check_sqlite(database_, sqlite3_step(remove.get()), "cleanup process");
      result.processes_removed +=
          static_cast<std::size_t>(sqlite3_changes(database_));
    }
    Statement events(database_,
                     "DELETE FROM mailbox_events WHERE created_at_ms<? "
                     "AND workspace_id=?");
    bind_integer(events.get(), 1, message_cutoff);
    bind_text(events.get(), 2, workspace);
    check_sqlite(database_, sqlite3_step(events.get()), "cleanup events");
    result.events_removed =
        static_cast<std::size_t>(sqlite3_changes(database_));
    if (result.messages_removed != 0 || result.agents_removed != 0 ||
        result.processes_removed != 0 || result.events_removed != 0)
      insert_event(database_, workspace, "cleanup", workspace, now);
    exec(database_, "COMMIT");
    return result;
  } catch (...) {
    exec(database_, "ROLLBACK");
    throw;
  }
}

} // namespace pi::core
