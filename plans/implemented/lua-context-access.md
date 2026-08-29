# Expose raw and effective model context to Lua plugins

## Objective

Extend the Lua plugin interface so a plugin can inspect either:

1. The current raw agent state: the configured system prompt, complete session
   transcript, model metadata, and registered tool definitions.
2. The effective context used for a model request: the context after any
   context transformation and message conversion, immediately before it is
   passed to the `LLMClient`.

The primary use case is a Lua `/context` diagnostic command, but the API should
also support exporters, context-size inspectors, redaction/auditing tools,
debugging add-ons, and future plugins that need to reason about what the model
actually received.

This plan is for the C++/Lua bridge and its tests. It does not require a
specific `/context` plugin, although a small example or fixture should be
included to prove the interface is usable.

## Repository state and important constraints

At the time this plan was written, the working tree already contained changes
for Muse support and other related work, including edits under `src/core`,
`src/main.cpp`, tests, `config.toml.example`, and an untracked `addons/`
directory. Treat those changes as user-owned. Do not reset, discard, or
mechanically rewrite unrelated changes while implementing this plan.

Follow the repository instructions in `AGENTS.md`:

- Use `make format` for formatting.
- Use a narrow `make dev` or equivalent build during the edit loop.
- Before committing, run `make test`, or the documented CMake build and CTest
  equivalents.
- Run `make lint` and treat new warnings in touched code as actionable.
- Keep terminal ownership RAII-only and avoid decorative divider comments.

## Current implementation

The existing hook path is close but intentionally exposes only a reduced view:

- `src/main.cpp` dispatches slash commands through
  `hooks->on_command(cmd, args, agent.state().messages())`.
- `src/core/lua_tool.h` declares `LuaHooks::on_command` with the native
  signature `(cmd, args, vector<Message> transcript)`.
- `src/core/lua_tool.cpp::call_on_command` passes three Lua arguments:
  `cmd`, `args`, and `push_messages_to_lua(...)`.
- `push_messages_to_lua` currently emits records shaped approximately as
  `{index, role, content, turn, tool_name, is_error}`. It concatenates only
  `TextContent` blocks into a string.
- The current Lua projection therefore drops `ThinkingContent`,
  `ImageContent`, and `ToolCall` blocks. It also drops timestamps, assistant
  metadata, usage, stop reason, response identifiers, error messages, tool
  call IDs, tool-result details, and other fields represented by
  `Message`.
- `pici.log(...)` is available to Lua and writes diagnostic output to stderr.
  It remains useful for add-on diagnostics, but user-facing command output
  should use `CommandResult.output` so it follows the active renderer.

The relevant state and request path are:

- `src/core/agent_state.h::AgentState` stores the raw system prompt, model,
  messages, and tools.
- `src/core/agent.cpp::Agent::create_context_snapshot()` builds an
  `AgentContext` from the current system prompt, messages, and tools.
- `src/core/agent_loop.cpp::stream_assistant_response()` copies
  `context.messages`, applies `config.transform_context` when present, then
  applies `config.convert_to_llm`, and passes the resulting `llm_context` to
  `LLMClient::stream(...)`.
- `src/core/message_types.cpp::json::to_json(const Message&)` already
  serializes complete message content blocks and message metadata.
- `src/core/lua_tool.cpp` already contains a generic `json_to_lua` helper for
  converting JSON values into Lua tables.
- `src/core/agent_loop.h::AgentContext` carries the system prompt, messages,
  model, and tools. The effective snapshot replaces its messages with the
  post-transform/post-conversion request context and associates the request
  model from `AgentLoopConfig`.

## Design decisions

### Preserve existing Lua hooks

Keep the existing first three `on_command` arguments and add the new context as
a fourth Lua argument:

```lua
on_command = function(cmd, args, transcript, context)
  ...
end
```

`CommandResult.output`, when present, is displayed through the active CLI
renderer without being sent to the model. This is the user-facing output
channel for diagnostic commands; `pici.log(...)` remains stderr-only diagnostic
logging, and `prompt` continues to mean “send another prompt to the model.”

Existing Lua functions that declare only three parameters continue to work,
because Lua ignores extra arguments. Existing `transcript` behavior should
remain stable during the compatibility phase; new code should use
`context.raw.messages` for complete messages.

Update the native C++ callback type as needed to carry a context snapshot, but
do not make callers construct Lua tables. Keep native C++ state strongly typed
and perform Lua conversion in `lua_tool.cpp`.

### Use explicit `raw` and `effective` views

The fourth Lua argument should be a table with a stable top-level shape:

```lua
context = {
  raw = {
    system_prompt = "...",
    messages = { ... },
    model = { ... },
    tools = { ... },
  },
  effective = {
    available = true,
    system_prompt = "...",
    messages = { ... },
    model = { ... },
    tools = { ... },
    provider = "...",
    api = "...",
  },
}
```

`raw` means the current `AgentState` snapshot at the time the hook is
invoked. It is the right view for inspecting the complete session, configured
instructions, and tools before the next turn.

`effective` means the last context snapshot captured immediately before the
LLM client was called, after `transform_context` and `convert_to_llm` have run.
It must be marked unavailable (`available = false`, or represented as `nil`
with documented semantics) until the first model request has been prepared.
This avoids pretending that a command invoked before the next turn can predict
provider-specific transformations that have not happened yet.

If a future API needs to expose the *next* effective context, it should reuse
the exact request-preparation path rather than independently reimplementing
the transformations in the Lua bridge. That is outside the minimum scope of
this plan.

### Preserve complete message structure

New context messages must retain the complete internal representation rather
than flattening content to text. A message should include its role and a
`content` array containing entries such as:

```lua
{
  role = "assistant",
  content = {
    { type = "text", text = "visible answer" },
    {
      type = "toolCall",
      id = "call_1",
      name = "search",
      arguments = { query = "..." },
    },
  },
  usage = { ... },
  timestamp = 0,
  model = "...",
  provider = "...",
  api = "...",
  stopReason = "tool_use",
}
```

Use the existing message JSON representation as the compatibility reference:
`src/core/message_types.cpp::json::to_json(const Message&)`. Preserve its
field names and content-block types unless there is a documented reason to
add a Lua-specific field such as the 1-based `index` or `turn`.

The full representation includes image data and reasoning/signature fields.
Do not silently drop them. Document that plugins receiving this view may see
large base64 payloads, redacted/encrypted reasoning data, and sensitive prompt
content.

### Represent model and tools explicitly

The raw and effective views should include enough information to understand
what was configured and what was sent:

- Model: at minimum `id`, `name`, `provider`, `api`, `base_url`, reasoning
  capability, input capabilities, context window, and max tokens. Include
  pricing only if it is already represented consistently; do not invent
  values for unknown pricing or limits.
- Tools: include name, description, source path when available, and the full
  input schema as structured JSON/Lua data. Do not expose only the abbreviated
  `AgentInfo.tool_names` list for this API.
- Effective view: include the provider and API identifiers actually associated
  with the request-ready model context.

Prefer shared serializers or structured conversion helpers over a second,
independently maintained message format. If the existing `Model` JSON helper
does not include fields needed by this API, extend it deliberately or add a
dedicated documented context serializer rather than making the Lua bridge
depend on formatted JSON strings.

### Decide where the effective snapshot lives

The effective snapshot must be copied before `LLMClient::stream(...)` begins,
because the local `llm_context` is currently a function-local value. Choose one
of these implementations, documenting the tradeoff in code:

1. Add a thread-safe last-effective-context snapshot to `Agent`/`AgentState`.
   `stream_assistant_response` updates it after transformation/conversion, and
   the CLI supplies it to the Lua hook. This is the clearest approach for a
   later `/context` command.
2. Add a context-observer callback to `Agent::Options`/`AgentLoopConfig` and
   have `main.cpp` retain the most recent snapshot for Lua. This avoids making
   the core state itself responsible for diagnostics but adds callback wiring.

Prefer the smallest design that provides a synchronized copy and does not
expose mutable internal state to Lua. The snapshot must be immutable from
Lua's perspective; `truncate_to` remains the only existing command operation
that mutates the transcript.

Do not capture API keys, authorization headers, or raw HTTP request headers in
the context view. The provider-ready message/tool context is sufficient for
this feature and avoids turning a diagnostic command into a credential dump.

## Implementation steps

### 1. Define the native hook context contract

Update `src/core/lua_tool.h` with a small native context/snapshot type or an
equivalent callback contract that can carry:

- raw system prompt, messages, model, and tools;
- effective availability and request-ready system prompt, messages, model,
  tools, provider, and API;
- immutable value semantics suitable for crossing the Lua mutex boundary.

Keep the existing `CommandResult` behavior unchanged. Update comments and Lua
signature documentation to describe the fourth argument and the raw/effective
semantics.

### 2. Build complete Lua values

In `src/core/lua_tool.cpp`:

- Replace or supplement `push_messages_to_lua` with a complete message
  serializer based on the canonical message JSON representation.
- Preserve 1-based `index` and existing `turn` convenience fields where they
  are useful, while retaining the complete `content` array.
- Add model and tool serializers. Tool schemas should become structured Lua
  tables, with deterministic fallback behavior for invalid schemas.
- Add a context-table serializer for `raw` and `effective`.
- Keep old `transcript` serialization available for compatibility if existing
  tests or plugins depend on its flattened `content` string.

Do not call `pici.log` from the bridge as part of serialization. Logging should
remain the plugin's choice.

### 3. Capture the effective request context

In `src/core/agent_loop.cpp` or the selected observer/state location:

- Capture the post-`transform_context`, post-`convert_to_llm` context at the
  same point where `llm_context` is constructed.
- Copy the relevant model/provider/API metadata alongside the context.
- Publish the snapshot before calling `LLMClient::stream`.
- Make concurrent reads safe while the agent is streaming.
- Clear or replace the snapshot on reset/session changes according to the
  documented semantics; do not accidentally show a previous session as the
  current effective context.

In `src/main.cpp`, pass the raw state and the current effective snapshot to the
Lua hook when dispatching a command. Ensure this works with composed hooks and
with no hooks loaded.

This implementation uses the observer option: `AgentLoopConfig` publishes a
value copy before `LLMClient::stream`, and the CLI retains the latest snapshot
behind a mutex. The CLI clears it when switching, forking, or truncating the
session so a previous branch is not presented as the current effective view.

### 4. Preserve composition and existing hook behavior

Update `compose_hooks` so every composed `on_command` receives and forwards
the same context snapshot. The first handled result must still win.

Update command completion only if the new context is intentionally exposed
there. It is not required for the initial feature. Do not change the behavior
of `before_tool_call`, `after_tool_call`, or `should_stop_after_turn` unless
the chosen native context abstraction can be reused without altering their
existing Lua contracts.

### 5. Add a diagnostic Lua fixture/example

Add a small example under the repository's existing add-on/example location
(or a test-only Lua fixture if no user-facing addon directory is appropriate)
that implements `/context` and demonstrates selecting either view:

```lua
on_command = function(cmd, args, transcript, context)
  if cmd ~= "context" then return nil end

  local view = args == "effective" and context.effective or context.raw
  if not view.available and args == "effective" then
    return { handled = true, output = "effective context is not available yet" }
  end

  return { handled = true, output = json.encode(view) }
end
```

The example should make clear that `pici.log` writes to stderr for diagnostics
and that command output can contain sensitive or very large data.

### 6. Document the interface

Update the Lua hook documentation in `src/core/lua_tool.h` and the relevant
user-facing documentation/README section with:

- the fourth `on_command` argument;
- the raw/effective definitions;
- complete message and tool shapes;
- effective-context availability and last-request semantics;
- sensitive-data and image-size warnings;
- the distinction between this provider-neutral context and an exact wire
  payload.

## Tests

Extend `test/test_lua_tool.cpp` using its existing hand-rolled test harness.
Tests should exercise the public Lua hook behavior, not just internal helper
functions.

### Serialization coverage

Construct messages containing every supported block type and verify Lua can
inspect:

- text content and message role;
- thinking content, signatures, and redacted state;
- image data and MIME type;
- tool-call ID, name, and structured arguments;
- tool-result IDs, names, details, error state, and content;
- timestamps and assistant metadata/usage/stop reason where applicable;
- stable 1-based indexes and compatibility fields.

Verify invalid or unusual tool schemas have deterministic, non-crashing
representation.

### Hook/API coverage

Add tests that verify:

- a four-argument Lua `on_command` receives raw and effective context;
- an existing three-argument `on_command` still works unchanged;
- raw context contains the full system prompt, model, messages, and tools;
- effective context is marked unavailable before any request;
- effective context reflects post-transform/post-conversion messages after a
  request-preparation test seam runs;
- context snapshots are immutable from Lua and do not alter agent state;
- composed hooks forward the same context and preserve first-handled-wins;
- `pici.log` is not required for context construction or serialization.

If the effective snapshot is stored at the agent level, add a focused agent or
agent-loop test covering replacement/reset/session behavior and concurrent
read safety. Use a fake LLM client or an existing test seam rather than a live
network call.

### Fixture coverage

Run the example `/context` fixture through the Lua test loader where practical,
including both raw/default output and the effective-unavailable path.

## Verification checklist

During implementation:

```sh
make format
make dev
ctest --test-dir build --output-on-failure -R 'lua_tool|agent|agent_loop'
```

Before committing:

```sh
make test
make lint
```

If the default build is unavailable or stale, use the documented equivalent:

```sh
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

Manually verify with the example plugin that:

1. `/context` prints the raw transcript without sending a prompt to the model.
2. `/context effective` reports unavailable before the first request.
3. After a request, `/context effective` shows the transformed/converted
   request-ready messages, including tool calls/results where present.
4. Existing Lua tools and hooks still load and run.
5. Images and large content do not crash or corrupt the terminal, even if the
   diagnostic output is intentionally large.

## Non-goals and follow-up work

- Do not expose API keys, authorization headers, or arbitrary HTTP headers.
- Do not make Lua mutate raw or effective snapshots.
- Do not promise that `effective` is the exact provider wire JSON. Capturing
  provider request payloads is a separate tracing/diagnostics feature and
  should reuse the existing stream diagnostics mechanism or a new explicit
  payload hook.
- Do not add context trimming, token counting, context-window enforcement, or
  provider-specific serialization to Lua.
- Do not add a general-purpose `pici.print` API in this feature. User-facing
  command output uses `CommandResult.output` and the existing renderer
  lifecycle; `pici.log` remains available for stderr diagnostics.

## Completion criteria

This plan is complete when an independent implementation can load a Lua hook,
inspect both raw and last-effective context through documented fields, see all
internal message block types without flattening, retain compatibility with
existing three-argument hooks, pass focused and full test suites, and verify
the behavior with a working `/context` example.
