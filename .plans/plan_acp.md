# Plan: ACP Server Support

## Goal

Expose pici as an **Agent Communication Protocol (ACP) server** so any
ACP-compatible client or orchestrator can discover and invoke the local
agent over HTTP.  The result is a new `pi-acp` binary (plus an optional
`--acp-port` flag on `pi-cli`) that runs the existing agent loop behind
a standards-conformant REST + SSE interface.

## Background

ACP (now under Linux Foundation governance, merged with Google A2A) is a
REST-first agent interoperability standard.  Key concepts:

- **Agent manifest** — a JSON descriptor advertising the agent's name,
  description, and supported MIME types.
- **Run** — a single agent invocation.  Modes: sync (block until done),
  stream (SSE while running), async (webhook on completion).
- **Session** — optional `session_id` UUID that preserves conversation
  history across runs.
- **Await** — a run can transition to `awaiting` to pause and request
  more input (maps to pici's `steer()` mechanism).

Reference spec: `https://agentcommunicationprotocol.dev` / OpenAPI at
`https://github.com/i-am-bee/acp/blob/main/docs/spec/openapi.yaml`.

## Mapping: pici → ACP concepts

| pici | ACP |
|------|-----|
| `Agent` | server-side agent (one per process for now) |
| `Agent::prompt(text)` | `POST /runs` with `mode=stream` |
| `EventStream<AgentEvent>` | SSE event stream |
| `AgentState::messages()` | session history (keyed by `session_id`) |
| `Agent::steer(msgs)` | run transitions to `awaiting` + client resumes |
| tool `ToolDefinition::name/description` | included in manifest metadata |
| `AssistantMessage` text content | `message.part` SSE event (text/plain) |
| `ToolExecutionStartEvent` | `message.part` (application/json tool call) |
| `AgentEndEvent` | `run.completed` SSE event |

## Wire format reference

### Message / MessagePart

```json
{
  "role": "agent",
  "parts": [
    {
      "content_type": "text/plain",
      "content": "Hello, world."
    }
  ]
}
```

### SSE stream (one event per line-pair)

```
event: message.part
data: {"type":"message.part","part":{"content_type":"text/plain","content":"Hello"}}

event: run.completed
data: {"type":"run.completed","run":{"status":"completed","output":[...]}}
```

### RunCreateRequest

```json
{
  "agent_name": "pi",
  "input": [{"role":"user","parts":[{"content_type":"text/plain","content":"..."}]}],
  "session_id": "uuid-optional",
  "mode": "stream"
}
```

### AgentManifest

```json
{
  "name": "pi",
  "description": "pi-cpp coding agent",
  "metadata": {
    "framework": "pi-cpp",
    "tools": ["read","write","edit","bash","grep","find","ls"]
  },
  "input_content_types":  ["text/plain"],
  "output_content_types": ["text/plain"]
}
```

## Architecture

```
src/
  acp/
    server.h/cpp        — HTTP server lifecycle, route dispatch
    handlers.h/cpp      — per-endpoint request/response logic
    session_store.h/cpp — in-memory session → AgentState map
    sse.h/cpp           — SSE emitter (chunked transfer encoding)
    types.h/cpp         — ACP JSON types (de/serialize via nlohmann)
```

New CMake target: `pi-acp` (executable).  `pi-core` + `pi-http` remain
unchanged.  HTTP server uses **cpp-httplib** (MIT, single header, no new
system deps, added via FetchContent).

## HTTP endpoints (MVP)

| Method | Path | Notes |
|--------|------|-------|
| `GET`  | `/agents` | list — returns array with one entry |
| `GET`  | `/agents/{name}` | manifest for this agent |
| `POST` | `/runs` | create a run (sync or stream) |
| `GET`  | `/runs/{run_id}` | poll status of an async run |
| `GET`  | `/health` | `{"status":"ok"}` |

Sessions (`POST /sessions`, `GET /sessions/{id}/runs`) deferred to a
follow-on phase — the `session_id` field on `POST /runs` is enough for
stateful use without the full sessions API.

## Phases

### Phase ACP-1 — HTTP infrastructure + manifest

**Deliverables:**

1. Add `cpp-httplib` via `FetchContent` in `CMakeLists.txt`.

2. `src/acp/types.h` — plain C++ structs with nlohmann `to_json` /
   `from_json` for:
   - `AgentManifest`, `MessagePart`, `Message`, `RunCreateRequest`,
     `RunStatus` (enum), `Run`.

3. `src/acp/server.h/cpp` — thin wrapper around `httplib::Server`:
   - `AcpServer::start(port, agent_factory)` — blocks serving.
   - Registers routes; `agent_factory` is a `std::function<Agent*()>`.
   - Handles `Content-Type: application/json` on all non-SSE responses.

4. `GET /health` and `GET /agents` / `GET /agents/{name}` returning the
   manifest built from the live agent's tool list.

5. New `src/acp/main.cpp` + `pi-acp` CMake target.  Accepts `--port`
   (default 8080), `--model`, `--provider`, same flags as `pi-cli`.

6. Tests: start server on a random port in-process, `GET /agents`,
   verify manifest JSON fields.

**Does not include:** run execution, SSE.

---

### Phase ACP-2 — Sync and streaming runs

**Deliverables:**

1. `src/acp/session_store.h/cpp`:
   - `SessionStore` — `std::unordered_map<std::string, AgentState>` 
     protected by mutex.
   - `get_or_create(session_id)` → reference to persisted messages.
   - Max 100 sessions; LRU eviction.

2. `src/acp/sse.h` — `SseEmitter` RAII helper:
   - Sets `Content-Type: text/event-stream`, `Cache-Control: no-cache`.
   - `emit(event_name, json_data)` writes `event: …\ndata: …\n\n` then
     flushes.

3. `src/acp/handlers.cpp` — `POST /runs`:
   - Parse `RunCreateRequest`; extract user text from first `text/plain`
     part.
   - Look up or create session by `session_id`.
   - Create `Agent` with session's existing messages pre-loaded.
   - **`mode=stream`** (primary): set response headers for SSE, then
     iterate `agent.prompt(text)` converting each `AgentEvent` to ACP
     events (see mapping table above).
   - **`mode=sync`** (secondary): collect all events, return completed
     `Run` object as JSON.
   - Emit `run.created` → one or more `message.part` → `run.completed`
     (or `run.failed`).
   - After completion, persist updated messages back to session store.

4. `GET /runs/{run_id}` — trivial: runs are synchronous or streaming so
   status is always terminal by the time this is callable; return the
   stored `Run` object.

5. Tests: start in-process server, `POST /runs` with `mode=sync`, assert
   response contains `status=completed` and non-empty `output`.

---

### Phase ACP-3 — Await / interrupt + async mode

**Deliverables:**

1. **Await support** — when the agent is waiting for tool approval (via
   `before_tool_call` hook returning `block=true` with a special
   `await_reason`), the run transitions to `awaiting` and emits a
   `run.awaiting` SSE event with an `AwaitRequest` payload.  Client
   resumes by calling `POST /runs/{run_id}/resume` with new input, which
   calls `agent.steer(messages)`.

2. **Async mode** — `mode=async` returns immediately with `status=created`
   and a `run_id`.  A background thread drives the agent loop and stores
   the final `Run` for later retrieval.  Optional `webhook_url` support:
   HTTP POST to it on completion.

3. `POST /runs/{run_id}/cancel` — calls `agent.abort()`.

4. Tests: `mode=async` run; poll until completed; assert output.

---

### Phase ACP-4 — Multi-agent / agent-as-tool

**Deliverables:**

1. `AcpClientTool` — a `ToolDefinition` that calls a remote ACP agent
   via `POST /runs` (using `pi-http`).  Registered from CLI with
   `--remote-agent name=http://host:port`.

2. This lets pici orchestrate other ACP-compliant agents as tools in its
   own loop.

3. Tests: mock server; AcpClientTool; verify tool result plumbing.

---

## Minimum viable deliverable

Phases ACP-1 + ACP-2 = a working ACP server.  Any ACP-compatible client
(BeeAI studio, custom scripts) can discover the agent and run prompts
against it over HTTP with full streaming.  Phases ACP-3 and ACP-4 are
additive.

## Out of scope

- TLS / authentication (spec does not mandate; add later with a reverse
  proxy or `--tls-cert` flag).
- Persistent session storage (file-based or DB); in-memory only for now.
- Multi-agent manifest (one agent per process).
- Full OpenAPI spec generation (can be done by reflecting on types).

## Open questions

1. `cpp-httplib` handles one request per thread; for high concurrency a
   thread pool size should be configurable (`--acp-threads`, default 4).
2. The ACP spec is still evolving (v0.2.0 at time of writing).  Types
   should be kept behind a thin `acp/types.h` layer to absorb spec
   changes without touching handlers.
3. When `pi-cli --acp-port 8080` is used, the agent runs interactively
   AND serves ACP.  The two share one `Agent` instance protected by a
   mutex; the REPL should warn if a run is in progress.
