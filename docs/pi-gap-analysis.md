# Pici and pi architecture gap analysis

This is a comparison of the current pici implementation with the local
`vendor/pi` snapshot. It is intended to guide follow-on work, not to prescribe
a line-for-line port of pi's TypeScript architecture into C++.

## Executive summary

Pici already has a comparable agent-loop kernel: message/content types,
streaming provider events, tool execution, steering, follow-ups, renderers,
session persistence, Lua hooks, ACP, and optional OpenTelemetry.

The largest difference is the product/runtime layer. Pici currently assembles
model resolution, tools, hooks, persistence, rendering, commands, and turn
coordination in `src/main.cpp`, while ACP constructs a separate, weaker runtime.
Pi centralizes those concerns in an `AgentSession` plus session services.

The highest-ROI sequence is:

1. Harden run lifecycle and cancellation.
2. Extract a shared `AgentSession`/`SessionRuntime` for CLI and ACP.
3. Upgrade sessions to entry-level JSONL with durable mutations.
4. Add token budgeting and automatic/manual compaction.
5. Add richer runtime events and a full JSONL RPC control surface.
6. Refactor providers/auth if multi-provider portability is strategic.
7. Improve TUI components later.

## Conceptual mapping

| pi package | pici equivalent | Assessment |
| --- | --- | --- |
| `pi-ai` | `message_types`, `llm_client`, `providers` | Similar abstraction, but pici has only two actual wire clients and env-only authentication. |
| `pi-agent` | `Agent`, `AgentLoop`, `AgentState` | Strong parity: tools, streaming, steering, follow-ups, and cancellation concepts. |
| `pi-coding-agent` | `main.cpp`, built-in tools, Lua, session code | Largest gap: no shared product/runtime façade. |
| `pi-tui` | renderers, readline, tree selector | Pici already has a useful renderer kernel, but less composable UI infrastructure. |
| `pi-orchestrator` / RPC | ACP HTTP server | Pici has useful run plumbing, but not a durable control plane. |

## Ranked gaps

### 1. Shared session runtime

Pici's CLI owns model resolution, tools, hooks, persistence, rendering, slash
commands, and turn persistence in one coordinator. ACP constructs another
`Agent` and uses an in-memory LRU session store.

Pi centralizes this into `AgentSessionRuntime` and cwd-bound services.

The recommended C++ shape is an `AgentSession` or `SessionRuntime` owning:

- the `Agent`;
- durable session management;
- model/provider resolution;
- tools and extensions;
- context management;
- lifecycle/event subscriptions.

CLI and ACP should become adapters over that runtime. This prevents behavior
drift and gives later session, compaction, RPC, and extension work one home.

### 2. Entry-level durable sessions and compaction

Pici's durable store is a session header followed by message lines. Forking is
represented by a parent session plus a message offset. It has no entry IDs,
leaf pointers, model/thinking change entries, custom entries, compaction
entries, or branch summaries.

Pici also persists new messages after a completed turn. A crash during a turn
loses in-progress history, and in-memory transcript truncation from an add-on
is not itself durable.

Pi's session manager stores an entry tree and derives the active context from
the selected leaf. Its compaction implementation uses token estimation,
reserve/keep budgets, safe cut points, and summary entries.

The practical path is to add entry IDs and parent IDs first, then make
compaction a first-class session entry. Do not begin by porting every session
feature from pi.

### 3. Runtime lifecycle and error hardening

Pici's high-level `Agent` catches exceptions around its worker wrapper, but the
low-level loop launches a detached thread. Exceptions escaping that worker,
especially from custom tools, cannot be translated by the high-level catch.

The stop source is also owned by `AgentState`; `abort()` permanently requests
it, while `reset()` does not recreate it. This can make a reused agent remain
cancelled.

Pi creates a fresh abort controller per run, awaits the loop, and converts
failures into normal agent events.

This should be treated as a reliability prerequisite:

- use one joinable run task instead of detaching the inner loop;
- create a fresh stop source per run;
- catch provider and tool exceptions inside the loop;
- always emit a terminal error/abort event;
- expose event subscriptions and streaming-message state.

### 4. Extension/runtime surface

Pici's Lua layer already supports tools, before/after-tool hooks, stop hooks,
commands, completion, prompt-line customization, sub-agents, storage, and
reload.

Pi additionally exposes session lifecycle, context mutation, provider payloads
and headers, model changes, compaction, tree navigation, custom messages and
entries, UI widgets, themes, and resource discovery.

The high-ROI subset is a typed runtime event bus covering prompt preparation,
provider requests, message completion, session lifecycle, forking, and
compaction. A full TypeScript-style extension system is not required.

### 5. Provider registry and credentials

Pici's model catalog contains many OpenAI-compatible entries, but the actual
client registry is narrow. Pi separates provider identity, model lists,
streaming APIs, dynamic refresh, and authentication.

If multi-provider use is strategic, move model definitions out of compiled C++,
make providers register models plus stream factories, and add persistent
credentials. The abstraction and credential store matter more than porting
Pi's full provider list.

### 6. Tool progress and mutation safety

The basic tool inventory is already close: read, bash, edit, write, grep, find,
and ls. Pici's workspace path confinement and edit normalization are useful.

The more valuable differences are operational: pici ignores bash update
callbacks, truncates output without preserving a full-output path, emits
parallel tool-start events too late for live UI, and has no file mutation queue.
Pi has bounded streaming accumulation, truncation metadata, full-output paths,
and serialized file mutations.

This is a good small project after lifecycle hardening.

### 7. TUI infrastructure

Pici already has raw, diff, markdown, and viewport renderers plus readline
separation. Pi's TUI is more composable and includes a component tree,
overlays, editor/autocomplete infrastructure, themes, and terminal image
support.

This is not the next major investment unless interactive UI polish is the
primary product goal. Session reliability and context management have higher
return.

## Recommended implementation order

### Phase 1: lifecycle hardening

Keep the public `Agent` API stable where possible. Replace the detached inner
loop with a joinable run abstraction, introduce per-run cancellation, and make
all provider/tool failures settle the stream with a terminal event. Add focused
tests for abort, reuse after abort, tool exceptions, provider exceptions, and
exactly-once terminal events.

### Phase 2: shared runtime

Extract the setup and coordination currently in `main.cpp` into a reusable
runtime object. Adapt the CLI renderer and ACP event transport to it instead of
creating separate agent/session paths.

### Phase 3: durable context model

Add entry IDs, parent links, explicit leaf selection, durable transcript
mutations, and then context budgeting/compaction. Preserve the existing JSONL
format through a versioned migration rather than silently changing it.

### Phase 4: integration surface

Expose prompt, steer, follow-up, abort, state, session switching, fork, and
event streaming through a JSONL RPC protocol. Consider orchestration and
multi-process supervision only after this shared runtime exists.
