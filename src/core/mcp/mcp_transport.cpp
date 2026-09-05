#include "core/mcp/mcp_transport.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <type_traits>
#include <unistd.h> // declares `environ` under glibc's default feature-test macros
#include <utility>

namespace pi::core::mcp {

namespace {

constexpr std::size_t kMaxUnterminatedLineBytes = 32ULL * 1024 * 1024; // 32MB
constexpr std::size_t kMaxStderrBufferBytes = 8192;
constexpr auto kShutdownGraceCap = std::chrono::milliseconds(2000);
constexpr auto kSigtermGrace = std::chrono::milliseconds(300);
constexpr auto kPollInterval = std::chrono::milliseconds(20);

bool write_all_best_effort(int fd, const char *data, std::size_t len) {
  std::size_t offset = 0;
  while (offset < len) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    const auto n = ::write(fd, data + offset, len - offset);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    if (n == 0)
      return false;
    offset += static_cast<std::size_t>(n);
  }
  return true;
}

// Resolves `command` to an executable path without spawning anything, so a
// missing command throws synchronously from the constructor instead of only
// manifesting later as an async fork+exec failure. Mirrors
// sandbox.cpp's executable_in_path, but returns the resolved path (needed
// for execve, which -- unlike execvp -- does not itself search PATH).
std::optional<std::string> resolve_executable(const std::string &command) {
  if (command.find('/') != std::string::npos) {
    if (::access(command.c_str(), X_OK) == 0)
      return command;
    return std::nullopt;
  }
  const char *path_env = std::getenv("PATH"); // NOLINT(concurrency-mt-unsafe)
  if (path_env == nullptr || *path_env == '\0')
    return std::nullopt;

  const std::string path(path_env);
  std::size_t start = 0;
  while (start <= path.size()) {
    const auto end = path.find(':', start);
    const auto dir = path.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    auto candidate = (dir.empty() ? std::string(".") : dir) + "/" + command;
    if (::access(candidate.c_str(), X_OK) == 0)
      return candidate;
    if (end == std::string::npos)
      break;
    start = end + 1;
  }
  return std::nullopt;
}

} // namespace

McpStdioTransport::McpStdioTransport(McpServerProcessConfig config)
    : config_(std::move(config)) {
  spawn(config_);

  // Started last, after the child is fully spawned and stdin_/stdout_/
  // stderr_fd_ are populated, so both loops can safely use them the moment
  // they start running.
  stderr_thread_ =
      std::jthread([this](const std::stop_token &st) { stderr_loop(st); });
  reader_thread_ =
      std::jthread([this](const std::stop_token &st) { reader_loop(st); });
}

McpStdioTransport::~McpStdioTransport() {
  // (1) Signal stop. Each loop registered a std::stop_callback against its
  // own jthread's stop_token that writes to that loop's wakeup pipe --
  // request_stop() runs that callback synchronously on THIS thread, which
  // is what actually unblocks a loop currently parked in poll().
  reader_thread_.request_stop();
  stderr_thread_.request_stop();

  // (2) Join both threads before touching any fd. Explicit here (not just
  // relying on the jthread members' own destructors later) so the ordering
  // is guaranteed regardless of destructor-body edits; the member
  // declaration order backs this up structurally too (see mcp_transport.h).
  if (reader_thread_.joinable())
    reader_thread_.join();
  if (stderr_thread_.joinable())
    stderr_thread_.join();

  // (3)/(4) Close the pipe fds, stdin first: many MCP servers treat stdin
  // EOF as a graceful-shutdown signal. This now actually works -- pipe2's
  // O_CLOEXEC means pici's own copy of the write end is the only one that
  // can possibly be open (no other child this process ever spawns can have
  // inherited it), so closing it here is guaranteed to deliver EOF to the
  // server rather than silently doing nothing because some unrelated
  // process still holds a duplicate write end open.
  if (stdin_fd_ >= 0) {
    ::close(stdin_fd_);
    stdin_fd_ = -1;
  }
  if (stdout_fd_ >= 0) {
    ::close(stdout_fd_);
    stdout_fd_ = -1;
  }
  if (stderr_fd_ >= 0) {
    ::close(stderr_fd_);
    stderr_fd_ = -1;
  }
  if (reader_wakeup_read_ >= 0)
    ::close(reader_wakeup_read_);
  if (reader_wakeup_write_ >= 0)
    ::close(reader_wakeup_write_);
  if (stderr_wakeup_read_ >= 0)
    ::close(stderr_wakeup_read_);
  if (stderr_wakeup_write_ >= 0)
    ::close(stderr_wakeup_write_);

  // (5) Poll is_alive() with a ~2s total cap -- not a fixed short sleep:
  // real node/python MCP servers can take longer than BashTool's 200ms
  // grace period to exit cleanly after stdin EOF.
  if (pid_ > 0) {
    const auto deadline = std::chrono::steady_clock::now() + kShutdownGraceCap;
    while (is_alive() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(kPollInterval);

    // (6) SIGTERM if still alive.
    if (is_alive()) {
      ::kill(pid_, SIGTERM); // NOLINT(misc-include-cleaner)
      const auto term_deadline =
          std::chrono::steady_clock::now() + kSigtermGrace;
      while (is_alive() && std::chrono::steady_clock::now() < term_deadline)
        std::this_thread::sleep_for(kPollInterval);
    }

    // (7) SIGKILL if still alive after the grace period.
    if (is_alive())
      ::kill(pid_, SIGKILL); // NOLINT(misc-include-cleaner)

    // (8) Final waitpid, tolerant of ECHILD (something else already reaped
    // it -- e.g. is_alive() itself, moments ago).
    reap_blocking();
  }
}

void McpStdioTransport::spawn(const McpServerProcessConfig &config) {
  const auto resolved = resolve_executable(config.command);
  if (!resolved)
    throw std::runtime_error("mcp: command not found: " + config.command);

  // pipe2(..., O_CLOEXEC) -- NOT plain pipe() -- is load-bearing: without
  // it, these fds would leak into every OTHER child this pici process ever
  // spawns afterward (another MCP server, a BashTool invocation), which
  // independently breaks "close stdin to signal shutdown" for THIS server
  // (some unrelated child would still hold a write end open). The dup2
  // calls in the child below clear O_CLOEXEC on exactly the three fds that
  // become 0/1/2 in that child; everything else stays close-on-exec.
  std::array<int, 2> in_pipe{-1, -1};  // child reads [0], parent writes [1]
  std::array<int, 2> out_pipe{-1, -1}; // child writes [1], parent reads [0]
  std::array<int, 2> err_pipe{-1, -1}; // child writes [1], parent reads [0]

  auto make_pipe = [](std::array<int, 2> &fds) {
    if (::pipe2(fds.data(), O_CLOEXEC) != 0)
      throw std::runtime_error(
          "mcp: failed to create pipe: " +
          std::string(std::strerror(errno))); // NOLINT(concurrency-mt-unsafe)
  };
  make_pipe(in_pipe);
  make_pipe(out_pipe);
  make_pipe(err_pipe);

  // Build argv/envp entirely in the parent, before forking: the child may
  // only call async-signal-safe functions between fork() and execve()
  // (dup2, close, prctl, getppid, chdir, execve, _exit -- no allocation, no
  // std::string/std::vector construction), so all of that work happens
  // here instead.
  std::vector<std::string> argv_storage;
  argv_storage.reserve(config.args.size() + 1);
  argv_storage.push_back(config.command);
  argv_storage.insert(argv_storage.end(), config.args.begin(),
                      config.args.end());
  std::vector<char *> argv;
  argv.reserve(argv_storage.size() + 1);
  for (auto &arg : argv_storage)
    argv.push_back(arg.data());
  argv.push_back(nullptr);

  // Env policy (see McpServerProcessConfig::env's doc comment): inherit
  // pici's own environment, then overlay `config.env` on top.
  std::map<std::string, std::string> merged_env;
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
  for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string kv(*entry);
    const auto eq = kv.find('=');
    if (eq == std::string::npos)
      continue;
    merged_env.emplace(kv.substr(0, eq), kv.substr(eq + 1));
  }
  for (const auto &[key, value] : config.env)
    merged_env[key] = value;

  std::vector<std::string> envp_storage;
  envp_storage.reserve(merged_env.size());
  for (const auto &[key, value] : merged_env) {
    std::string entry_str = key;
    entry_str += '=';
    entry_str += value;
    envp_storage.push_back(std::move(entry_str));
  }
  std::vector<char *> envp;
  envp.reserve(envp_storage.size() + 1);
  for (auto &entry : envp_storage)
    envp.push_back(entry.data());
  envp.push_back(nullptr);

  const std::string cwd_storage = config.cwd.value_or(std::string{});
  const std::string &exec_path = *resolved;
  const pid_t parent_pid_before_fork = ::getpid();

  const pid_t pid = ::fork();
  if (pid < 0) {
    const int saved_errno = errno;
    for (int fd : {in_pipe[0], in_pipe[1], out_pipe[0], out_pipe[1],
                   err_pipe[0], err_pipe[1]})
      ::close(fd);
    throw std::runtime_error(
        "mcp: fork failed: " +
        std::string(
            std::strerror(saved_errno))); // NOLINT(concurrency-mt-unsafe)
  }

  if (pid == 0) {
    // Child. Only async-signal-safe calls until execve (or _exit on any
    // failure path).
    ::dup2(in_pipe[0], STDIN_FILENO);
    ::dup2(out_pipe[1], STDOUT_FILENO);
    ::dup2(err_pipe[1], STDERR_FILENO);
    ::close(in_pipe[0]);
    ::close(in_pipe[1]);
    ::close(out_pipe[0]);
    ::close(out_pipe[1]);
    ::close(err_pipe[0]);
    ::close(err_pipe[1]);

    // REQUIRED: main.cpp's SIGTERM handler is `std::exit(0)`, which skips
    // ~McpStdioTransport (and every other destructor) entirely, so this is
    // what actually prevents an orphaned server on a normal quit.
    ::prctl(PR_SET_PDEATHSIG, SIGTERM); // NOLINT(misc-include-cleaner)
    // Fork race: the parent may have already exited between fork()
    // returning here and the prctl() call just above -- in which case this
    // process was already reparented (getppid() changed) before PDEATHSIG
    // was armed, and PDEATHSIG can no longer save it. Self-terminate
    // immediately rather than running on as an orphan.
    if (::getppid() != parent_pid_before_fork)
      ::_exit(1);

    if (!cwd_storage.empty() && ::chdir(cwd_storage.c_str()) != 0)
      ::_exit(126);

    ::execve(exec_path.c_str(), argv.data(), envp.data());
    ::_exit(127); // execve only returns on failure
  }

  // Parent.
  ::close(in_pipe[0]);
  ::close(out_pipe[1]);
  ::close(err_pipe[1]);

  pid_ = pid;
  stdin_fd_ = in_pipe[1];
  stdout_fd_ = out_pipe[0];
  stderr_fd_ = err_pipe[0];

  // NOLINTNEXTLINE(misc-include-cleaner): POSIX fcntl declarations vary.
  ::fcntl(stdout_fd_, F_SETFL, O_NONBLOCK);
  ::fcntl(stderr_fd_, F_SETFL, O_NONBLOCK);

  std::array<int, 2> reader_wakeup{-1, -1};
  std::array<int, 2> stderr_wakeup{-1, -1};
  if (::pipe2(reader_wakeup.data(), O_CLOEXEC) != 0 ||
      ::pipe2(stderr_wakeup.data(), O_CLOEXEC) != 0) {
    const int saved_errno = errno;
    ::kill(pid_, SIGKILL); // NOLINT(misc-include-cleaner)
    int status = 0;
    ::waitpid(pid_, &status, 0);
    throw std::runtime_error(
        "mcp: failed to create wakeup pipe: " +
        std::string(
            std::strerror(saved_errno))); // NOLINT(concurrency-mt-unsafe)
  }
  reader_wakeup_read_ = reader_wakeup[0];
  reader_wakeup_write_ = reader_wakeup[1];
  stderr_wakeup_read_ = stderr_wakeup[0];
  stderr_wakeup_write_ = stderr_wakeup[1];
}

void McpStdioTransport::write_message(const nlohmann::json &message) {
  if (reader_dead_.load())
    throw TransportError("mcp transport is dead");

  const std::string line = message.dump() + "\n";
  std::lock_guard<std::mutex> lock(write_mutex_);
  if (stdin_fd_ < 0)
    throw TransportError("mcp transport stdin is closed");

  std::size_t offset = 0;
  while (offset < line.size()) {
    const auto n =
        ::write( // NOLINT(cppcoreguidelines-pro-bounds-pointer-arithmetic)
            stdin_fd_, line.data() + offset, line.size() - offset);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      // With SIGPIPE globally ignored (see main.cpp/acp/main.cpp), a write
      // to a dead child's pipe fails with EPIPE instead of killing this
      // process -- exactly the case this branch turns into a clean error.
      throw TransportError(
          std::string("mcp transport write failed: ") +
          std::strerror(errno)); // NOLINT(concurrency-mt-unsafe)
    }
    offset += static_cast<std::size_t>(n);
  }
}

nlohmann::json McpStdioTransport::call(const std::string &method,
                                       nlohmann::json params,
                                       std::chrono::milliseconds timeout) {
  const auto id = next_id_.fetch_add(1);
  auto pending = std::make_shared<Pending>();
  auto future = pending->promise.get_future();
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.emplace(id, pending);
  }

  const nlohmann::json request{
      {"jsonrpc", "2.0"},
      {"id", id},
      {"method", method},
      {"params", std::move(params)},
  };

  try {
    write_message(request);
  } catch (...) {
    // The write never reached the server (or the transport was already
    // known dead) -- nobody will ever answer this id, so remove our own
    // registration and propagate the write failure directly.
    std::lock_guard<std::mutex> lock(pending_mutex_);
    pending_.erase(id);
    throw;
  }

  if (future.wait_for(timeout) == std::future_status::timeout) {
    // Extract-and-erase under the lock, symmetric with handle_response's
    // own extract-then-act: whichever side (this timeout, or the reader
    // thread matching a response) wins the race to erase this id owns
    // deciding what happens to the promise.
    std::shared_ptr<Pending> extracted;
    {
      std::lock_guard<std::mutex> lock(pending_mutex_);
      auto it = pending_.find(id);
      if (it != pending_.end()) {
        extracted = it->second;
        pending_.erase(it);
      }
    }
    if (extracted) {
      // We won: the promise is still unfulfilled and nobody else holds a
      // reference to it (handle_response won't find this id anymore, and
      // will just drop the late response if one arrives). Report a clean
      // timeout without ever calling future.get() -- the promise is never
      // going to be fulfilled, so get() would block forever, not throw.
      throw TransportError("mcp call timed out: " + method);
    }
    // The reader thread already extracted this id moments before our
    // timeout fired: it holds the other shared_ptr to this same Pending
    // (our local `pending` keeps it alive too) and is in the middle of
    // calling set_value/set_exception on it. Fall through to future.get()
    // below, which will return/throw whatever the reader ends up setting,
    // instead of reporting a spurious second timeout.
  }

  try {
    const auto response = future.get();
    if (response.contains("error")) {
      const auto &error = response.at("error");
      throw TransportError(
          "mcp error calling " + method + ": " +
          error.value("message", std::string{"unknown error"}));
    }
    if (response.contains("result"))
      return response.at("result");
    return nlohmann::json::object();
  } catch (const std::future_error &err) {
    // Only reachable if the promise was destroyed without ever having a
    // value/exception set on it -- e.g. this transport was torn down (or
    // its reader thread exited) with this call still in flight. Translate
    // to TransportError rather than letting std::future_error (an
    // unexpected type at this API boundary) escape.
    throw TransportError(
        std::string("mcp transport closed before response to ") + method +
        ": " + err.what());
  }
}

void McpStdioTransport::notify(const std::string &method,
                               nlohmann::json params) {
  const nlohmann::json note{
      {"jsonrpc", "2.0"},
      {"method", method},
      {"params", std::move(params)},
  };
  write_message(note);
}

void McpStdioTransport::set_notification_callback(NotificationCallback cb) {
  std::lock_guard<std::mutex> lock(notification_mutex_);
  notification_callback_ = std::move(cb);
}

bool McpStdioTransport::is_alive() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  if (exited_)
    return false;
  if (pid_ <= 0)
    return false;

  int status = 0;
  const pid_t result = ::waitpid(pid_, &status, WNOHANG);
  if (result == 0)
    return true; // still running
  if (result == pid_) {
    exited_ = true;
    exit_status_ = status;
    return false;
  }
  if (result < 0 && errno == ECHILD) {
    // Something else already reaped it (e.g. a concurrent call racing the
    // destructor's own final waitpid) -- tolerate this as "not alive"
    // rather than treating it as an error.
    exited_ = true;
    return false;
  }
  // Transient (e.g. EINTR): report alive conservatively; the next call
  // retries the waitpid.
  return true;
}

void McpStdioTransport::reap_blocking() {
  std::lock_guard<std::mutex> lock(status_mutex_);
  if (exited_ || pid_ <= 0)
    return;
  int status = 0;
  const pid_t result = ::waitpid(pid_, &status, 0);
  if (result == pid_)
    exit_status_ = status;
  // ECHILD here just means something else (e.g. is_alive()) already reaped
  // it -- either way, the child is gone.
  exited_ = true;
}

void McpStdioTransport::handle_response(RequestId id,
                                        nlohmann::json response_obj) {
  std::shared_ptr<Pending> pending;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    auto it = pending_.find(id);
    if (it != pending_.end()) {
      pending = it->second;
      pending_.erase(it);
    }
  }
  if (!pending) {
    // Either no such id was ever registered, or a timed-out caller already
    // won the race and erased it first -- either way, a late/unmatched
    // response is dropped, not treated as an error.
    return;
  }
  try {
    pending->promise.set_value(std::move(response_obj));
  } catch (const std::future_error &) { // NOLINT(bugprone-empty-catch)
    // Defensive only: each id is extracted at most once (under the lock
    // above), so set_value on an already-satisfied promise should be
    // unreachable. Never let a promise-API misuse escape the reader thread.
  }
}

void McpStdioTransport::fail_all_pending(const std::string &reason) {
  std::vector<std::shared_ptr<Pending>> all;
  {
    std::lock_guard<std::mutex> lock(pending_mutex_);
    all.reserve(pending_.size());
    for (auto &[id, entry] : pending_)
      all.push_back(entry);
    pending_.clear();
  }
  for (auto &entry : all) {
    try {
      entry->promise.set_exception(
          std::make_exception_ptr(TransportError(reason)));
    } catch (const std::future_error &) { // NOLINT(bugprone-empty-catch)
      // Defensive only, same reasoning as handle_response's catch above.
    }
  }
}

void McpStdioTransport::handle_line(const std::string &line) {
  const auto parsed = nlohmann::json::parse(line, nullptr, false);
  if (parsed.is_discarded())
    return; // non-JSON banner line from a real-world server -- skip, not fatal

  const auto discriminated = parse_inbound_message(parsed);
  if (!discriminated)
    return; // unrecognized shape -- drop

  std::visit(
      [&](auto &&value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, JsonRpcResponse>) {
          handle_response(value.id, parsed);
        } else if constexpr (std::is_same_v<T, JsonRpcNotification>) {
          NotificationCallback callback;
          {
            std::lock_guard<std::mutex> lock(notification_mutex_);
            callback = notification_callback_;
          }
          if (callback)
            callback(value.method, value.params);
        } else if constexpr (std::is_same_v<T, InboundServerRequest>) {
          // Phase 1 doesn't implement sampling/roots/elicitation -- reply
          // with a stub "method not found" so a spec-compliant server
          // doesn't hang waiting for a response it will never otherwise
          // get.
          const nlohmann::json reply{
              {"jsonrpc", "2.0"},
              {"id", value.id},
              {"error", {{"code", -32601}, {"message", "Method not found"}}},
          };
          try {
            write_message(reply);
          } catch (...) { // NOLINT(bugprone-empty-catch)
            // Best-effort: if the transport is already dead the reply
            // can't be delivered anyway.
          }
        }
      },
      *discriminated);
}

bool McpStdioTransport::consume_stdout_chunk(std::string &buffer,
                                             const char *data,
                                             std::size_t len) {
  buffer.append(data, len);

  std::size_t newline_pos = 0;
  while ((newline_pos = buffer.find('\n')) != std::string::npos) {
    const std::string line = buffer.substr(0, newline_pos);
    buffer.erase(0, newline_pos + 1);
    try {
      handle_line(line);
    } catch (...) { // NOLINT(bugprone-empty-catch)
      // An exception escaping a jthread's function is std::terminate -- this
      // reader must survive a malformed/unexpected message, not bring down
      // the whole process over one bad line.
    }
  }

  return buffer.size() > kMaxUnterminatedLineBytes;
}

void McpStdioTransport::reader_loop(const std::stop_token &stop_tok) {
  // Mirrors BashTool's self-pipe-wakeup pattern (builtin_tools.cpp): the
  // stop_callback fires synchronously on whatever thread calls
  // request_stop(), writing into the wakeup pipe to unblock poll()
  // immediately rather than waiting for the next scheduled wakeup.
  std::stop_callback wakeup(stop_tok, [this] {
    const char byte = 1;
    write_all_best_effort(reader_wakeup_write_, &byte, 1);
  });

  std::string buffer;
  std::array<char, 65536> chunk{};
  bool oversized = false;

  while (!stop_tok.stop_requested()) {
    std::array<struct pollfd, 2> fds = {{
        {.fd = stdout_fd_, .events = POLLIN, .revents = 0},
        {.fd = reader_wakeup_read_, .events = POLLIN, .revents = 0},
    }};
    const int n = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), -1);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if ((fds[1].revents & POLLIN) != 0)
      break; // stop requested

    if ((fds[0].revents & POLLIN) != 0) {
      const auto nr = ::read(stdout_fd_, chunk.data(), chunk.size());
      if (nr < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
          continue;
        break;
      }
      if (nr == 0)
        break; // EOF: child closed stdout

      if (consume_stdout_chunk(buffer, chunk.data(),
                               static_cast<std::size_t>(nr))) {
        oversized = true;
        break;
      }
    } else if ((fds[0].revents & (POLLHUP | POLLERR)) != 0) {
      break;
    }
  }

  if (oversized) {
    // The server is producing an unbounded line with no newline in sight;
    // treat the transport as dead outright rather than growing the buffer
    // further or continuing to poll a child nobody is meaningfully reading
    // from anymore. Best-effort nudge so it doesn't just sit there.
    if (pid_ > 0)
      ::kill(pid_, SIGTERM); // NOLINT(misc-include-cleaner)
  }

  // Whatever the exit reason (EOF/POLLHUP, oversized line, or a requested
  // stop), nobody will read anything more from this child. Mark the
  // transport dead so subsequent call()/notify() attempts fail fast
  // instead of writing into a pipe nobody drains, and fail any calls still
  // waiting on a response that will now never arrive.
  reader_dead_.store(true);
  fail_all_pending("mcp transport reader stopped (server exited or crashed)");
}

void McpStdioTransport::stderr_loop(const std::stop_token &stop_tok) {
  std::stop_callback wakeup(stop_tok, [this] {
    const char byte = 1;
    write_all_best_effort(stderr_wakeup_write_, &byte, 1);
  });

  std::array<char, 4096> chunk{};
  while (!stop_tok.stop_requested()) {
    std::array<struct pollfd, 2> fds = {{
        {.fd = stderr_fd_, .events = POLLIN, .revents = 0},
        {.fd = stderr_wakeup_read_, .events = POLLIN, .revents = 0},
    }};
    const int n = ::poll(fds.data(), static_cast<nfds_t>(fds.size()), -1);
    if (n < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if ((fds[1].revents & POLLIN) != 0)
      break;

    if ((fds[0].revents & POLLIN) != 0) {
      const auto nr = ::read(stderr_fd_, chunk.data(), chunk.size());
      if (nr < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
          continue;
        break;
      }
      if (nr == 0)
        break;

      std::lock_guard<std::mutex> lock(stderr_buffer_mutex_);
      stderr_buffer_.append(chunk.data(), static_cast<std::size_t>(nr));
      if (stderr_buffer_.size() > kMaxStderrBufferBytes)
        stderr_buffer_.erase(0, stderr_buffer_.size() - kMaxStderrBufferBytes);
    } else if ((fds[0].revents & (POLLHUP | POLLERR)) != 0) {
      break;
    }
  }
}

} // namespace pi::core::mcp
