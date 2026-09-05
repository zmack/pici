#pragma once

// McpStdioTransport: owns one persistent MCP server child process for its
// whole lifetime, communicating over stdio with newline-delimited JSON-RPC
// 2.0 (see mcp_protocol.h for the envelope types and the inbound-message
// discriminator).
//
// This is deliberately NOT modeled on BashTool's one-shot fork/exec/capture
// pattern in builtin_tools.cpp, even though it borrows that pattern's
// self-pipe-wakeup + std::stop_callback idiom for its poll loops: BashTool
// spawns a child, waits for it to finish, and captures output once. This
// transport spawns a child that runs indefinitely, exchanges many
// request/response pairs (and unsolicited notifications, and even
// server-initiated requests) with it concurrently from multiple caller
// threads, and must shut it down cleanly and promptly without orphaning it.
//
// Process-creation choice: the design doc considered posix_spawn (to avoid
// async-signal-safety hazards from doing string/container work between
// fork() and exec()), but posix_spawn_file_actions_t has no way to run
// arbitrary child-side code -- and PR_SET_PDEATHSIG (see below) MUST run in
// the child before its own exec. So this uses a plain fork(), but all
// argv/envp construction happens in the PARENT before forking; the child
// between fork() and execve() only calls documented async-signal-safe
// functions (dup2, close, prctl, getppid, chdir, execve, _exit) -- this
// preserves posix_spawn's safety property by construction rather than by
// using posix_spawn itself.
//
// PR_SET_PDEATHSIG is REQUIRED, not optional: main.cpp's SIGTERM handler is
// `std::exit(0)`, which skips ~McpStdioTransport (and every other automatic
// object's destructor) entirely. Without PDEATHSIG, a normal Ctrl+C/kill
// quit would orphan every running MCP server. PDEATHSIG is delivered when
// the *thread* that called prctl exits/dies -- since that's the same
// forked child process before its execve, this is exactly the calling
// thread whose exit we care about (the process exiting via signal or
// std::exit both count). The classic fork-race (parent already exited
// between fork() returning and the child's prctl() call) is handled by
// re-checking getppid() immediately after and self-terminating if it
// changed.

#include "core/mcp/mcp_protocol.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace pi::core::mcp {

struct McpServerProcessConfig {
  std::string command;
  std::vector<std::string> args;
  // Phase 1's fixed policy (Phase 2's config layer is what will make this
  // user-facing/documented): the child inherits pici's own environment,
  // then these entries are overlaid on top (present keys override, new
  // keys are added). There is currently no way to request a fully-scrubbed
  // environment for a server -- a documented non-goal for Phase 1.
  std::map<std::string, std::string> env;
  std::optional<std::string> cwd;
};

using NotificationCallback = std::function<void(const std::string &method,
                                                const nlohmann::json &params)>;

// Thrown for any transport-level failure: dead/crashed child, EPIPE on
// write, malformed handshake, call timeout, etc. Deliberately a single
// error type at this API boundary rather than letting raw errno failures,
// std::future_error, or other exception types leak out of call()/notify().
class TransportError : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class McpStdioTransport {
public:
  explicit McpStdioTransport(McpServerProcessConfig config);
  ~McpStdioTransport();

  McpStdioTransport(const McpStdioTransport &) = delete;
  McpStdioTransport &operator=(const McpStdioTransport &) = delete;
  McpStdioTransport(McpStdioTransport &&) = delete;
  McpStdioTransport &operator=(McpStdioTransport &&) = delete;

  // Sends a request, waits up to `timeout` for the matching response, and
  // returns its "result" field. Throws TransportError on: a dead transport
  // (EPIPE, or the reader having already observed the child gone), a
  // malformed/absent id, a JSON-RPC error response, or a timeout. Safe to
  // call concurrently from multiple threads -- the write path is
  // serialized and each call gets its own request id / pending-map slot.
  nlohmann::json call(const std::string &method, nlohmann::json params,
                      std::chrono::milliseconds timeout);

  // Same as above, but also races the wait against `stop_tok`: if it is
  // stopped before the timeout elapses or a response arrives, this
  // best-effort sends `notifications/cancelled` (carrying the request id
  // this call minted) and returns promptly with a TransportError instead of
  // waiting out the rest of `timeout` -- the cooperative-cancellation path
  // for McpTool::execute()'s own stop_tok (see mcp_tool.cpp).
  //
  // A default-constructed (stop-incapable) std::stop_token makes
  // stop_tok.stop_possible() false, in which case this takes the exact same
  // single future::wait_for(timeout) path as the two-argument overload
  // above -- no polling-loop latency added for callers that pass one. The
  // two-argument overload is implemented in terms of this one, passing
  // std::stop_token{}.
  nlohmann::json call(const std::string &method, nlohmann::json params,
                      std::chrono::milliseconds timeout,
                      std::stop_token stop_tok);

  // Sends a notification (no id, no reply expected, no pending-map entry).
  // Throws TransportError on a dead transport, same as call().
  void notify(const std::string &method, nlohmann::json params);

  void set_notification_callback(NotificationCallback cb);

  // True if the child appears to still be running. Never blocks: uses
  // waitpid(..., WNOHANG) and caches the reaped status once observed, so
  // repeated/concurrent calls (including one racing the destructor's own
  // final reap) never re-issue waitpid on an already-reaped pid (which
  // would return ECHILD and could otherwise be mistaken for a new signal).
  bool is_alive() const;

  pid_t pid() const { return pid_; }

  // The tail (last kMaxStderrBufferBytes) of everything the server has
  // written to its own stderr, captured by stderr_loop's drain thread since
  // this transport was constructed. Returns whatever has been captured so
  // far even while the child is still running -- there is no "final"
  // snapshot distinct from this. Intended for surfacing on a failed call
  // (see McpTool::execute()), never printed directly to pici's own stderr.
  std::string recent_stderr() const;

  // Testing/diagnostic hook: the request id the NEXT call() will use.
  // Exists so tests can deliberately construct an id collision between a
  // server-initiated request and a client request, to prove the
  // parse_inbound_message discriminator doesn't cross-wire them (see
  // test_mcp_transport.cpp's
  // HandlesServerInitiatedRequestWithMethodAndNotFoundReply). Not intended
  // for production call sites.
  std::int64_t peek_next_request_id_for_testing() const {
    return next_id_.load();
  }

private:
  struct Pending {
    std::promise<nlohmann::json> promise;
  };

  void spawn(const McpServerProcessConfig &config);
  void write_message(const nlohmann::json &message);
  void reader_loop(const std::stop_token &stop_tok);
  void stderr_loop(const std::stop_token &stop_tok);
  // Appends `len` freshly-read bytes to `buffer`, dispatches each complete
  // (newline-terminated) line to handle_line, and reports whether the
  // still-unterminated remainder now exceeds kMaxUnterminatedLineBytes.
  // Factored out of reader_loop purely to keep that function's poll/error
  // handling readable; not meaningful to call outside that loop.
  bool consume_stdout_chunk(std::string &buffer, const char *data,
                            std::size_t len);
  void handle_line(const std::string &line);
  void handle_response(RequestId id, nlohmann::json response_obj);
  void fail_all_pending(const std::string &reason);
  void reap_blocking();

  McpServerProcessConfig config_;
  pid_t pid_{-1};

  int stdin_fd_{-1};  // parent's write end of the child's stdin
  int stdout_fd_{-1}; // parent's read end of the child's stdout
  int stderr_fd_{-1}; // parent's read end of the child's stderr

  int reader_wakeup_read_{-1};
  int reader_wakeup_write_{-1};
  int stderr_wakeup_read_{-1};
  int stderr_wakeup_write_{-1};

  // Serializes writers: multiple threads may call() concurrently.
  std::mutex write_mutex_;

  std::mutex pending_mutex_;
  std::map<std::int64_t, std::shared_ptr<Pending>> pending_;
  std::atomic<std::int64_t> next_id_{1};

  std::mutex notification_mutex_;
  NotificationCallback notification_callback_;

  mutable std::mutex status_mutex_;
  mutable bool exited_{false};
  mutable int exit_status_{0};

  mutable std::mutex stderr_buffer_mutex_;
  std::string stderr_buffer_; // capped tail; exposed via recent_stderr()

  // Set once the reader loop stops for any reason (EOF/POLLHUP, an
  // oversized unterminated line, or shutdown). A fast, purely local
  // fail-fast for subsequent call()/notify() attempts -- distinct from (and
  // in addition to) the EPIPE path, which only fires once a write is
  // actually attempted against a pipe nobody is reading from anymore.
  std::atomic<bool> reader_dead_{false};

  // Declared LAST: the real ordering guarantee is the explicit
  // request_stop()+join() sequence at the top of ~McpStdioTransport()
  // (before any fd is closed); declaring the jthreads last backs that up
  // structurally, since member destructors run in reverse declaration
  // order -- these threads are joined (via std::jthread's own destructor)
  // before any earlier-declared member's destructor runs, matching
  // BashTool's stop_callback/wakeup-pipe pattern in builtin_tools.cpp.
  std::jthread stderr_thread_;
  std::jthread reader_thread_;
};

} // namespace pi::core::mcp
