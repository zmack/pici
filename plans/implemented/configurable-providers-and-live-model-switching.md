# Configurable providers, custom models, and live model switching

## Status and intended reader

This is an implementation plan for Luna. It is deliberately execution-ready:
it defines the configuration schema, runtime invariants, resolution rules,
authentication boundaries, session-journal behavior, CLI/RPC/ACP surfaces,
cross-provider transcript handling, test matrix, rollout order, and completion
criteria.

The target feature has two related parts:

1. Let one `config.toml` describe multiple providers and custom models while
   retaining the built-in model catalog.
2. Let a durable session change its active provider/model between turns without
   losing its transcript or leaking credentials across providers.

Provider/model switching means switching **between turns**. It does not mean
changing the model after a request has started streaming, while tool calls from
that request are still being executed, or while queued steering/follow-up work
belongs to the active run.

## Repository state and coordination warning

At the time this plan was written, the working tree already contained extensive
uncommitted work for `openai-codex` authentication and Responses support. That
work edits several files this feature also needs, including:

- `src/main.cpp`
- `src/acp/main.cpp`
- `src/cli/args.{h,cpp}`
- `src/cli/config.cpp`
- `src/core/agent.{h,cpp}`
- `src/core/agent_loop.{h,cpp}`
- `src/core/llm_client.h`
- `src/core/models.cpp`
- provider and HTTP implementations
- `test/test_config.cpp`

Treat those changes as user-owned and as the implementation baseline. Do not
reset, discard, or recreate them from `HEAD`. Review the current diff before
editing and preserve the new `RequestAuth`, credential-store, OAuth, and
`openai-codex-responses` behavior.

The existing `build/test-config` binary passed 34/34 tests during planning.
That is only a baseline observation, not a substitute for rebuilding and
running the final test suite.

Follow `AGENTS.md` throughout:

- Use `make dev` or a narrow target during the edit/compile loop.
- Run `make format` before final verification.
- Run `make lint` and treat new warnings in touched code as actionable.
- Before each commit, run `make test`, or the equivalent full CMake build and
  CTest commands.
- Do not add decorative section-divider comments.
- Preserve terminal-state RAII rules.
- Do not commit unless explicitly requested.

## Executive design decisions

Implement one immutable, process-wide effective provider/model catalog and one
mutable active model per `AgentSession`.

The catalog is assembled at startup from:

1. Built-in provider definitions.
2. Built-in model definitions.
3. Provider overrides from `config.toml`.
4. Custom models from `config.toml`.

The active selection is stored as a full `core::Model` in `AgentState`, as it is
today. Every new turn snapshots that model and asks `LLMClientRegistry` for the
client matching `model.api`. Existing in-flight turns retain their immutable
snapshot.

Use canonical `(provider_id, model_id)` pairs internally. A slash-joined string
is only user-facing syntax; it must not become the storage key because model IDs
such as `anthropic/claude-*`, `accounts/fireworks/models/*`, and local model IDs
can legitimately contain slashes.

Provider credentials are resolved by provider at request time. A CLI API key
must be associated with the provider selected when the key is supplied; it must
never become a process-wide key returned for every later provider.

Session persistence must distinguish:

- The provider/model attached to each historical assistant message.
- The provider/model selected to handle the **next** turn.

Historical messages already carry `api`, `provider`, and `model`. Add a
replayable session metadata record for the current selection.

## Non-goals

Do not expand this feature into:

- Implementing arbitrary new wire APIs. Initially, configurable providers may
  use only API IDs registered in the C++ binary.
- Downloading or refreshing remote model catalogs.
- Hot-reloading `config.toml` while the process is running.
- Automatic provider failover or round-robin routing.
- Switching during an active request or tool-execution phase.
- Sharing one active-model setting across all ACP sessions.
- Executing shell commands from config values.
- Persisting API keys, OAuth tokens, or resolved headers in session files.
- Rewriting the model catalog generator or importing the full vendored
  TypeScript model-runtime architecture.

The initially supported wire API IDs are those actually registered by the
binary at implementation time. On the current working tree they are:

- `openai-completions`
- `openai-codex-responses`
- `muse-messages`

If another client is added before this work lands, expose it through the same
registry validation rather than hard-coding this list in the TOML parser.

## Current implementation and reusable seams

The runtime already contains most of the low-level switching machinery:

- `core::Model` includes `id`, `api`, `provider`, `base_url`, capability,
  pricing, header, and thinking metadata.
- `AgentState::model()` and `AgentState::set_model()` are synchronized.
- `Agent::create_loop_config()` reads `state_.model()` and calls
  `LLMClient::create(config.model)` for every new run. A later turn can
  therefore use a different client without reconstructing the `Agent`.
- Provider request builders call `transform_messages()` using the destination
  model.
- `transform_messages()` retains signed/encrypted thinking only when historical
  assistant output has the same provider, API, and model. Cross-model readable
  thinking becomes plain text, redacted state is dropped, images can be
  downgraded, and tool-call IDs can be normalized.
- Every `AssistantMessage` stores the actual API/provider/model that produced
  it.
- `AgentTaskManager` already begins a child with the parent context's current
  model, although explicit child model resolution currently uses only the
  built-in catalog.
- RPC `get_state` already returns the current model, and RPC already enforces an
  idle-agent rule for session mutations.

The important deficiencies are:

- `load_config()` returns `Args`, so TOML config and CLI occurrences are
  flattened into one singular model/provider/base URL/API key.
- `all_models()` is a static built-in vector with no configured provider layer.
- `find_model()` contains increasingly fragile slash heuristics and silently
  creates fallback models.
- `pi-cli` and `pi-acp` duplicate model-resolution logic.
- The CLI captures one startup `model` variable in UI, hook, pricing, session,
  and child-task paths. Those captures become stale after switching.
- `args.api_key` is captured as a universal fallback even though a key belongs
  to one provider.
- `Model::headers` is not consistently merged by all providers.
- Session headers record only the creation-time model; resume does not restore
  the most recently selected model.
- ACP creates a fresh `AgentSession` per run, so it must restore the current
  model from the durable session journal on each request.

## Configuration schema

### Backward-compatible default selection

Keep the existing `[model]` table as the default startup selection:

```toml
[model]
provider = "openai"
id = "gpt-4.1"
```

Continue accepting the legacy selection-local fields during migration:

```toml
[model]
provider = "local"
id = "qwen3-coder"
base_url = "http://127.0.0.1:8080/v1"
api_key = "legacy-literal-key"
```

Treat `base_url` and `api_key` here as overrides for the selected startup
provider/model only. They must not mutate every catalog entry for that provider.
Document `model.api_key` as legacy and discourage it, but do not silently break
existing configs.

### Provider definitions

Add named provider tables:

```toml
[providers.openai]
api = "openai-completions"
base_url = "https://api.openai.com/v1"
api_key_env = "OPENAI_API_KEY"

[providers.local]
api = "openai-completions"
base_url = "http://127.0.0.1:8080/v1"
auth = "none"

[providers.proxy]
api = "openai-completions"
base_url = "https://proxy.example.test/v1"
api_key_env = "PROXY_API_KEY"
headers = { X-Tenant = "engineering" }
```

Provider fields:

| Field | Type | Required | Meaning |
|---|---|---:|---|
| `api` | string | For a new provider | Registered wire API ID inherited by its models |
| `base_url` | string | For a new network provider | Endpoint root inherited by its models |
| `api_key_env` | string | No | Name of an environment variable captured into the immutable auth environment at startup |
| `api_key` | string | No | Literal key retained for compatibility/convenience; warn in docs that TOML is plaintext |
| `auth` | string | No | `"required"`, `"optional"`, `"none"`, or `"oauth"`; see authentication policy |
| `headers` | table of strings | No | Provider-level request headers, excluding protected auth headers |

Rules:

- Provider IDs are the TOML child-table keys and must be non-empty.
- Reject a provider with both `api_key` and `api_key_env`.
- Reject new providers missing `api` or `base_url`, except an explicitly
  supported special provider whose registered client owns its endpoint.
- A provider override may omit `api` and/or `base_url` when the built-in
  provider definition supplies them.
- Validate `api` against `LLMClientRegistry` after clients are registered.
- Reject header values that are not strings.
- Reject or ignore with an explicit diagnostic any attempt to configure
  `Authorization`, `Proxy-Authorization`, `Cookie`, or provider-protected OAuth
  headers in ordinary provider/model headers. Authentication resolution owns
  those values and wins case-insensitively.
- Do not evaluate commands from `api_key`, `api_key_env`, or headers.
- Read configured environment-variable names before worker threads start and
  retain their values in the same immutable/cache discipline as
  `env_api_keys.cpp`. Do not add arbitrary per-request `std::getenv()` calls.
- `auth = "oauth"` is valid only for a provider supported by `AuthResolver`.
- `openai-codex` remains OAuth-only. Reject its configured `api_key`,
  `api_key_env`, legacy `[model].api_key`, or `--api-key` path.

### Custom model definitions

Add arrays of model tables nested under a provider:

```toml
[[providers.local.models]]
id = "qwen3-coder"
name = "Qwen 3 Coder (Local)"
context_window = 131072
max_tokens = 16384
reasoning = true
input_capabilities = ["text"]

[[providers.local.models]]
id = "vision-model"
api = "openai-completions"
input_capabilities = ["text", "image"]
headers = { X-Model-Route = "vision" }

[providers.local.models.cost]
input_per_mtok = 0.0
output_per_mtok = 0.0
cache_read_per_mtok = 0.0
cache_write_per_mtok = 0.0
```

Because TOML has no null value, represent explicitly unsupported thinking
levels with `false` and mapped values with strings:

```toml
thinking_level_map = { off = false, minimal = "low", low = "low", medium = "medium", high = "high", xhigh = false }
```

Model fields and defaults:

| Field | Type | Default |
|---|---|---|
| `id` | string | Required |
| `name` | string | `id` |
| `api` | string | Provider `api` |
| `base_url` | string | Provider `base_url` |
| `reasoning` | bool | `false` |
| `input_capabilities` | string array | `["text"]` |
| `context_window` | positive integer | `128000` |
| `max_tokens` | positive integer | `4096` or an agreed project default |
| `headers` | string table | Empty; merged above provider headers |
| `cost.*_per_mtok` | number | `0.0` |
| `thinking_level_map` | string/false inline table | Empty/default provider behavior |

Validation rules:

- Reject missing or empty `id`.
- Reject zero, negative, or overflowing context/output limits.
- Accept only `text` and `image` input capabilities initially; deduplicate
  while preserving deterministic order.
- Reject `reasoning = false` combined with a non-empty thinking map.
- Validate thinking keys against every `core::ThinkingLevel` value.
- Validate model-level `api` through the client registry.
- Apply the same protected-header rules as provider headers.
- Reject duplicate custom model IDs within the same provider rather than
  allowing last-one-wins accidents.

### Merge semantics

Build the effective catalog deterministically:

1. Seed explicit built-in provider definitions.
2. Copy the built-in model catalog.
3. Apply a configured provider's `api`, `base_url`, and headers to every model
   with that provider ID.
4. Upsert each configured custom model by exact `(provider, id)`.
5. Sort only for display. Preserve stable catalog construction order for tests,
   but never use order to resolve ambiguity.

When a custom model matches a built-in `(provider, id)`, treat it as a complete
model replacement with provider defaults filled in. Do not accidentally erase
critical built-in metadata by interpreting a sparse replacement as a complete
object. Implement either:

- a distinct `ModelOverride` type with `std::optional` fields and merge it into
  the built-in model, or
- two explicit TOML concepts (`models` for additions and `model_overrides` for
  sparse overrides).

Prefer the second, less ambiguous schema if implementation remains manageable:

```toml
[providers.openai.model_overrides."gpt-4.1"]
context_window = 200000

[[providers.openai.models]]
id = "company-fine-tune"
name = "Company Fine Tune"
```

If `model_overrides` is implemented:

- Ignore no unknown IDs silently; emit a config error or actionable warning.
- Merge cost and headers per key.
- Replace scalar/vector fields only when explicitly present.
- Apply overrides before custom-model additions.

### Configuration representation

Stop using `Args` as the parsed TOML document. Add a separate representation,
for example:

```cpp
struct ApiKeyConfig {
  std::optional<std::string> literal;
  std::optional<std::string> env_var;
};

enum class ProviderAuthPolicy { required, optional, none, oauth };

struct ProviderConfig {
  std::string id;
  std::optional<std::string> api;
  std::optional<std::string> base_url;
  ApiKeyConfig api_key;
  std::optional<ProviderAuthPolicy> auth;
  std::map<std::string, std::string> headers;
  std::vector<ConfiguredModel> models;
  std::map<std::string, ModelOverride> model_overrides;
};

struct Config {
  Args defaults;
  std::map<std::string, ProviderConfig> providers;
  std::vector<Args::Diagnostic> diagnostics;
};
```

Exact names may differ, but preserve the separation:

- TOML document and provider catalog inputs.
- CLI occurrences and CLI-only commands.
- Effective runtime options after precedence is applied.

Do not hide malformed provider/model entries behind the current "unknown keys
are silently ignored" policy. Existing unrelated unknown top-level settings may
remain ignored for compatibility, but recognized provider/model structures must
be type-checked and diagnosed with a TOML path such as
`providers.local.models[1].context_window`.

## Provider and model registry

### Core types

Add an immutable registry owned by a `std::shared_ptr<const ModelRegistry>`:

```cpp
struct ProviderDefinition {
  std::string id;
  std::string api;
  std::string base_url;
  ProviderAuthPolicy auth;
  ApiKeyConfig api_key;
  std::map<std::string, std::string> headers;
};

struct ModelKey {
  std::string provider;
  std::string id;
};

class ModelRegistry {
public:
  const std::vector<Model> &models() const;
  const ProviderDefinition *provider(std::string_view id) const;
  const Model *exact(std::string_view provider, std::string_view id) const;
  ModelResolution resolve(const ModelSelection &selection) const;
  std::vector<const Model *> search(std::string_view filter) const;
};
```

The registry should own its models so pointers/references remain stable for its
lifetime. Callers that persist a selection should copy the `Model`, matching
the current `AgentState` behavior.

Seed built-in provider definitions explicitly rather than inferring provider
metadata from the first model found in `kModels`. First-model inference is
order-dependent and fails if models for one provider use different API types.
It is acceptable to keep the built-in providers next to the model catalog in
`models.cpp` initially, but expose them as a separate collection.

### API registry validation

Add a read-only `LLMClientRegistry::has_client(api_id)` or equivalent. Client
registration occurs before config/model-registry construction in both
executables. Validate all effective models once at startup so an invalid API
does not produce a delayed `No LLM client configured` error on the first
request.

Tests may register faux API IDs before constructing a registry.

### Model resolution rules

Replace ad hoc `find_model()` behavior for application selection with one
resolver shared by CLI, RPC, ACP, and child agents. Keep a compatibility wrapper
temporarily if low-level tests or callers still need `find_model()`.

Inputs are:

- optional explicit provider
- optional model string
- optional base URL override
- source (`cli`, `config`, `session`, `rpc`, `acp`, `child`), for diagnostics

Resolution is exact and deterministic:

1. If an explicit provider is supplied, treat the model string as a complete
   model ID. For convenience, strip one matching `<provider>/` prefix if
   present.
2. Look for exact `(provider, id)`.
3. If not found but the provider exists, construct a fallback model from the
   provider definition using that full ID. This preserves first-use support for
   newly released models without catalog churn.
4. Without an explicit provider, first test whether the prefix before the first
   slash is a known provider **and** the remaining ID resolves for it. Prefer
   that canonical interpretation.
5. Otherwise test the entire string as a raw model ID across every provider.
6. If exactly one model matches, use it.
7. If more than one matches, return an ambiguity error listing canonical
   `provider/id` choices. Never choose based on catalog order.
8. If the string started with a known provider but no canonical match existed,
   allow an exact raw-ID match such as an OpenRouter model whose ID begins with
   another provider name.
9. Unknown providers are errors unless an explicit base URL is also supplied;
   the legacy arbitrary endpoint path may construct an ephemeral
   `openai-completions` provider/model for that selection only.
10. Empty model IDs are errors except for the existing no-flags local fallback,
    if that behavior is intentionally retained.

Provider IDs should be canonical lowercase ASCII in built-ins and examples.
Decide once whether lookup is case-sensitive. Prefer case-insensitive provider
lookup with the canonical configured spelling returned, and case-sensitive
model IDs because upstream APIs may distinguish them.

### Startup-selection precedence

For a new session:

1. Explicit CLI `--provider`/`--model`/`--base-url`.
2. Config `[model]` default.
3. Existing local `default` fallback.

For `--continue`, `--resume`, RPC `switch_session`, or ACP requests with an
existing session:

1. Explicit request/CLI provider/model selection, if supplied.
2. Persisted current model metadata from the session journal.
3. Config `[model]` default.
4. Existing local fallback.

If a persisted model no longer exists:

- If its provider still exists, construct the provider fallback model with the
  saved ID and warn.
- Otherwise warn and fall back to config/default selection.
- Never reinterpret the saved model ID under a different provider merely
  because another provider has the same ID.

## Authentication and header policy

### Provider-indexed authentication

Extend authentication resolution so it can consult the effective provider
definition:

```cpp
resolve(provider_id, runtime_overrides, provider_config, stop_token)
```

Recommended precedence:

1. Provider-specific protocol credentials, such as `openai-codex` OAuth.
2. A runtime key override associated with this exact provider.
3. The provider's configured literal key.
4. The provider's configured `api_key_env` value from the startup environment
   snapshot.
5. Existing built-in environment-variable lookup from `env_api_keys.cpp`.
6. No auth when policy is `optional` or `none`.
7. An actionable missing-auth error when policy is `required` or `oauth`.

At startup, resolve the selected model before handling `--api-key`, then store
the CLI key in an in-memory map keyed by `selected_model.provider`. If the user
switches providers, that key must not follow them. Switching back within the
same process may reuse the override.

Never serialize that runtime map. Avoid capturing the startup `model` by
reference in auth callbacks; use the provider argument passed by the agent loop.

`openai-codex` policy remains special:

- Reject `--api-key` and TOML API-key configuration.
- Resolve/refresh OAuth only for `openai-codex` requests.
- Never pass its bearer token to `openai`, a custom OpenAI-compatible endpoint,
  or a configured proxy.

### Availability versus request-time resolution

The model selector should be able to indicate whether a model appears usable
without printing or resolving secrets. Add a safe provider-auth status query:

- `configured`: a runtime/config/env/OAuth credential source appears present.
- `not_required`: provider policy is `none` or `optional`.
- `missing`: required auth has no configured source.
- `expired_or_refresh_needed`: OAuth exists but needs request-time refresh.

Do not run shell commands (none are supported) or refresh OAuth merely to render
the selector. The actual request continues to resolve and refresh auth with a
stop token.

Selection may allow a model with missing auth so users can inspect it, but the
UI must mark it unavailable and direct selection must return a useful error
before changing session state. Local `auth = "none"` providers remain usable.

### Header precedence

Apply headers case-insensitively in this order, lowest to highest:

1. Provider-config headers.
2. Model-config/model-override headers.
3. Application/request `Agent::Options.headers`.
4. Provider-required and authentication headers from `RequestAuth`.

Fix all clients to consume the same effective model/request header merge.
Currently `Model::headers` is not uniformly used outside the Codex Responses
client. Prefer one shared merge helper rather than reproducing precedence in
every provider.

Do not log effective headers in verbose output or diagnostics.

## Safe live switching contract

### Core API

Do not let application code mutate `AgentState::model()` directly for this
feature. Add an `Agent` or `AgentSession` operation that enforces the lifecycle,
for example:

```cpp
struct ModelSwitchResult {
  Model previous;
  Model current;
  ThinkingLevel thinking_level;
  std::optional<std::string> warning;
};

ModelSwitchResult AgentSession::set_model(Model model,
                                          ThinkingLevelPolicy policy);
```

Required invariants:

- Acquire the same lifecycle synchronization used by `begin_run()` so the
  idle check and state mutation cannot race with prompt startup.
- Reject switching if the agent is streaming.
- Reject switching while queued/pending tool execution belongs to an active
  turn. In normal CLI operation this follows from the streaming state, but keep
  the core invariant explicit.
- Decide queued steering/follow-up behavior conservatively: reject switching
  while either queue is non-empty, or document and test that queued messages
  use the newly selected model. Prefer rejection because the user queued them
  against the previous active run.
- Apply the complete resolved `Model` and normalized thinking level in one
  state mutation. Do not expose an intermediate new-model/old-thinking pair to
  prompt snapshots or observers.
- Do not mutate the transcript.
- The next `Agent::prompt()` snapshots the new model and creates its client.
- An in-flight turn always retains its old model/client/auth snapshot.

If exposing queue state is invasive, implement `Agent::set_model()` under
`worker_mutex_`, reject only `state_.is_streaming()`, and add tests proving a
switch cannot race a prompt. Do not perform a check followed by an unlocked
`state_.set_model()`.

### Thinking-level policy

Switching models may invalidate the current thinking level. Add one shared
helper used by startup and live switching:

```cpp
ThinkingLevelResolution resolve_thinking_level(const Model &model,
                                                ThinkingLevel requested);
```

Policy:

- A non-reasoning model uses `off`.
- If the exact level is supported by `thinking_level_map`, preserve it.
- A map entry of `false`/`nullopt` is unsupported.
- When no map exists, preserve the provider's currently supported standard
  behavior; do not invent `xhigh` support.
- If the requested level is unsupported, clamp to the nearest supported level
  at or below it, falling back to `off` when supported.
- Return a warning whenever the level changes automatically.

If provider implementations currently differ on this behavior, centralize the
capability decision without removing their wire-value mapping.

### Cross-provider transcript conversion

Keep `transform_messages()` as the authority for replaying historical content
to a destination model. Add explicit cross-provider tests covering:

- Visible text survives unchanged.
- Signed thinking from provider A is not sent as signed thinking to provider B.
- Readable thinking becomes ordinary text where appropriate.
- Redacted/encrypted reasoning is retained only for the exact same
  provider/API/model.
- Images become placeholders when switching to a text-only model.
- Tool calls and results retain valid ordering and IDs.
- Error/aborted assistant messages are not replayed as successful context.
- Switching away and then back does not resurrect credentials or opaque state
  in the wrong intervening request.

Do not permit a switch in the middle of an assistant tool-use turn. By the time
the switching API succeeds, every emitted tool call must have its corresponding
tool result or the existing normalization path must synthesize one.

### Context-window changes

A session may switch from a large-context model to a smaller one. The current
token estimate is approximate, and addons may compact context in
`prepare_context`, so do not reject a switch solely from the raw transcript
estimate.

Instead:

1. Compute an approximate raw context estimate when selecting and warn if it
   exceeds the destination `context_window`.
2. Continue to run existing transform/prepare-context hooks on the next turn
   using the destination model.
3. After context preparation, enforce the destination context limit if a
   reliable estimate is available. Return an actionable error naming the model,
   estimated size, and limit instead of sending a predictably oversized
   request.
4. Do not silently truncate durable transcript history as part of switching.

If post-prepare enforcement is beyond this feature's reliable tokenizer
capability, keep the warning and provider error behavior, but isolate the
estimate in a reusable function and add a follow-up note. Do not claim exact
token accounting from the current byte-based estimator.

## Session persistence and resume

### Journal record

Add a session metadata operation, for example:

```json
{"type":"meta","timestamp":1722912345,"provider":"openrouter","model":"anthropic/claude-sonnet-4-5"}
```

The record contains only canonical provider and model IDs. Do not persist API,
base URL, headers, keys, credential source, or a serialized `Model`; the current
registry rehydrates those properties on resume.

Add `SessionStore::set_model(session_id, provider, model)` and append the record
under the existing store mutex. `AgentSession::set_model()` must coordinate the
durable write and in-memory transition as one lifecycle operation. Define
failure semantics:

- Use one session/agent lifecycle transition shared with prompt startup. After
  resolving and copying all potentially throwing inputs, acquire the transition
  lock, recheck idle, append the journal record, then move the prepared
  model+thinking state into `AgentState` before releasing the lock. If the
  journal append fails, state remains unchanged. If this requires a small
  `Agent::with_idle_transition(...)` or `AgentState::set_model_and_thinking(...)`
  seam, add it rather than leaving a check/persist/set race.
- The caller must never report success when state and durable journal disagree.
- Keep a memory-only switch available for `AgentSession` instances without a
  store, such as tests or ephemeral children.

### Loading and listing

While loading a session:

- Start with the creation header's provider/model.
- Apply later metadata records in file order; the last valid model record is
  the selected model for the next turn.
- Continue applying name and truncate metadata as today.
- Ignore malformed metadata records without crashing the entire session only
  if that matches existing tolerant journal policy; otherwise return an
  actionable corruption error. Be consistent and test it.

`SessionStore::list()` currently reads only the first line. Update its metadata
scan or clearly separate `created_model` from `current_model`. The session tree
and resume diagnostics should show the current provider/model, not stale
creation metadata.

Old session files remain valid:

- If no model metadata records exist, use header provider/model.
- If those fields are absent, inspect the latest usable `AssistantMessage` only
  as a compatibility fallback, then use config default.
- Never rewrite old session files merely by loading them.

### Truncation and forks

Model selection is session metadata, not transcript history. A transcript
truncate does not roll back model selection. Document and test that behavior.

A fork inherits the parent's current provider/model at fork time and writes it
into the child creation header. Later model switches in parent and child are
independent.

`/new` uses the current model as the new session's initial selection unless an
explicit new-session model option is later added.

### Resume behavior

`AgentSession::activate_session()` must accept or have access to the shared
registry and apply the restored selection while idle. It should restore model,
messages, session ID, and session name as one coherent operation. Avoid the
current behavior where activation replaces messages but leaves the process
startup model in state.

If restoration falls back because provider/model config changed, display one
warning and persist no replacement metadata until the user explicitly switches
or successfully starts a turn with the fallback. Avoid silently rewriting user
history during read-only session listing.

## CLI behavior

### Commands

Add built-in commands:

```text
/model
/model <provider/model-id>
/models [filter]
```

Behavior:

- `/model` in a TTY opens a model selector showing provider, model ID, context
  size, reasoning/image capabilities, and auth availability. Preselect the
  current model.
- `/model` in non-interactive input prints the current canonical selection and
  usage instead of attempting terminal control.
- `/model <spec>` resolves and switches directly.
- `/models [filter]` prints the effective configured+built-in catalog using the
  existing model-table formatting.
- `--list-models` must also use the effective registry.
- A successful switch prints `model: provider/id` through the renderer command
  output path.
- A failed switch leaves state and session journal unchanged.

The selector may reuse the interaction patterns from `tree_selector`, but keep
model selection in a dedicated component such as
`src/cli/model_selector.{h,cpp}`. Preserve terminal RAII and delete ownership-
duplicating copy/move operations.

Add command and argument completion:

- Complete `/model`, `/models`, and existing built-ins.
- For `/model ` arguments, complete canonical provider/model strings from the
  effective registry before delegating unknown commands to addon completion.
- Do not split or truncate slash-heavy model IDs.

### Eliminate stale startup-model captures

After constructing `AgentSession`, audit every use/capture of the local startup
`model` in `src/main.cpp`. Use `agent.state().model()` or a request-local copy
for:

- Prompt-line hook model ID.
- `LuaUiContext.model` and any provider/API fields added later.
- Hook `configure` information.
- Verbose model output.
- `/new` and `/fork` headers.
- Cost/pricing availability.
- Child-agent default model and model resolution.
- Authentication callbacks.
- Status/tab-title refresh.

Do not keep a reference returned from `state().model()`; the accessor returns a
copy intentionally.

After switching:

1. Update agent/session state and durable metadata.
2. Re-run hook configuration with the new model.
3. Recompute status line and terminal title.
4. Clear only request-local effective-context diagnostics; do not clear the raw
   transcript or accumulated usage.
5. Refresh current pricing/capability display.

Accumulated session cost remains a sum of each message's already-computed cost,
even across providers. `has_pricing` must not remain a boolean captured from the
startup model. Last-turn display should reflect the model that produced that
turn; current-model display should reflect the model selected for the next
turn.

## RPC behavior

Add commands:

```json
{"id":"1","type":"list_models","filter":"gpt"}
{"id":"2","type":"set_model","provider":"openai","model":"gpt-4.1"}
```

Also accept one canonical `model` spec without `provider` if that is easier for
clients, but do not create two ambiguous wire formats. Prefer explicit provider
and model fields for protocol stability.

Rules:

- `list_models` returns effective model metadata and safe auth availability,
  never credentials.
- `set_model` requires an idle agent using the same atomic lifecycle check as
  CLI.
- Reject while `run_active_` or streaming.
- Persist the switch through `AgentSession`.
- Return previous/current model and effective thinking level.
- Emit or return an automatic thinking-clamp warning.
- `get_state` already reports current model and should require no schema break.
- `new_session`, `switch_session`, and `fork` must use/restore the correct
  session-specific model.

Add RPC tests for successful switching, unknown/ambiguous models, active-run
rejection, persistence, session switching, and cross-provider second prompts.

## ACP behavior

ACP currently constructs a fresh `AgentSession` for every `/runs` request,
including requests that reuse a durable `session_id`. Therefore live model
selection must be request- and session-aware rather than stored in
`ServerConfig::agent_opts.model` alone.

Extend `RunCreateRequest` with optional selection fields, preferably:

```json
{
  "agent_name": "pici",
  "session_id": "...",
  "provider": "openrouter",
  "model": "anthropic/claude-sonnet-4-5",
  "input": [...]
}
```

Rules:

- `ServerConfig` owns the shared immutable model registry and auth runtime.
- For a new run without selection, use config default.
- For an existing session without selection, restore the journal's current
  model.
- Explicit request selection overrides the restored model and is persisted
  before the run begins.
- Resolve/authenticate per provider exactly as CLI does.
- Return the effective provider/model in run metadata so clients can verify
  routing.
- Concurrent runs against the same durable session are already a broader
  consistency concern. At minimum, prevent two simultaneous model metadata
  updates/runs from silently racing through the store. Reuse store/session
  synchronization or return a conflict if per-session run locking is not
  already implemented.

If changing the ACP request schema is deemed outside the immediate release,
the minimum acceptable phase is correct restoration of persisted session model
and shared configurable defaults. However, the plan is not fully complete for
"switch providers halfway through a session" until ACP has an explicit
per-run selection surface.

## Child agents

Pass the shared immutable registry into `AgentTaskManager` rather than calling
global `find_model()`.

- Default child model is the parent's current model at spawn time.
- Explicit child model selection uses the same resolver and ambiguity rules.
- A child switch does not change its parent or siblings.
- Child auth callbacks already receive a provider argument; ensure they use the
  provider-indexed runtime and do not capture the root's startup model/key.
- Configured custom models must be usable by child agents.
- Cross-provider inherited history goes through the same message transform.

The existing child task is in-memory and has no independent durable session
store, so its model switch need not emit root-session metadata unless/ until
child session persistence is introduced.

## Suggested file ownership

The exact split may change, but keep responsibilities clear:

- `src/cli/config.{h,cpp}`
  - Parse `Config`, providers, configured models, overrides, and diagnostics.
  - Merge ordinary config defaults with explicit CLI options.
- `src/core/models.{h,cpp}` or new `src/core/model_registry.{h,cpp}`
  - Built-in provider definitions.
  - Effective catalog construction.
  - Exact resolution/search and thinking-level capability helpers.
- `src/core/llm_client.{h,cpp}`
  - Read-only registered-API validation.
- `src/core/auth/auth_resolver.{h,cpp}`
  - Provider-indexed runtime/config/env/OAuth resolution.
- `src/core/agent.{h,cpp}` and `src/core/agent_state.h`
  - Atomic idle-only model transition.
- `src/core/session/session_{record,store}.{h,cpp}`
  - Current-model journal metadata and replay.
- `src/core/session/agent_session.{h,cpp}`
  - Coherent switch/persist/restore operation.
- `src/cli/model_selector.{h,cpp}`
  - TTY selector and display model.
- `src/main.cpp`
  - Shared registry wiring, commands, completion, hooks, UI refresh.
- `src/cli/rpc_mode.{h,cpp}`
  - List/set model protocol.
- `src/acp/types.{h,cpp}`, `src/acp/server.h`, `src/acp/handlers.cpp`,
  `src/acp/main.cpp`
  - Per-run model selection and restoration.
- `src/core/agent_task.{h,cpp}`
  - Shared-registry child resolution.
- Provider request code or a shared header helper
  - Consistent provider/model/request/auth header merge.
- `config.toml.example`, `README.md`
  - User-facing schema and switching behavior.
- `CMakeLists.txt`
  - New source and focused test targets.

Avoid placing all registry, parsing, UI, and auth logic into `src/main.cpp`.

## Test plan

### Config parser tests

Expand `test/test_config.cpp` or split provider/model parsing into a focused
test executable. Cover:

- Existing singular `[model]` config remains valid.
- Two providers with independent base URLs and auth sources.
- Built-in provider override without restating models.
- Multiple custom models under one provider.
- Same model ID under two providers.
- Model IDs containing one and multiple slashes.
- Sparse built-in model override retains unspecified metadata.
- Provider and model header merge.
- Literal key versus environment-variable source.
- Mutual exclusion of `api_key` and `api_key_env`.
- OAuth provider rejects API-key configuration.
- Missing new-provider API/base URL.
- Unknown/unregistered API.
- Duplicate provider-model definitions.
- Wrong TOML types with full-path diagnostics.
- Invalid auth policy.
- Invalid/duplicate capabilities.
- Invalid numeric limits and overflow.
- Invalid thinking keys/value types.
- Legacy config plus CLI precedence.
- CLI `--api-key` attaches only to selected provider.

Do not read real developer credentials in tests. Use unique test environment
variables set before the resolver cache initializes, or inject an environment
lookup seam.

### Registry and resolver tests

Add focused tests for:

- Built-in catalog unchanged without provider config.
- Provider overrides affect all provider models.
- Custom addition and sparse override semantics.
- Exact explicit provider resolution.
- Matching redundant `provider/model` prefix with explicit provider.
- Unique bare ID resolution.
- Ambiguous bare ID produces sorted choices.
- Canonical provider prefix versus raw slash-containing ID.
- Unknown model fallback under a known provider.
- Unknown provider plus explicit base URL compatibility.
- Unknown provider without base URL error.
- Case policy for providers and models.
- Search/list includes configured models.
- Registered API validation using faux clients.
- Stable results independent of catalog ordering.

### Agent switching tests

Use faux clients registered under two different API IDs. Record the model,
provider, API, auth source, headers, and transformed context seen by each call.
Cover:

- First turn uses provider A; second turn uses provider B.
- Client factory changes with API ID.
- Model switch is rejected during an active stream.
- Prompt startup cannot race the switch check/state update.
- Failed resolution/auth/persistence leaves the old model active.
- Switching back to A works.
- Provider A key is never supplied to B.
- Provider/model/request/auth header precedence.
- Thinking level preservation and clamp warnings.
- Cross-provider signed/reasoning/image/tool history normalization.
- Usage/cost from both turns remains accumulated correctly.
- Current UI/hook context reports B after switching.

Avoid real network calls and paid providers.

### Session-store tests

Cover:

- Creation header model loads when no metadata exists.
- Later model metadata wins.
- Multiple switches replay in order.
- Model IDs with slashes round-trip unchanged.
- `list()` reports current model.
- Old session without model metadata remains valid.
- Missing header fields compatibility fallback.
- Switch persistence failure does not report success or leave divergent state.
- Truncate preserves current model selection.
- Fork inherits the current model and later diverges independently.
- Resume restores provider/model before the next request.
- Unavailable saved provider/model follows the documented fallback and warning.

### CLI tests

Use subprocess/faux-provider fixtures where practical:

- `--list-models` shows configured entries.
- Startup selection precedence.
- `/model <spec>` changes the second turn's provider.
- `/model` selector cancel leaves state unchanged.
- Missing auth and ambiguous selection diagnostics.
- Slash command completion for slash-heavy IDs.
- `/new`, `/fork`, `/tree`, and resume preserve/restore current model.
- Prompt/status/hook model values change immediately.
- Mixed-provider `/usage` remains accurate.
- Non-TTY `/model` does not enter raw mode.

### RPC tests

Extend `test/test_rpc_mode.cpp` for:

- `list_models` response.
- `set_model` success payload.
- Active-run rejection.
- Unknown and ambiguous resolution.
- Thinking clamp response.
- Durable switch and session reactivation.
- Provider A first prompt/provider B second prompt.
- Provider-specific auth and headers.

### ACP tests

Extend `test/test_acp.cpp` for:

- Configured default model.
- Explicit per-run model on a new session.
- Explicit provider switch on the second run with the same session ID.
- Third run without explicit selection restores the second run's model.
- Effective model in run response metadata.
- Unknown/ambiguous/unavailable selection returns 4xx without starting a run.
- Concurrent mutation policy returns deterministic conflict or serializes.
- No credentials appear in JSON or SSE.

### Regression tests for existing providers

Keep all existing OpenAI-compatible, Muse, OpenAI Codex, agent-loop, session,
child-agent, and auth tests green. In particular verify:

- Existing `OPENAI_API_KEY` behavior.
- Existing unauthenticated local endpoint behavior.
- `openai-codex` OAuth refresh and header isolation.
- OpenRouter/raw slash model IDs.
- Fireworks slash-heavy IDs.
- Muse encrypted-thinking replay.
- Existing CLI-only `--base-url --model` invocation.

## Implementation phases

Keep phases independently reviewable. Run focused tests during each phase and
the full quality gate before any commit.

### Phase 0: reconcile baseline and lock decisions

1. Review the current uncommitted OpenAI auth diff and run focused config,
   model, session, agent, RPC, and ACP tests.
2. Confirm the TOML names and backward-compatibility policy with the owner.
3. Decide whether sparse `model_overrides` ships in the first version. Do not
   use ambiguous sparse replacement semantics.
4. Record the registered API IDs after all provider clients initialize.
5. Inventory every startup `model` and `args.api_key` capture.

Deliverable: no production behavior; update this plan if repository state has
materially changed.

### Phase 1: config domain types and parsing

1. Separate parsed `Config` from `Args`.
2. Parse provider definitions, auth sources, headers, custom models, and model
   overrides.
3. Add path-aware validation diagnostics.
4. Preserve existing config/CLI behavior.
5. Update `config.toml.example` provisionally.

Suggested focused target: `test-config`.

### Phase 2: immutable effective registry and resolver

1. Add explicit built-in provider definitions.
2. Build configured effective catalogs.
3. Add client-registry API validation.
4. Implement deterministic canonical resolution and ambiguity errors.
5. Route `--list-models`, CLI startup, ACP startup, and child resolution through
   the shared registry.
6. Remove duplicated resolver logic only after coverage is green.

Suggested focused targets: `test-core`, `test-config`, relevant provider tests.

### Phase 3: provider-indexed auth and header merge

1. Add provider-keyed runtime API-key overrides.
2. Integrate provider config sources into `AuthResolver`.
3. Preserve OAuth-only policy for `openai-codex`.
4. Add safe auth availability queries.
5. Centralize effective header merging and update all provider clients.
6. Audit verbose/error paths for secret leakage.

Suggested focused targets: credential/auth tests and all provider tests.

### Phase 4: atomic model switching and persistence

1. Add the idle-only core/session switching API.
2. Add thinking-level normalization.
3. Add session model metadata records and replay.
4. Restore the active model on activate/resume.
5. Update fork/new/truncate/list semantics.
6. Add faux cross-provider switching tests.

Suggested focused targets: `test-agent`, session tests, `test-agent-tasks`.

### Phase 5: interactive CLI selection

1. Add `/model`, `/model <spec>`, `/models`, and completion.
2. Add the TTY selector with RAII terminal ownership.
3. Replace stale startup-model captures.
4. Refresh hooks, status, title, pricing, and effective-context diagnostics.
5. Verify print mode and non-TTY behavior.

Suggested focused targets: CLI/system-prompt/terminal tests plus `make dev`.

### Phase 6: RPC and ACP surfaces

1. Add RPC list/set model commands.
2. Add ACP per-run selection and response metadata.
3. Restore per-session ACP model on every run.
4. Define and enforce concurrent session mutation behavior.
5. Add protocol tests and update protocol documentation/examples.

Suggested focused targets: `test-rpc-mode`, `test-acp`.

### Phase 7: documentation and final verification

1. Document multi-provider TOML, custom models, auth-source safety, startup
   precedence, switching commands, resume behavior, and context-limit warnings.
2. Update CLI help and ACP/RPC examples.
3. Run `make format`.
4. Run `make lint`; review touched-code warnings.
5. Run `make test` or:

   ```sh
   cmake --build build --parallel
   ctest --test-dir build --output-on-failure
   ```

6. Manually smoke-test with faux/local providers first. Use real provider
   credentials only with explicit authorization, and never print them.

## Manual smoke-test checklist

Use isolated config/session/auth paths and local or owner-approved accounts:

1. Start with provider A from `[model]`; verify displayed canonical model.
2. Run a text turn and a tool-calling turn.
3. Open `/model`; cancel and verify no state change.
4. Switch to provider B; verify hooks/status/title update.
5. Run another turn and confirm provider B receives transformed provider A
   history without provider A auth or opaque reasoning state.
6. Switch back to provider A and run a third turn.
7. Inspect session JSONL structurally without printing secrets; verify model
   metadata records and per-message provider/model fields.
8. Exit and `--resume`; verify the last selected model is restored.
9. Fork; switch the child branch; verify parent branch selection is unchanged.
10. Switch from a large-context to a smaller-context model and verify warning or
    enforced post-compaction error behavior.
11. Exercise a same-ID model under two providers and verify ambiguity handling.
12. Exercise an OpenRouter-style slash model ID.
13. Exercise RPC `set_model` and ACP per-run switching with the same durable
    session.
14. Verify verbose output, stream diagnostics, session files, RPC JSON, and ACP
    JSON/SSE contain no API keys, OAuth tokens, or resolved auth headers.

## Completion criteria

The feature is complete only when all of the following are true:

- One TOML file can declare at least two providers and multiple custom models.
- Built-in models remain available unless explicitly overridden.
- Startup selection and all live-selection surfaces use one deterministic
  registry/resolver.
- Ambiguous IDs fail clearly; slash-heavy IDs work.
- A session can use provider A for one completed turn and provider B for the
  next completed turn without reconstructing or losing the transcript.
- Switching during an active turn is rejected atomically.
- Provider A credentials/headers are never sent to provider B.
- Cross-provider reasoning, image, and tool history follows tested conversion
  rules.
- Thinking capabilities are normalized predictably with visible warnings.
- The current selection is journaled, listed, forked, and restored on resume.
- CLI, RPC, and ACP expose coherent session-specific switching behavior.
- Child agents resolve configured models and inherit the parent's current model
  without coupling later changes.
- Existing single-provider configs and CLI invocations remain compatible.
- Config, resolver, auth, provider, agent, session, CLI, RPC, and ACP tests pass.
- Formatting and lint gates pass with no new warnings in touched code.

## Known risks and mitigations

| Risk | Consequence | Mitigation |
|---|---|---|
| Startup `model` references remain after switching | UI, sessions, hooks, or children report/use stale model | Audit every capture and test all commands after a switch |
| Global CLI API key follows model switch | Credential disclosure to another provider | Bind runtime override to the selected provider ID |
| Slash syntax is treated as storage identity | OpenRouter/Fireworks/custom IDs resolve incorrectly | Store `(provider,id)` separately and test canonical/raw ambiguity |
| Sparse custom model erases built-in metadata | Wrong limits, pricing, capabilities, or API | Use explicit `ModelOverride` optionals and deterministic merge |
| Unknown API reaches stub client | Delayed confusing runtime failure | Validate every effective model against registered clients at startup |
| Model switch races prompt start | One turn mixes state/auth/client snapshots | One atomic idle-only lifecycle operation under agent synchronization |
| Session header stays creation-only | Resume silently uses wrong provider | Append/replay current-model metadata and update listing |
| Smaller destination context | Provider rejects request or context is lost | Warn on raw estimate, run compaction hooks, enforce after preparation where reliable |
| Signed/encrypted reasoning crosses providers | Invalid request or sensitive opaque-state disclosure | Exact provider/API/model gate in `transform_messages()` plus regressions |
| Provider/model headers override auth | Wrong tenant/auth or credential leak | Shared case-insensitive precedence with auth last |
| ACP creates a new in-memory agent per run | Session switches disappear between requests | Restore/persist selection in durable journal on every run |
| Concurrent ACP runs mutate one session | Journal/model/run order divergence | Per-session serialization or explicit conflict response |
| Config literal secrets leak through logs/errors | Credential exposure | Prefer env references, redact values, never serialize effective auth |

## Recommended first deliverable boundary

If this must be split across releases, the first coherent deliverable is:

1. Config domain types and effective registry.
2. Deterministic startup resolution in CLI and ACP.
3. Provider-indexed auth/header merging.
4. Atomic `AgentSession::set_model()` plus session journal persistence.
5. Direct `/model <spec>` and RPC `set_model`.
6. Full faux cross-provider and resume tests.

The interactive picker and ACP request-level selection may follow immediately,
but do not describe the project as supporting live switching everywhere until
ACP restores and accepts session-specific model selection.
