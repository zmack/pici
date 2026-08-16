# Remote-Controlled Faux Client: Renderer Content Testing

> **Audience note:** this plan is written to be executed directly, including
> by a smaller/faster model than the one that wrote it. Every integration
> point below cites the exact existing file and pattern to copy — do not
> invent alternatives to these patterns; the codebase already has working
> precedent for every piece except the JSON schema and the two new classes
> described below. Read a cited file before writing the code that touches it.

## Goal

Add a way to drive a real `pi` process — real `AgentSession`, real
`agent_loop.cpp` (including genuine parallel tool dispatch), real renderer
(`--render region`, `viewport`, etc.) — using **scripted, remote-controlled
content** instead of a real model and real tools. An external process (a
test harness, a shell script, a person with `nc`) connects to a Unix domain
socket, sends line-delimited JSON describing assistant text, thinking, and
tool calls (including their streaming partial output and final result), and
watches the real renderer paint it. No network, no API key, no real shell
commands. This is how you verify turn rendering, markdown, and — especially
— concurrent tool-call regions, without needing a live model session.

This reuses two pieces of infrastructure that already exist and already do
almost exactly this, just not remotely:

- `core::FauxClient` (`src/core/providers/faux.h`, `.cpp`) — an `LLMClient`
  that replays a pre-built list of `AssistantMessageEvent`s instead of
  calling a real API.
- `core::LLMClientRegistry` (`src/core/llm_client.h:79-98`) — a global,
  keyed factory registry (same shape as `StreamRendererRegistry`) that lets
  a `Model.api` string resolve to an arbitrary `LLMClient`, including a faux
  one. `test/test_rpc_mode.cpp:1-75` is a complete, working example of
  registering a faux client this way and running a real `AgentSession`
  against it — **read that file before starting**, it is the template for
  the session-construction half of this work.

## Architecture summary

```
 external process (test harness / nc / python script)
        │  Unix domain socket, one JSON object per line
        ▼
 FauxControlMode (new, mirrors cli::RpcMode)
        │  compiles each "round" into a FauxClient::Script,
        │  registers each tool_call's scripted behavior
        ▼
 RemoteFauxClient (new, LLMClient)  ──registered in──▶ LLMClientRegistry
        │  stream() blocks for the next queued Script
        ▼
 real core::AgentSession / core::Agent / agent_loop.cpp
        │  real execute_tool_calls_parallel, real std::async tool threads
        ▼
 ScriptedTool (new, ToolDefinition) × N   ──registered in place of real tools──
        │  execute() replays the scripted updates/result for its call_id
        ▼
 real Renderer (RegionRenderer, ViewportRenderer, ...) via dispatch_event
```

Nothing downstream of `RemoteFauxClient`/`ScriptedTool` is new — the whole
point is that `agent_loop.cpp`, `dispatch_event`, and every renderer run
completely unmodified and unaware anything is fake.

## Part 1 — `RemoteFauxClient` and the JSON→`Script` compiler

### New files: `src/core/providers/faux_control.h`, `src/core/providers/faux_control.cpp`

Add both to `CMakeLists.txt` next to wherever `src/core/providers/faux.cpp`
is currently listed (grep the file for `providers/faux.cpp` to find the
right target/list).

#### Data types

```cpp
// faux_control.h
#pragma once
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/providers/faux.h"
#include <condition_variable>
#include <deque>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pi::core {

struct ScriptedToolUpdate {
  int after_ms{0};       // delay from tool start, not from previous update
  std::string partial;   // passed to ToolExecutionContext.on_update
};

struct ScriptedToolBehavior {
  std::vector<ScriptedToolUpdate> updates;  // in ascending after_ms order
  std::string result_content;
  bool is_error{false};
  int finish_after_ms{0};                   // total time before returning
};

// Thread-safe call_id -> behavior lookup shared between the JSON compiler
// (which populates it) and every ScriptedTool instance (which consumes it).
class ScriptedToolRegistry {
public:
  void register_behavior(std::string call_id, ScriptedToolBehavior behavior);
  // Removes and returns the behavior so a call_id can't accidentally be
  // replayed twice; std::nullopt if no behavior was registered for call_id
  // (the ScriptedTool must return an error ToolResult in that case, not
  // block or crash).
  std::optional<ScriptedToolBehavior> take_behavior(const std::string &call_id);

private:
  std::mutex mutex_;
  std::unordered_map<std::string, ScriptedToolBehavior> behaviors_;
};

// LLMClient that serves FauxClient::Script objects pushed to it from the
// control-mode thread instead of a fixed vector supplied at construction.
class RemoteFauxClient : public LLMClient {
public:
  void push_round(FauxClient::Script script);
  // Wakes any thread blocked in stream() with an error AssistantMessage.
  // Call this when the control connection closes.
  void close();

  std::shared_ptr<AssistantMessage> stream(const Model &model,
                                           const AgentContext &context,
                                           const StreamOptions &options,
                                           AssistantEventCallback on_event,
                                           std::stop_token stop_tok) override;
  std::string_view provider_name() const override { return "faux-control"; }
  std::string_view api_id() const override { return "faux-control"; }

private:
  std::mutex mutex_;
  std::condition_variable_any cv_;   // condition_variable_any: needed for the
                                     // stop_token-aware wait overload below
  std::deque<FauxClient::Script> queue_;
  bool closed_{false};
};

// Parses one JSON "round" object (see schema below) into a FauxClient::Script
// and registers any tool_call behaviors it contains into `registry`.
// Returns std::nullopt and fills `error` if the JSON is malformed.
std::optional<FauxClient::Script>
compile_round(const nlohmann::json &round, ScriptedToolRegistry &registry,
              std::string &error);

} // namespace pi::core
```

#### `RemoteFauxClient::stream()`

Model this on `FauxClient::stream()` (`src/core/providers/faux.cpp:20-84`) —
same event-replay loop, same "no more content" error-`AssistantMessage`
shape on the terminating condition — but pop the `Script` from `queue_`
instead of indexing a fixed vector, and block (cancellably) if the queue is
empty:

```cpp
std::shared_ptr<AssistantMessage>
RemoteFauxClient::stream(const Model &model, const AgentContext &,
                         const StreamOptions &, AssistantEventCallback on_event,
                         std::stop_token stop_tok) {
  FauxClient::Script script;
  {
    std::unique_lock lock(mutex_);
    // condition_variable_any::wait(lock, stop_token, predicate) returns
    // false if the stop_token fired before the predicate became true —
    // exactly the "connection closed / cancelled" case.
    const bool got_one = cv_.wait(lock, stop_tok, [&] {
      return !queue_.empty() || closed_;
    });
    if (!got_one || queue_.empty()) {
      // Mirror FauxClient's "No more faux scripts queued" error message
      // shape exactly (src/core/providers/faux.cpp:36-46) so callers that
      // already handle that error path (if any) keep working.
      auto msg = std::make_shared<AssistantMessage>();
      msg->api = "faux-control";
      msg->provider = "faux-control";
      msg->model = model.id;
      msg->stop_reason = StopReason::error;
      msg->error_message = "No more faux-control rounds queued";
      if (on_event)
        on_event(AssistantMessageErrorEvent{.reason = StopReason::error,
                                            .error = *msg});
      return msg;
    }
    script = std::move(queue_.front());
    queue_.pop_front();
  }
  // Replay exactly like FauxClient::stream()'s event loop
  // (src/core/providers/faux.cpp:52-76): for each event in script.events,
  // call on_event(ev), track the terminal Done/Error event into final_msg,
  // sleep script.delay_between if set. Copy that loop here — it is ~20
  // lines and duplicating it is preferable to modifying FauxClient's
  // signature to support both modes.
}
```

`push_round`/`close` just lock, mutate, `cv_.notify_all()`.

#### JSON schema for one "round"

A **round** is one call to the model — text/thinking/tool-call content plus
a stop reason. A **turn** (see Part 2) consists of one or more rounds:
`agent_loop.cpp` calls `stream()` again automatically whenever a round's
`stop_reason` means "I called tools, give me the results and continue" —
nothing new to build there, that's `Agent`'s existing behavior once tool
results are appended to context.

```jsonc
{
  "type": "round",
  "delay_between_ms": 15,           // optional; paced between each emitted event
  "stop_reason": "tool_calls",      // "tool_calls" | "end_turn"
  "content": [
    {"type": "text", "text": "Let me check that test.\n"},
    {"type": "thinking", "text": "considering the mutex..."},
    {
      "type": "tool_call",
      "call_id": "call_1",          // must be unique per process lifetime
      "name": "bash",               // must match a name a ScriptedTool was
                                     // registered under (see Part 3)
      "args": {"command": "pytest -k flaky"},
      "updates": [
        {"after_ms": 100, "partial": "collecting...\n"},
        {"after_ms": 250, "partial": "collecting...\n2 passed, 1 failed\n"}
      ],
      "result": {"content": "2 passed, 1 failed", "is_error": false},
      "finish_after_ms": 400
    }
  ]
}
```

`compile_round` must:

1. For each `content` entry, in order, build the matching
   `AssistantMessageEvent`s and append a matching `ContentBlock` to a
   `std::vector<ContentBlock>` being assembled for the final message:
   - `"text"` → `AssistantMessageTextStartEvent{content_index, partial}`,
     `AssistantMessageTextDeltaEvent{content_index, text, partial}`,
     `AssistantMessageTextEndEvent{content_index, text, partial}`; append
     `TextContent{.text = text}`.
   - `"thinking"` → same shape with
     `AssistantMessageThinking{Start,Delta,End}Event`; append
     `ThinkingContent{.text = text}` (check `message_types.h` for the exact
     field name on `ThinkingContent` before writing this — it may not be
     `.text`).
   - `"tool_call"` → build a `ToolCall{.id = call_id, .name = name,
     .arguments = args, .partial_json = args.dump()}` (check
     `message_types.h`'s `ToolCall` struct for exact field names — `test/
     test_agent_loop.cpp:2153-2200`'s `test_streaming_tool_call()` is a
     complete working example of constructing this event sequence
     end-to-end; mirror it exactly), emit
     `AssistantMessageToolCallStartEvent`,
     `AssistantMessageToolCallDeltaEvent`, `AssistantMessageToolCallEndEvent`;
     append the `ToolCall` as a `ContentBlock`. **Also** call
     `registry.register_behavior(call_id, ScriptedToolBehavior{...})` with
     the `updates`/`result`/`finish_after_ms` from this JSON object — this
     is what lets the real tool-dispatch machinery replay it later.
   `content_index` is the position of this block within `content` (0-based),
   matching how real providers number them — check `message_types.h`'s
   `AssistantMessageTextStartEvent` etc. for the exact field.
2. Build the terminal `AssistantMessage`: `api = "faux-control"`,
   `provider = "faux-control"`, `model = <whatever model.id is at call
   time — the compiler doesn't have this, so leave it empty; RemoteFauxClient
   fills msg->model after compiling, or thread the model id through>`,
   `content = <the vector built above>`, `stop_reason = StopReason::tool_use`
   if `"stop_reason":"tool_calls"` else `StopReason::stop` (check
   `event_types.h`/`message_types.h` for the exact `StopReason` enumerator
   names — do not guess; `test_streaming_tool_call` again shows the correct
   one for a tool-calling response).
3. Emit `AssistantMessageDoneEvent{reason, message}` as the final script
   event.
4. Return `FauxClient::Script{.events = <all events built above>,
   .delay_between = round.value("delay_between_ms", 0) > 0 ?
   optional<milliseconds>{...} : nullopt}`.

If any required field is missing (`type`, tool call missing `call_id`/
`name`), return `std::nullopt` and set `error` to a human-readable message —
the caller (`FauxControlMode::handle`) turns that into an error response
sent back over the socket, it must never throw or crash the process on bad
input from the harness.

## Part 2 — `ScriptedTool`: real tool dispatch, scripted behavior

### Add to `src/core/providers/faux_control.h`/`.cpp`

```cpp
class ScriptedTool : public ToolDefinition {
public:
  ScriptedTool(std::string name, std::shared_ptr<ScriptedToolRegistry> registry);

  std::string_view name() const override { return name_; }
  std::string_view description() const override {
    return "Remote-controlled scripted tool (faux-control mode)";
  }
  ToolSchema &schema() const override;   // see below
  std::shared_ptr<ToolResult>
  execute(std::string_view args_json, ToolExecutionContext context) const override;

private:
  std::string name_;
  std::shared_ptr<ScriptedToolRegistry> registry_;
};
```

`schema()`: needs to return a `ToolSchema&` that accepts any JSON object
(the real validation already happened conceptually when the harness wrote
the JSON — this tool doesn't need to re-validate). Write a small
`PermissiveToolSchema : public ToolSchema` in this file whose
`serialize()` returns a minimal valid JSON-schema string (e.g.
`{"type":"object"}`), `to_definition()` returns an empty map, and
`validate_arguments()` uses the base class's default (always succeeds,
`message_types.h:178-182`). Store one static instance per `ScriptedTool` (a
function-local `static PermissiveToolSchema instance;` returned by
reference is fine — schemas are stateless).

`execute()`:

```cpp
std::shared_ptr<ToolResult>
ScriptedTool::execute(std::string_view, ToolExecutionContext context) const {
  auto behavior = registry_->take_behavior(std::string(context.call_id));
  if (!behavior) {
    return std::make_shared<ScriptedToolResult>(
        /*content=*/"faux-control: no scripted behavior registered for call_id \"" +
            std::string(context.call_id) + "\"",
        /*is_error=*/true);
  }
  const auto start = std::chrono::steady_clock::now();
  for (const auto &update : behavior->updates) {
    const auto target = start + std::chrono::milliseconds(update.after_ms);
    if (context.stop_token.stop_requested())
      return std::make_shared<ScriptedToolResult>("cancelled", true);
    std::this_thread::sleep_until(target);   // simple sleep is fine here —
                                              // this is a test harness, not
                                              // the production paint loop
    if (context.on_update)
      context.on_update(std::make_shared<ScriptedToolResult>(update.partial, false));
  }
  const auto finish_target =
      start + std::chrono::milliseconds(behavior->finish_after_ms);
  if (std::chrono::steady_clock::now() < finish_target &&
      !context.stop_token.stop_requested())
    std::this_thread::sleep_until(finish_target);
  return std::make_shared<ScriptedToolResult>(behavior->result_content,
                                              behavior->is_error);
}
```

`ScriptedToolResult` is a small local `ToolResult` subclass — there is no
existing reusable one (`StaticToolResult` in `agent_loop.cpp:131` and
`TextToolResult` in `builtin_tools.cpp:52` are both file-local/anonymous-
namespace, not exported). Implement the four virtuals from
`message_types.h:185-199`: `is_error()` returns the stored bool,
`content()` returns the stored string, `details()` returns `std::nullopt`,
`terminate()` uses the base default (`false`).

## Part 3 — `FauxControlMode`: the socket + JSONL command loop

### New files: `src/cli/faux_control_mode.h`, `src/cli/faux_control_mode.cpp`

Mirror `src/cli/rpc_mode.h`/`.cpp` closely — **read both files in full
before writing this one**, the shape (a `handle(json)` dispatcher, an
`Output` callback typedef, a `std::jthread` per in-flight run) is exactly
what's needed here too, just driving `RemoteFauxClient`/`AgentSession`
instead of real prompts.

```cpp
// faux_control_mode.h
#pragma once
#include "core/providers/faux_control.h"
#include "core/session/agent_session.h"
#include <atomic>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <thread>

namespace pi::cli {

class FauxControlMode {
public:
  using Output = std::function<void(const nlohmann::json &)>;

  FauxControlMode(core::AgentSession &session, core::RemoteFauxClient &client,
                  std::shared_ptr<core::ScriptedToolRegistry> tool_registry,
                  Output output);
  ~FauxControlMode();
  FauxControlMode(const FauxControlMode &) = delete;
  FauxControlMode &operator=(const FauxControlMode &) = delete;

  void handle(const nlohmann::json &command);
  void wait_for_idle();
  void stop();

private:
  void emit(const nlohmann::json &value) const;
  void response(const nlohmann::json &command, bool success,
                nlohmann::json data = nullptr, std::string error = {}) const;
  void handle_round(const nlohmann::json &command);
  void handle_turn(const nlohmann::json &command);

  core::AgentSession &session_;
  core::RemoteFauxClient &client_;
  std::shared_ptr<core::ScriptedToolRegistry> tool_registry_;
  Output output_;
  mutable std::mutex output_mutex_;
  std::atomic<bool> run_active_{false};
  std::jthread run_thread_;
};

// Listens on a Unix domain socket at `socket_path`, accepting one connection
// at a time. For each connection, reads newline-delimited JSON commands and
// dispatches them via FauxControlMode::handle, writing FauxControlMode's
// output back as newline-delimited JSON on the same connection. Returns when
// the process should exit (a "quit" command was received).
int run_faux_control_socket(core::AgentSession &session,
                            core::RemoteFauxClient &client,
                            std::shared_ptr<core::ScriptedToolRegistry> tool_registry,
                            const std::string &socket_path);

} // namespace pi::cli
```

### `handle()` dispatch

Three command `"type"` values, matching `RpcMode::handle`'s style
(`src/cli/rpc_mode.cpp:250` onward — read it for the exact `if/else`
dispatch-on-`"type"` pattern and copy it):

- `"round"` → `handle_round`: call `core::compile_round(command,
  *tool_registry_, error)`; on success `client_.push_round(*script)` and
  `response(command, true)`; on failure `response(command, false, nullptr,
  error)`.
- `"turn"` → `handle_turn`: refuse with an error response if
  `run_active_.exchange(true)` was already `true` (mirror
  `RpcMode::start_prompt`'s reentrancy guard, `rpc_mode.cpp:198-203`). Join
  any previous `run_thread_` if joinable, then spawn a new
  `std::jthread` that calls `session_.run_prompt("", [this](const
  core::AgentEvent &event) { emit(core::event_to_json(event)); })` —
  streaming every real `AgentEvent` back over the socket as JSON (via the
  existing `core::event_to_json`, already used identically in
  `rpc_mode.cpp:210`) gives the harness a structured, parseable record of
  exactly what happened, on top of whatever is visible on the terminal.
  On completion emit `{"type":"turn.completed"}` (or `"turn.failed"` with
  the error, mirroring `rpc_mode.cpp:212-215`) and reset `run_active_`.
- `"quit"` → respond, then arrange for `run_faux_control_socket`'s loop to
  return (e.g. an `std::atomic<bool> should_quit_` flag checked by the
  accept loop after each connection closes, or closing the listening socket
  fd directly to unblock `accept()`).

### Socket loop (`run_faux_control_socket`)

POSIX raw sockets — this codebase has no socket abstraction library, so
write this directly against `<sys/socket.h>`, `<sys/un.h>`, `<unistd.h>`
(the same headers `AltScreenSession`/`terminal.cpp` already use for raw
POSIX calls, so this is consistent with existing style):

1. `::unlink(socket_path.c_str())` first (clean up a stale socket file from
   a previous crashed run — `bind` fails on `EADDRINUSE` otherwise).
2. `::socket(AF_UNIX, SOCK_STREAM, 0)`, build a `sockaddr_un` with
   `sun_path = socket_path`, `::bind`, `::listen(fd, 1)`.
3. Loop: `::accept(fd, ...)` one connection at a time (a second connection
   attempt naturally queues at the OS level and gets accepted after the
   first disconnects — good enough for a test harness driving one demo
   session at a time; no need for concurrent-connection handling).
4. Per connection: read into a buffer, split on `'\n'` into complete lines
   (small helper: keep a `std::string tail_` across `read()` calls, append
   new bytes, repeatedly find `'\n'` and extract). Reuse the *exact* parse-
   and-dispatch pattern from `run_rpc_mode` (`rpc_mode.cpp:546-570`): empty
   lines skipped, `nlohmann::json::parse(line, nullptr, false)`, on
   `.is_discarded()` write an inline error JSON line back, else
   `mode.handle(parsed)`.
5. `Output` for this connection: write `value.dump() + "\n"` to the
   connected fd via `::write` (loop-and-retry on short writes/`EINTR`, same
   as `write_all` in `src/core/region_renderer.cpp:264-278` — copy that
   helper or a version of it here rather than trusting a single `::write`
   call to flush everything).
6. On `read()` returning `0` or `< 0` (peer closed / error): call
   `client.close()` (unblocks any `RemoteFauxClient::stream()` currently
   waiting, so a stuck `run_prompt` doesn't hang forever), close the fd,
   loop back to `accept()` — unless a `"quit"` was received, in which case
   return.
7. On the way out: `::close(fd)`, `::unlink(socket_path.c_str())`.

## Part 4 — Wiring into `main.cpp`

### `src/cli/args.h`

Add near the other output/testing flags (around line 51,
`std::string render;`):

```cpp
std::string faux_control_socket; // --faux-control <path>: serve a JSONL
                                 // control socket instead of a real model
```

### `src/cli/args.cpp`

Find the parse site for a comparable single-string-value flag (e.g.
`--session-dir`, search for `"--session-dir"` in this file) and copy its
`if (arg == "--faux-control" && i + 1 < argc) { result.faux_control_socket
= argv[++i]; }`-shaped block.

### `src/main.cpp`

The exact place to branch is right where `resolve_model`/`registry`/`opts`/
`runtime` (the `AgentSession`) get built — `main.cpp:847` (`resolve_model`
call) through `main.cpp:1090` (`AgentSession runtime(...)` construction).
When `!args.faux_control_socket.empty()`:

1. **Skip real model resolution.** Instead of `resolve_model(args,
   registry)`, build a synthetic `Model`/`ProviderConfig`/`ModelRegistry`
   exactly like `test/test_rpc_mode.cpp:52-68` does: `provider.api =
   "faux-control"`, `model.api = "faux-control"`, `model.provider =
   "faux-control"`.
2. **Register the client.** Before constructing `AgentSession`:
   ```cpp
   auto remote_client = std::make_shared<core::RemoteFauxClient>();
   core::LLMClientRegistry::instance().register_client(
       "faux-control", [remote_client] { return remote_client; });
   ```
   (Mirrors `test_rpc_mode.cpp:45-48`'s `register_client` call exactly,
   except the lambda captures and returns the *same* shared instance every
   time — needed so the queue persists across every `stream()` call within
   the process, not a fresh empty client per call.)
3. **Build `opts`/`runtime` as normal** (the existing code at
   `main.cpp:916-1092` is model-agnostic — it doesn't need to change,
   just make sure `opts.model` is the synthetic faux-control model from
   step 1).
4. **Replace tool registration.** Skip the real
   `core::create_all_tools(...)` block (`main.cpp:1095-1111`) entirely when
   `faux_control_socket` is set. Instead, register one `core::ScriptedTool`
   per name the harness might reference. MVP: a fixed list covering the
   common builtin tool names (grep `create_all_tools`'s implementation for
   the exact set — likely `bash`, `read_file`, `write_file`, `edit_file`,
   `grep`, `glob`, or similar) plus accept any name — actually, since a
   `ScriptedTool` is just a name-forwarding shim, the simplest correct
   approach is: **don't pre-register a fixed list at all.** Instead, have
   `compile_round` (Part 1) also return the *set of tool names* it
   referenced, and in `main.cpp`, after `LLMClientRegistry` registration,
   register a small closure with `agent.add_tool(...)` lazily the first
   time `FauxControlMode::handle_round` sees a new tool name — i.e.
   `FauxControlMode` needs a reference to `agent.state()` (or an
   `add_tool_if_missing(name)` callback threaded in through its
   constructor) to call `agent.add_tool(std::make_shared<ScriptedTool>(name,
   tool_registry_))` the first time that name appears in a `"tool_call"`
   block, tracked via a local `std::set<std::string> known_tool_names_`
   member on `FauxControlMode`. This avoids hardcoding a tool-name list
   that will inevitably drift from whatever names a test script wants to
   use.
5. **Branch to the new mode**, after `runtime`/`agent` are fully set up
   (same place `main.cpp:1632-1634` branches to `run_rpc_mode`):
   ```cpp
   if (!args.faux_control_socket.empty()) {
     return cli::run_faux_control_socket(runtime, *remote_client,
                                         tool_registry, args.faux_control_socket);
   }
   ```

## Part 5 — Testing

### `test/test_faux_control.cpp` (new; add to `CMakeLists.txt` test list
next to `test_faux_client.cpp`/`test_rpc_mode.cpp`)

No socket needed for these — test the compiler and the tool directly:

1. `compile_round` on a JSON round with one `"text"` block → assert the
   resulting `Script.events` has exactly
   `{Start?, TextStart, TextDelta, TextEnd, Done}` in that shape (adjust
   once you confirm whether an explicit `AssistantMessageStartEvent` is
   required — check whether `FauxClient::stream()` or `agent_loop.cpp`
   requires one before the first content event; `test_streaming_tool_call`
   in `test_agent_loop.cpp:2153` is again the reference for what a real
   sequence needs) and that `Done`'s `message.content` contains one
   `TextContent` matching the input text.
2. `compile_round` on a JSON round with a `"tool_call"` block → assert the
   `Done` event's `message.content` contains a `ToolCall` with the right
   `id`/`name`/`arguments`, **and** that `ScriptedToolRegistry::take_behavior`
   returns the matching `updates`/`result`/`is_error` for that `call_id`
   afterward.
3. `compile_round` on malformed input (missing `"call_id"` on a tool_call,
   unknown `"type"`) → assert `std::nullopt` is returned with a non-empty
   `error`, not a crash/throw.
4. `ScriptedTool::execute()` directly: register a behavior with two
   `updates` and a `finish_after_ms`, call `execute()` with a matching
   `ToolExecutionContext` whose `on_update` pushes into a
   `std::vector<std::string>`, assert both partials arrived in order before
   the final result, and assert wall-clock time taken is close to
   `finish_after_ms` (loose bound, e.g. within 100ms, to avoid CI flakiness
   — this is timing-sensitive test code, keep the scripted delays small,
   tens of milliseconds, not seconds).
5. `ScriptedTool::execute()` with **no** registered behavior for the given
   `call_id` → assert an error `ToolResult` is returned, not a hang or
   crash.
6. `RemoteFauxClient`: push one `Script`, call `stream()`, assert it
   returns that script's terminal message. Then call `stream()` again with
   an empty queue and a `std::stop_source` that's cancelled from another
   thread shortly after — assert `stream()` returns promptly (bounded test
   timeout) with the "No more faux-control rounds queued"/error shape,
   proving it doesn't hang forever on shutdown.

### Manual verification

Build `pi-cli`, run:

```sh
pi --render region --faux-control /tmp/pici-demo.sock
```

From a second terminal, use `nc -U /tmp/pici-demo.sock` (or a short Python
script using the `socket` module, `AF_UNIX`/`SOCK_STREAM`) to send a few
JSONL lines: one `"round"` with text + two concurrent `"tool_call"` blocks
(different `call_id`s, different `finish_after_ms` so they finish out of
order) and `"stop_reason":"tool_calls"`, a second `"round"` with closing
text and `"stop_reason":"end_turn"`, then `{"type":"turn"}`. Confirm in the
`pi` terminal that: both tool regions appear in call order (not completion
order — this is the exact bug class the whole `RegionRenderer` project
exists to prevent), each tool's body updates live as its scripted `updates`
arrive, and the final text block renders after both tools regardless of
which finished last.

## Explicit non-goals for this pass

- Single connection at a time — no concurrent test harnesses talking to one
  `pi` process simultaneously.
- No reconnect-mid-turn recovery. A dropped connection mid-`"turn"` closes
  `RemoteFauxClient`, which surfaces as a normal `run_prompt` error, same as
  a real provider connection dropping.
- No artificial sub-chunking of text/thinking deltas (one `TextDelta` per
  `"text"` block). `delay_between_ms` still paces between distinct emitted
  events (tool call start vs. its delta vs. its end, etc.), which is enough
  to exercise the paint loop's coalescing behavior; finer intra-block
  chunking can be added later as an optional `"chunk_size"` field if
  needed.
- No fake token-usage control beyond whatever zero-valued/default
  `TokenUsage` ends up on the synthesized `AssistantMessage` — enough to
  exercise the status bar's usage display, not to test exact numbers.
- `"quit"` ends the `pi` process. Handing control back to an interactive
  readline REPL after a `--faux-control` session is out of scope.
