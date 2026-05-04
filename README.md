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

## Dependencies

- **CMake 3.28+** (for C++23 support)
- **g++ 13+** or **clang 17+**
- **libstdc++** with C++23 support (threads, stop_token, concepts, ranges)
- **nlohmann/json** v3.11.3 (fetched automatically via FetchContent)
- **pboettch/json-schema-validator** v2.3.0 (fetched automatically via FetchContent)

## Building

```bash
# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build
cmake --build build

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
2. **No exceptions** — error propagation via `std::optional` and return values
3. **Thread safety** — `AgentState` uses `std::mutex` for all shared state
4. **Async streaming** — `EventStream` supports blocking iterator, `for_each`, and `wait()`
5. **Provider abstraction** — `LLMClient` interface allows swapping providers without touching the loop
6. **C++23 features** — `std::stop_token`/`std::stop_source` for cancellation, concepts, `if constexpr`, `std::scoped_lock`, structured bindings
7. **Self-contained tests** — custom test harness, no Catch2 or other test frameworks

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
