# pi-cpp — C++23 Agent Loop

A C++23 implementation of the [pi-mono](https://github.com/badlogic/pi-mono) core agent loop runtime, built with CMake.

## Architecture

```
┌─────────────────────────────────────────────────────┐
│                     pi-cpp                          │
├─────────────────────────────────────────────────────┤
│                                                     │
│  ┌──────────┐  ┌────────────┐  ┌──────────────┐    │
│  │  Agent   │  │AgentLoop   │  │ AgentState   │    │
│  │          │──▶│            │──▶│             │    │
│  │ - prompt │  │ - run_loop │  │ - messages  │    │
│  │ - continue│ │ - tool_exec│  │ - tools     │    │
│  │ - steer  │  │ - steering │  │ - model     │    │
│  │ - follow │  │ - follow_up│  │ - pending   │    │
│  │          │  └────────────┘  └──────────────┘    │
│  │          │                                        │
│  │          │  ┌────────────────────────────────┐    │
│  │          │  │       EventStream              │    │
│  │          │  │  push ──▶ next ──▶ wait ──▶ R │    │
│  │          │  └────────────────────────────────┘    │
│  │          │                                        │
│  │          │  ┌──────────┐  ┌──────────┐           │
│  │          │  │ Messages │  │  LLM     │           │
│  │          │  │ User/Ass │  │ Client   │           │
│  │          │  │ ToolRes  │  │ (stub)   │           │
│  │          │  └──────────┘  └──────────┘           │
│  │          │                                        │
│  │          │  ┌──────────┐  ┌──────────┐           │
│  │          │  │  Events  │  │  Tools   │           │
│  │          │  │ start/end│  │ execute  │           │
│  │          │  │ turn/msg │  │          │           │
│  │          │  │ tool_*   │  │          │           │
│  │          │  └──────────┘  └──────────┘           │
│  └──────────┘                                        │
└─────────────────────────────────────────────────────┘
```

## Project Structure

| File(s) | Lines | Purpose |
|---------|-------|---------|
| `message_types.h/.cpp` | 947 | Message types, content blocks, model, tools, built-in JSON |
| `event_types.h/.cpp` | 290 | Event types (agent, turn, message, tool events) |
| `stream.h` | 325 | Thread-safe `EventStream` — blocking iterator, callbacks, drain |
| `agent_state.h` | 193 | Thread-safe agent state: transcript, tools, model, stop token |
| `agent_loop.h/.cpp` | 942 | Core loop: LLM call → tools → repeat |
| `agent.h/.cpp` | 252 | High-level `Agent` API: prompt, continue, steer, abort |
| `llm_client.h/.cpp` | 92 | Abstract LLM provider with stub fallback |
| `main.cpp` | 203 | CLI entry with demo tool |
| `test/` | ~1500 | Self-hosted test harness (no Catch2) |

**Total: ~3,300 lines of C++**

## Core Types

### Messages (LLM-compatible)
- **UserMessage** — user input with text + optional images
- **AssistantMessage** — LLM response with text, thinking, tool calls, usage stats
- **ToolResultMessage** — tool execution results

### Content Blocks
- **TextContent** — text response
- **ThinkingContent** — reasoning/thinking blocks  
- **ImageContent** — base64 images
- **ToolCall** — tool invocation with id, name, arguments

### Events
- **AgentStart/End** — agent lifecycle
- **TurnStart/End** — one LLM call + tool executions
- **MessageStart/Update/End** — streaming message updates
- **ToolExecutionStart/Update/End** — tool call lifecycle

### Tool System
- **ToolDefinition** — abstract tool interface (`name()`, `schema()`, `execute()`)
- **ToolSchema** — abstract schema for tool parameter definitions
- **ToolResult** — abstract result from tool execution
- Support for sequential and parallel tool execution modes

## Providers and live model switching

The effective catalog combines the built-in models with provider and custom
model tables from `config.toml`. Provider credentials and headers are scoped by
provider; authentication-owned headers cannot be overridden by model config.

```toml
[providers.local]
api = "openai-completions"
base_url = "http://127.0.0.1:8080/v1"
auth = "none"

[[providers.local.models]]
id = "qwen3-coder"
context_window = 131072
max_tokens = 16384
```

Use `--list-models` or the interactive `/models [filter]` command to inspect
the effective catalog. `/model <provider/model>` switches an idle session;
`/model` opens the TTY selector and prints usage in non-interactive input.
Switches are rejected during streaming or queued tool work and are journaled as
provider/model metadata, so resume, fork, RPC `set_model`, and ACP per-run
selection restore the correct next-turn model. Historical assistant messages
remain tagged with the model that produced them and are transformed for the
next provider.

## Bash sandbox

The `bash` tool supports per-session process isolation on Linux through
[bubblewrap](https://github.com/containers/bubblewrap). Select the behavior with
`--sandbox <mode>` or `[sandbox].mode` in the config file:

- `auto` (default): use bubblewrap and fail clearly if it is unavailable.
- `required`: require bubblewrap explicitly.
- `disabled`: run bash directly on the host; this is an explicit escape hatch.

Sandboxed commands receive the workspace at `/workspace`, an isolated `/tmp`,
no network namespace access, and a minimal read-only runtime. The user home
directory, credentials, and host sockets are not mounted. Existing process-group
timeouts and cancellation still apply. `--no-sandbox` is an alias for
`--sandbox disabled`.

The Lua `before_tool_call` hook remains a policy layer; it can block calls but
cannot weaken the C++ sandbox boundary.

## Renderer

The `Renderer` interface is a pure presentation observer decoupled from the
agent loop.  The agent produces `AgentEvent` variants; `dispatch_event`
translates them into typed `Renderer` calls.  The two are orthogonal — swap
either without touching the other.

### Interface (`src/core/stream_renderer.h`)

```cpp
class Renderer {
public:
  // A new user → assistant turn is beginning.
  virtual void on_turn_start() {}

  // Streaming assistant answer text.  Only pure-virtual method.
  virtual void on_text_delta(std::string_view delta) = 0;

  // Streaming model reasoning (emitted separately by some models).
  virtual void on_thinking_start() {}
  virtual void on_thinking_delta(std::string_view delta) {}
  virtual void on_thinking_end() {}

  // Tool call lifecycle.  call_id correlates start↔end for parallel tools.
  virtual void on_tool_start(std::string_view call_id,
                              std::string_view tool_name,
                              std::string_view args_json) {}
  virtual void on_tool_end(std::string_view call_id,
                            std::string_view tool_name,
                            const ToolResult &result,
                            bool is_error) {}

  // One assistant message fully received (several per turn when tools run).
  virtual void on_message_end(const TokenUsage &usage) {}

  // Entire agent turn complete (all messages + tool results).
  virtual void on_turn_end() {}

  // LLM, transport, tool, or abort error.
  virtual void on_error(RendererErrorKind kind, std::string_view message) {}
};
```

All methods except `on_text_delta` have default no-op implementations, so an
implementation only overrides what it cares about.

### Wiring to the event stream

```cpp
// dispatch_event translates one AgentEvent into the appropriate Renderer call.
void dispatch_event(const AgentEvent &ev, Renderer &renderer);

// Use inside any EventStream iteration loop:
for (const auto &ev : agent.prompt(text))
    dispatch_event(ev, *my_renderer);
```

### Built-in renderers

| Name | Factory | Behaviour |
|------|---------|-----------|
| `"auto"` | `make_auto_renderer(fd)` | Markdown on TTY, raw on pipes |
| `"markdown"` | `make_diff_renderer(fd)` | In-place markdown with scrollback-safe committed regions |
| `"viewport"` | `make_viewport_renderer(fd)` | Alternate-screen viewport with status bar, readline row, and scroll controls |
| `"raw"` | `make_raw_renderer(fd)` | Plain text append, no escape codes |

### Viewport renderer notes

The viewport renderer is a full-screen compositor.  It owns the assistant output
area and status row, but the final row is reserved for readline input.  That
separation matters:

- Assistant output does **not** draw a fake cursor.  The output area can repaint
  frequently while a response streams.
- Readline draws the visible user insertion cursor on the prompt row, so the
  cursor marks where typed text will appear even if viewport repainting moves the
  terminal's hardware cursor.
- `Renderer::on_scroll(RendererScrollCommand)` is a no-op by default.  The CLI
  maps `PageUp`/`PageDown`, `Up`/`Down`, and `Home`/`End` to that hook; only the
  viewport renderer currently consumes it.
- Scroll state is tracked in wrapped terminal rows, not raw markdown bytes.  The
  row accounting shares the same ANSI/UTF-8 width helpers used by markdown
  rendering.

### Custom renderers

Implement `Renderer` and register with the global registry:

```cpp
StreamRendererRegistry::instance().register_renderer(
    "my-renderer",
    [](int fd) { return std::make_unique<MyRenderer>(fd); });
```

Select via `--render my-renderer` on the CLI.

### Error kinds

```cpp
enum class RendererErrorKind { llm, transport, tool, abort, unknown };
```

## Dependencies

- **CMake 3.28+** (for C++23 support)
- **g++ 13+** or **clang 17+**
- **libstdc++** with C++23 support (threads, stop_token, concepts, ranges)
- **nlohmann/json** v3.11.3 (fetched automatically via FetchContent)
- **pboettch/json-schema-validator** v2.3.0 (fetched automatically via FetchContent)

## Building

The root `Makefile` wraps the common CMake flows:

```bash
make dev          # Debug pi-cli in build/
make release      # Release pi-cli in build-release/
make lint         # clang-tidy target
make format       # clang-format target
make test         # build + ctest
```

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build --parallel

# Optional static analysis target.  This target currently prints advisory
# clang-tidy warnings but exits successfully unless clang-tidy itself fails.
cmake --build build --target tidy

# Run demo
./build/pi-cli demo

# Run version
./build/pi-cli version

# Run tests
./build/test-core
./build/test-agent-loop
./build/test-agent
./build/test-stream
```

For a faster edit-compile loop, build the target you are working on instead of
the whole default target graph:

```bash
cmake --build build --target pi-cli --parallel
cmake --build build --target test-markdown --parallel
```

### Streaming diagnostics

When a provider appears to return a complete response instead of streaming,
enable the privacy-safe JSONL trace. It records elapsed times, transport SSE
events, parser events, renderer events, and byte counts; it does not record
prompts, response text, tool arguments, or API keys.

```bash
./build/pi-cli --stream-trace /tmp/pici-stream.jsonl -p "say hello"
cat /tmp/pici-stream.jsonl
```

Compare `transport`, `parser`, and `renderer` timestamps. If transport events
arrive together, the provider or an intermediary batched the response. If
parser events advance but renderer events do not, the client path is at fault.

### JSONL RPC mode

`pi-cli --mode rpc` provides a long-lived, machine-facing control stream. Send
one JSON command per stdin line and read one JSON response or event per stdout
line; keep stdin open while a prompt is running. Responses preserve the optional
client-supplied `id`.

```json
{"id":"p1","type":"prompt","message":"Summarize this repository"}
{"id":"state","type":"get_state"}
{"id":"stop","type":"abort"}
```

The initial command set is `prompt`, `steer`, `follow_up`, `abort`,
`get_state`, `get_messages`, `set_thinking_level`, `new_session`,
`switch_session`, `fork`, and `set_session_name`. Prompt events use
`{"type":"event","event":"message_update",...}` and expose text,
thinking, tool-call, and tool-execution updates. A run ends with either
`run.completed` or `run.failed`.

### ACP task control

pi-acp owns one asynchronous task manager shared by its HTTP routes. Child
tasks are controlled through:

- POST /tasks to spawn;
- GET /tasks and GET /tasks/:id to inspect;
- POST /tasks/wait for bounded status waits;
- POST /tasks/:id/message and /follow-up;
- POST /tasks/:id/interrupt and /close.

GET /tasks/events?after_generation=N&timeout_ms=... returns retained lifecycle
events. Add stream=1 for an SSE stream. Child agents inherit only the
currently certified read-only built-in tools.

The default configuration keeps tests enabled, but OpenTelemetry API
instrumentation is off by default because it pulls in a large vendored target
graph.  Enable it explicitly when working on tracing:

```bash
cmake -B build-otel -DPI_CPP_OTEL_API=ON
```

### Lua context access

Lua `on_command` hooks receive a fourth argument in addition to the legacy
`cmd`, `args`, and flattened `transcript` values:

```lua
on_command = function(cmd, args, transcript, context)
  local view = args == "effective" and context.effective or context.raw
  if args == "effective" and not view.available then
    return {handled = true, output = "effective context is not available yet"}
  end
  return {handled = true, output = json.encode(view)}
end
```

`context.raw` contains the configured system prompt, complete message content
and metadata, model metadata, and full structured tool definitions.
`context.effective` is unavailable until a request is prepared; afterward it
contains the most recent post-transform/post-conversion context, plus the
provider and API identifiers. It is provider-neutral context, not the exact
wire JSON sent to a model. Lua receives immutable copies, and existing
three-argument hooks remain compatible.

Messages preserve all content blocks, including thinking/signatures, images,
tool calls/results, usage, timestamps, and response metadata. The example add-on
also provides static TUI views: `/context genome`, `/context heatmap`, and
`/context tools`, with effective-context variants such as
`/context effective heatmap`. Context dumps may contain sensitive prompt
content, reasoning data, and large base64 image payloads. See
[`addons/context.lua`](addons/context.lua) for the implementation.

Use `/reload-addons` after editing Lua hooks or add-on tools to reload the
configured files without restarting the session. Built-in and tools-directory
tools are preserved; registered add-on tools and hook callbacks are replaced.

Add-ons can also customize the interactive UI with `status_line(ctx)` and
`tab_title(ctx)` hooks. The status line is rendered above the readline prompt
(or in the viewport renderer's reserved status row), while the title hook
updates the terminal tab/window title. Both receive model, usage, tool-count,
and session metadata; ANSI color sequences are supported in the status line.

For a lean local build directory that skips test targets:

```bash
cmake -B build-dev -DPI_CPP_BUILD_TESTS=OFF -DPI_CPP_OTEL_API=OFF
cmake --build build-dev --target pi-cli --parallel
```

CMake will use `ccache` automatically when it is installed.  Ninja also tends to
give better incremental scheduling than Make:

```bash
cmake -S . -B build-ninja -G Ninja -DPI_CPP_OTEL_API=OFF
cmake --build build-ninja --parallel
```

## OpenAI authentication

The `openai` provider uses the normal OpenAI Platform API key (`OPENAI_API_KEY`
or `--api-key`) and is billed through the Platform account. ChatGPT/Codex
subscription authentication is a separate experimental provider:

```bash
pi-cli auth login openai-codex
pi-cli auth status openai-codex
pi-cli --provider openai-codex --model gpt-5.5 -p "Explain this repository"
pi-cli auth logout openai-codex
```

Use `pi-cli auth login openai-codex --device` when the loopback browser callback
cannot be used. Credentials are stored as a versioned, mode-0600 file under
`$XDG_CONFIG_HOME/pici/auth.json` (or `~/.config/pici/auth.json`); set
`PICI_AUTH_FILE` to override the location. `OPENAI_API_KEY` is intentionally
not used for `openai-codex`, and `--api-key` is rejected for that provider.

## Local OpenAI-Compatible API

The CLI defaults to the local API at `http://127.0.0.1:8080/v1` and the
model currently exposed by that server:
`Qwen3.6-35B-A3B-UD-IQ4_NL.gguf`.

```bash
./build/pi-cli chat
```

Override either value when needed:

```bash
./build/pi-cli chat --base-url http://127.0.0.1:8080/v1 --model Qwen3.6-35B-A3B-UD-IQ4_NL.gguf
```

## Usage Example

```cpp
#include "core/agent.h"
#include "core/message_types.h"

using namespace pi::core;

// Define a custom tool
class MyTool : public ToolDefinition {
    std::string_view name() const override { return "my_tool"; }
    std::string_view description() const override { return "Does something useful"; }
    ToolSchema& schema() override { return schema_; }
    std::shared_ptr<ToolResult> execute(std::string_view, std::string_view,
                                        std::stop_token) const override {
        return std::make_shared<MyResult>();
    }
    ToolExecutionMode execution_mode() const override {
        return ToolExecutionMode::parallel;
    }
private:
    struct Schema : ToolSchema {
        std::string serialize() const override { return R"({"type":"object"})"; }
        std::map<std::string, std::string> to_definition() const override {
            return {{"type", "object"}};
        }
    } schema_;
    
    struct Result : ToolResult {
        bool is_error() const override { return false; }
        std::string content() const override { return "Done"; }
        std::optional<std::string> details() const override { return std::nullopt; }
    };
};

int main() {
    Model model;
    model.id = "gpt-4";
    model.name = "GPT-4";
    model.api = "openai-completions";
    model.provider = "openai";
    model.base_url = "https://api.openai.com/v1";

    Agent::Options opts;
    opts.model = model;
    opts.system_prompt = "You are helpful.";

    Agent agent(opts);
    agent.add_tool(std::make_shared<MyTool>());

    // Run a prompt and process events
    auto stream = agent.prompt("Hello, what can you do?");
    for (auto& event : stream) {
        visit_event(event, [](const auto& ev) {
            // Handle events via template parameter
        });
    }

    // Or block for the final result
    auto [messages, error] = stream.wait();
}
```

## Key Design Decisions

1. **nlohmann/json** — all JSON parsing/serialization via nlohmann/json; JSON Schema validation via pboettch/json-schema-validator
2. **Explicit error surfaces** — expected runtime errors generally flow through `std::optional`, result objects, or renderer error callbacks; boundary code still catches and translates exceptions where third-party libraries throw
3. **Thread safety** — `AgentState` uses `std::mutex` for all shared state
4. **Async streaming** — `EventStream` supports blocking iterator, `for_each`, and `wait()`
5. **Provider abstraction** — `LLMClient` interface allows swapping providers without touching the loop
6. **C++23 features** — `std::stop_token`/`std::stop_source` for cancellation, concepts, `if constexpr`, `std::scoped_lock`, structured bindings
7. **Self-contained tests** — custom test harness, no Catch2 or other test frameworks

## Implementation Lessons

- Prefer public umbrella headers for vendored C libraries.  For libcurl, include
  `<curl/curl.h>` rather than internal headers such as `curl/easy.h`; the latter
  assumes setup macros from the public header.
- When using toml++, define configuration macros before including toml++ headers.
  This project uses header-only toml++ in `src/cli/config.cpp`.
- C++23 ranges calls do not take placeholder commas.  Use
  `std::ranges::sort(items, pred)`, `std::ranges::reverse(items)`, and remember
  `std::ranges::remove_if` returns a subrange, so erase with
  `removed.begin(), removed.end()`.
- For JSON iterators from nlohmann/json, use `it.value()` or `(*it)` explicitly.
  `*it[0]` parses as `*(it[0])`, not as `(*it)[0]`.
- RAII types that own terminal or renderer state should explicitly delete copy
  and move operations when duplicating the handle would be invalid.
- The tidy target is useful as a regression net, but many current warnings are
  policy/advisory findings in established code paths.  Fix targeted warnings
  near edited code instead of churning the whole tree opportunistically.
- Keep optional instrumentation truly optional.  OpenTelemetry includes and
  link dependencies should stay behind `PI_CPP_OTEL_ENABLED`; otherwise a fast
  local build still needs the vendored telemetry headers.
- Avoid letting vendored helper tools leak into the default `all` target.  Use
  `EXCLUDE_FROM_ALL` for dependency FetchContent declarations when the project
  only needs their libraries.

## File Index

```
src/
├── core/
│   ├── message_types.h    # Message, ContentBlock, Model, Tool interfaces
│   ├── message_types.cpp  # JSON serialization (built-in parser)
│   ├── event_types.h      # AgentEvent variant and event classes
│   ├── event_types.cpp    # Event debug output
│   ├── stream.h           # EventStream template + AsyncEventStream
│   ├── stream.cpp         # Header-only stub
│   ├── agent_state.h      # Thread-safe state container
│   ├── agent_state.cpp    # Header-only stub
│   ├── agent_loop.h       # RunAgentLoop config and entry points
│   ├── agent_loop.cpp     # Main loop: LLM call → tools → repeat
│   ├── agent.h            # High-level Agent class
│   ├── agent.cpp          # Agent implementation
│   └── llm_client.h       # LLM provider interface
│   └── llm_client.cpp     # Stub client + registry
├── http/
│   ├── http_client.h      # HTTP client (optional, disabled)
│   └── http_client.cpp    # Curl-based client (optional, disabled)
├── main.cpp               # CLI entry point
test/
├── test_core.cpp          # Message, Event, Stream tests
├── test_agent_loop.cpp    # Agent loop integration tests
├── test_agent.cpp         # Agent API tests
└── test_stream.cpp        # EventStream tests
```

## Comparison with pi-mono (TypeScript)

| pi-mono (TS) | pi-cpp | Notes |
|-------------|--------|-------|
| `packages/agent/src/agent-loop.ts` | `agent_loop.h/.cpp` | Core multi-turn loop |
| `packages/agent/src/agent.ts` | `agent.h/.cpp` | High-level API |
| `packages/ai/src/types.ts` | `message_types.h` | Messages, content, tools |
| `packages/agent/src/types.ts` | `event_types.h` | AgentEvent, tool calls |
| `packages/coding-agent/src/core/session.ts` | `agent_state.h` | Transcript + state |
| (providers) | `llm_client.h` | Provider abstraction |
| nlohmann/json | `message_types.cpp` | Built-in JSON parser |
| Catch2 | `test/*.cpp` | Custom test harness |
