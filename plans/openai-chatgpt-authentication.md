# Add OpenAI authentication with ChatGPT subscription access

## Status and intended reader

This is an implementation plan for Luna. It is deliberately more specific
than a design sketch: it defines the feature boundary, data structures,
protocol behavior, file ownership, execution order, test matrix, security
requirements, compatibility gates, and completion criteria.

The feature described here is **ChatGPT sign-in for Codex-model subscription
access**, exposed in pici as a separate `openai-codex` provider. It is not a
replacement for the existing `openai` provider or `OPENAI_API_KEY` support.

No implementation should begin by deleting, renaming, or changing the
semantics of the current Platform API-key path. Existing invocations such as

```sh
OPENAI_API_KEY=... pi-cli --model openai/gpt-4.1
pi-cli --api-key ... --model openai/gpt-4.1
```

must remain behavior-compatible.

## Executive decision

Implement two explicitly different OpenAI authentication surfaces:

| Provider | Authentication | Billing/entitlement | Wire API |
|---|---|---|---|
| `openai` | `OPENAI_API_KEY` or `--api-key` | OpenAI Platform usage billing | `https://api.openai.com/v1/chat/completions` for the existing client |
| `openai-codex` | Browser OAuth or device-code login, with refreshable cached credentials | ChatGPT/Codex workspace or subscription entitlement | `https://chatgpt.com/backend-api/codex/responses` |

Do not send ChatGPT OAuth credentials to `api.openai.com/v1/chat/completions`.
Do not send a Platform API key to the ChatGPT backend and call that
subscription authentication. The provider split is a correctness and security
boundary, not just a display label.

## Authority and compatibility warning

OpenAI's public authentication documentation says that OpenAI's Codex clients
support both ChatGPT sign-in and API-key sign-in. It documents browser login,
device-code login, automatic refresh, credential caching, and keyring/file
storage:

- <https://developers.openai.com/codex/auth>
- <https://developers.openai.com/api/docs/quickstart>

The public API quickstart continues to specify API keys for ordinary
third-party OpenAI API applications. It does not publish a general third-party
ChatGPT OAuth client-registration contract.

The concrete OAuth client ID, endpoints, ChatGPT backend URL, headers, request
shape, and stream behavior in this plan come from the vendored TypeScript pi
implementation, especially:

- `vendor/pi/packages/ai/src/auth/oauth/openai-codex.ts`
- `vendor/pi/packages/ai/src/providers/openai-codex.ts`
- `vendor/pi/packages/ai/src/api/openai-codex-responses.ts`
- `vendor/pi/packages/ai/src/api/openai-responses-shared.ts`

Treat that wire surface as **experimental compatibility behavior**, not a
stable OpenAI Platform API guarantee.

Before merging, the implementer must complete this compatibility gate:

1. Re-read the four vendored files above and record the vendored commit SHA in
   the PR description.
2. Check the current public OpenAI authentication page for any change to
   browser login, device login, credential storage, or account access.
3. Confirm with the repository owner that using the vendored public OAuth
   client identity from an independent C++ client is an accepted product risk.
4. Put the `openai-codex` provider behind an explicit provider selection. Do
   not silently route `openai` models through it.
5. Mark the provider experimental in help and README text.

If step 3 is not accepted, stop after the credential-abstraction improvements
and retain API-key-only OpenAI support. Do not invent an OAuth client ID or
registration flow.

## Repository constraints

Follow `AGENTS.md` throughout:

- Use a narrow `make dev` or specific test target during the edit loop.
- Run `make format` before the final verification.
- Run `make lint`; new warnings in touched code are actionable.
- Before every commit, run `make test`, or the equivalent full CMake build and
  CTest commands.
- Do not add decorative section-divider comments to C++ sources.
- Preserve terminal ownership with RAII and delete copy/move operations for
  types that own sockets, file locks, or terminal state.
- Preserve user-owned worktree changes. At planning time,
  `tool_progress_presentation.html` is an unrelated untracked file.

The project is already POSIX-oriented (`unistd.h`, signals, file descriptors),
so the first implementation may support macOS and Linux only. Make that
constraint explicit in documentation and compile-time errors rather than
pretending Windows credential persistence is safe.

## Goals

1. Let a user run `pi-cli auth login openai-codex`, complete browser or device
   authentication, and persist a refreshable credential.
2. Let the user inspect authentication status without exposing secrets.
3. Let the user log out and remove the stored credential.
4. Refresh an expiring access token safely before a model request.
5. Add an `openai-codex-responses` `LLMClient` that supports pici's existing
   text, image, reasoning-summary, tool-call, tool-result, usage, cancellation,
   session, and streaming-event abstractions.
6. Make `pi-cli` and `pi-acp` use the same credential resolver and provider
   implementation.
7. Keep current API-key providers and local OpenAI-compatible servers working.
8. Ensure credentials never appear in logs, stream diagnostics, session
   records, exception messages, CLI status output, or test snapshots.

## Non-goals

- Replacing the stable `openai` Platform provider.
- Persisting Platform API keys in the new OAuth credential file.
- Importing or directly reading `~/.codex/auth.json` in the first release.
  Sharing another application's mutable refresh credential risks token
  rotation races. A future explicit import command may copy credentials into
  pici's store, but pici must own its stored refresh token thereafter.
- Implementing OpenAI Platform Responses API at the same time. The new client
  is specifically for the ChatGPT Codex backend.
- WebSocket transport. Implement SSE first; leave `Transport::websocket` and
  `Transport::websocket_cached` unsupported for `openai-codex` with a clear
  error.
- Dynamic model-catalog refresh unless the current backend exposes a verified,
  documented-by-the-vendored-client mechanism. Do not infer endpoints.
- OS keychain integration in the first merge. Design the store interface so a
  keychain backend can be added later.
- Browser cookie reuse, scraping ChatGPT, or accepting a raw browser session
  cookie.
- Enterprise Codex access-token creation. A pre-provisioned access-token input
  can be considered separately after OAuth works.
- Server-side tools, tool search, custom grammar tools, service tiers, or
  background Responses requests in the first implementation.

## Current implementation and why it is insufficient

The current path is API-key-only:

1. `src/core/env_api_keys.cpp` reads `OPENAI_API_KEY` into an immutable process
   cache.
2. `src/main.cpp` and `src/acp/main.cpp` create a synchronous
   `get_api_key(provider)` callback.
3. `src/core/agent_loop.cpp` copies the returned string into
   `StreamOptions::api_key` once per turn.
4. `src/http/http_client.cpp` prepends `Authorization: Bearer`.
5. `OpenAICompatibleClient` posts Chat Completions payloads to
   `{base_url}/chat/completions` and parses Chat Completions chunks.

ChatGPT authentication additionally needs an account ID, refresh token,
expiration, refresh coordination, provider-specific headers, a different
endpoint, a Responses request converter, and a Responses event parser.
Changing only `OPENAI_API_KEY` lookup would produce an invalid and unsafe
half-integration.

## Target command-line UX

Reserve `auth` as a top-level subcommand. Define these exact commands:

```text
pi-cli auth login openai-codex
pi-cli auth login openai-codex --browser
pi-cli auth login openai-codex --device
pi-cli auth status
pi-cli auth status openai-codex
pi-cli auth logout openai-codex
```

Rules:

- `--browser` is the default when neither login-mode flag is present.
- `--browser` and `--device` are mutually exclusive.
- `auth login` and `auth logout` require exactly one provider.
- Only `openai-codex` is supported initially. Unknown providers are errors,
  not warnings.
- `auth status` exits zero when no credentials exist; it reports
  `not logged in`.
- Login cancellation via SIGINT exits nonzero, closes the callback server, and
  does not alter an existing credential.
- A failed refresh does not delete the old credential automatically. Status
  reports that re-login is required; logout remains available.
- `--api-key` with `--provider openai-codex` is a usage error. Tell the user to
  use `--provider openai` for Platform API keys or run the auth login command.
- `OPENAI_API_KEY` must not satisfy `openai-codex` authentication.
- `pi-acp` does not expose interactive login. It reads the pici credential
  store and fails at startup with an actionable login command when a selected
  `openai-codex` model has no usable credential.

Example status output, with no secrets or token fragments:

```text
provider        method  status       expires
openai-codex    oauth   authenticated 2026-08-05T22:41:00Z
```

Expired but refreshable credentials may display `refresh required` without
performing network I/O. `auth status` must be a read-only command; model
requests own refresh.

## Target architecture

```text
CLI/API-key/env                         pici auth login
       |                                      |
       v                                      v
RequestAuthResolver <-------------- CredentialStore + OpenAICodexOAuth
       |
       v
AgentLoopConfig::get_auth(provider)
       |
       v
StreamOptions::auth
       |
       +----------------------+-----------------------+
       |                      |                       |
OpenAICompatibleClient   MuseMessagesClient   OpenAICodexResponsesClient
       |                      |                       |
       +----------------------v-----------------------+
                         HttpClient
```

The agent loop resolves credentials once per provider request. Provider
clients decide their wire payloads and merge provider-required headers. The
HTTP layer transports an already-resolved bearer credential and never knows
how OAuth refresh works.

## Core authentication types

Add `src/core/auth_types.h`. Keep this header free of filesystem, curl,
httplib, and OAuth protocol details so `pi-core` does not depend on `pi-http`.

Define:

```cpp
namespace pi::core {

enum class AuthKind {
  none,
  api_key,
  oauth,
};

struct RequestAuth {
  AuthKind kind{AuthKind::none};
  std::optional<std::string> bearer_token;
  std::map<std::string, std::string> headers;
  std::string source;
};

} // namespace pi::core
```

Semantics:

- `source` is safe display metadata such as `OPENAI_API_KEY`, `--api-key`, or
  `pici auth`; never put a path containing user secrets or token material in
  it.
- `headers` contains authentication-derived headers only. For
  `openai-codex`, this includes `chatgpt-account-id`.
- `bearer_token` is optional because local endpoints may require no auth.
- Do not add JSON serialization for `RequestAuth`; it must never become part
  of a session or event payload.
- Do not define equality or stream insertion that could accidentally expose
  secrets.

Replace the internal callback shape:

```cpp
std::function<std::optional<RequestAuth>(std::string_view provider)> get_auth;
```

in `Agent::Options` and `AgentLoopConfig`. Replace
`StreamOptions::api_key` with `std::optional<RequestAuth> auth`.

This is an internal source migration, not a CLI behavior change. Update every
construction site and test fixture in one commit so no provider temporarily
loses authentication.

For ordinary providers, build `RequestAuth` as follows:

```cpp
RequestAuth{
  .kind = AuthKind::api_key,
  .bearer_token = key,
  .headers = {},
  .source = "OPENAI_API_KEY",
}
```

Keep `get_env_api_key()` for now. A later cleanup may return `RequestAuth`
directly, but that is not needed to land this feature safely.

## Header-merging contract

Fix header handling before adding OAuth. HTTP header names are
case-insensitive; the current `std::map` comparison is not.

Create a small case-insensitive merge helper in `src/http/http_client.cpp` or
`src/http/http_headers.{h,cpp}`. Its precedence must be:

1. Transport defaults (`Content-Type`, `Accept`).
2. `Model::headers`.
3. `StreamOptions::headers` supplied by application hooks/configuration.
4. Provider-required headers added by the concrete client.
5. `RequestAuth::headers` and the generated `Authorization` header.

Auth wins last. A caller must not be able to override `Authorization` or
`chatgpt-account-id` through differently cased header keys.

Emit each logical header once. Never append two `Authorization` headers.
Tests must cover `authorization`, `Authorization`, `CONTENT-TYPE`, and
`content-type` collisions.

Change `HttpClient::post` and `post_streaming` to accept
`std::optional<RequestAuth>` instead of `std::optional<std::string> api_key`.
Both functions should apply the bearer token and auth headers. Preserve all
existing timeouts, cancellation, diagnostic hooks, and default Accept
behavior.

Do not log headers in verbose mode. Current provider verbose output prints URL
and JSON payload; preserve that boundary.

## Credential representation and storage

Add:

- `src/auth/credential_store.h`
- `src/auth/credential_store.cpp`
- `test/test_credential_store.cpp`

Use namespace `pi::auth`.

Define a narrow initial credential type:

```cpp
struct OAuthCredential {
  std::string access_token;
  std::string refresh_token;
  std::int64_t expires_at_ms{0};
  std::string account_id;
};
```

Do not overload `api_key` terminology for OAuth tokens.

Use this versioned JSON file:

```json
{
  "version": 1,
  "providers": {
    "openai-codex": {
      "type": "oauth",
      "access_token": "...",
      "refresh_token": "...",
      "expires_at_ms": 1785988860000,
      "account_id": "..."
    }
  }
}
```

Path policy:

- If `PICI_AUTH_FILE` is nonempty, use it. This override is primarily for
  tests, containers, and isolated automation.
- Otherwise use `$XDG_CONFIG_HOME/pici/auth.json` when XDG config is set.
- Otherwise use `$HOME/.config/pici/auth.json`.
- If neither HOME nor XDG config is usable, return an actionable error. Never
  silently write credentials into the current working directory.

The config directory helper should be shared with `src/cli/config.cpp` rather
than independently reimplementing slightly different HOME/XDG behavior.
Extract a small `src/core/paths.{h,cpp}` utility if necessary.

Storage requirements:

1. Create the parent directory with owner-only permissions (`0700`). If it
   exists, reject a non-directory. Warn or fail if it is group/world writable;
   prefer failing for the auth command.
2. Create the auth file with `0600` from its first write. Do not create with a
   permissive umask and chmod it later.
3. Write a temporary file in the same directory, `fsync` it, atomically rename
   it over the destination, and `fsync` the directory where supported.
4. Reject symlink destinations using `lstat`/`O_NOFOLLOW` where available.
5. Reject malformed JSON, unknown top-level versions, missing required string
   fields, non-integer expiration, and a non-object `providers` value.
6. Preserve unknown provider entries when modifying one known provider, but
   do not preserve malformed known entries.
7. Never return a partially parsed credential.
8. `logout` removes only the requested provider entry. Remove an empty auth
   file only if doing so is atomic and does not affect unknown entries.

Concurrency requirements:

- Use a sibling lock file such as `auth.json.lock`, opened `0600`, with an
  exclusive POSIX `flock` for every read-modify-write operation.
- Re-read the auth file after acquiring the lock.
- Hold the lock across OAuth refresh and the subsequent atomic write. This
  prevents two pici processes from simultaneously rotating the same refresh
  token.
- Plain status reads may use a shared lock.
- Make the lock an RAII type with copy and move deleted.
- Honor cancellation while waiting to acquire the lock by polling nonblocking
  `flock` with a short bounded wait; do not spin.

The store API should expose operations, not raw mutable JSON:

```cpp
class CredentialStore {
public:
  std::optional<OAuthCredential> read_oauth(std::string_view provider) const;
  std::vector<CredentialInfo> list() const;
  OAuthCredential modify_oauth(
      std::string_view provider,
      const std::function<OAuthCredential(
          const std::optional<OAuthCredential>&)> &fn,
      std::stop_token stop_tok = {});
  void erase(std::string_view provider);
};
```

`CredentialInfo` must contain only provider, kind, and expiration/status
metadata.

## OAuth protocol implementation

Add:

- `src/auth/openai_codex_oauth.h`
- `src/auth/openai_codex_oauth.cpp`
- `test/test_openai_codex_oauth.cpp`

Keep protocol constants private to the `.cpp`. At implementation time, copy
them from the checked vendored file and add a short source comment containing
the vendored commit SHA. The current planning baseline is:

```text
authorization endpoint: https://auth.openai.com/oauth/authorize
token endpoint:         https://auth.openai.com/oauth/token
browser redirect:       http://localhost:1455/auth/callback
device user-code:       https://auth.openai.com/api/accounts/deviceauth/usercode
device token polling:   https://auth.openai.com/api/accounts/deviceauth/token
device verification:    https://auth.openai.com/codex/device
device redirect:        https://auth.openai.com/deviceauth/callback
scope:                  openid profile email offline_access
```

Do not copy these values from this plan without comparing them to the vendored
source during implementation.

### PKCE and state

Implement reusable, testable helpers:

```cpp
std::string base64url_encode(std::span<const std::byte> bytes);
std::vector<std::byte> secure_random_bytes(std::size_t count);
PkcePair generate_pkce();
std::string generate_oauth_state();
```

Requirements:

- Read randomness from the OS CSPRNG. On supported POSIX platforms, an exact
  read from `/dev/urandom` is acceptable; retry `EINTR` and fail closed on a
  short read. Do not use `std::rand`, timestamps, UUID formatting, or
  `std::random_device` without proving its backing implementation.
- Generate a verifier meeting RFC 7636 length and character requirements.
- Compute `BASE64URL(SHA256(verifier))`, without padding, for the S256
  challenge.
- Pin and use one reviewed SHA-256 implementation. Prefer an already available
  system crypto target if CMake can require it consistently on macOS and Linux;
  otherwise add a small pinned header-only dependency. Do not hand-roll a new
  SHA-256 implementation in this feature.
- Generate at least 128 bits of independent state entropy.
- Compare returned state exactly before exchanging a code.
- Unit-test against RFC 7636's published S256 vector.

### Authorization URL

Generate all query parameters with a percent-encoding helper; do not concatenate
unescaped values. Match the vendored parameters, currently including:

```text
response_type=code
client_id=<verified vendored public client id>
redirect_uri=http://localhost:1455/auth/callback
scope=openid profile email offline_access
code_challenge=<generated>
code_challenge_method=S256
state=<generated>
id_token_add_organizations=true
codex_cli_simplified_flow=true
originator=pi
```

Use `originator=pici` only if live compatibility testing confirms it is
accepted; otherwise preserve the vendored compatibility value and document
why.

### Browser callback flow

Use `cpp-httplib` for a loopback-only callback server:

- Bind `127.0.0.1`, never `0.0.0.0`.
- Use fixed port `1455` because it is part of the registered redirect URI.
- Accept only `GET /auth/callback`.
- Reject missing code, missing state, or mismatched state with HTTP 400.
- Return HTTP 404 for every other route.
- Return a minimal static success/error HTML page. Do not reflect query
  parameters into HTML.
- Stop the server immediately after the first valid callback.
- Give the whole browser flow a 15-minute deadline.
- Make the server and waiting thread RAII-owned and stop-token-aware.
- If binding fails, return an error suggesting `--device`; do not choose a
  random port because the redirect URI would no longer match.
- Print the authorization URL before waiting. Automatic browser launch is
  optional and must not be required for acceptance.

The callback request must never be logged in full because it contains the
authorization code.

### Device-code flow

Match the vendored sequence:

1. POST JSON `{ "client_id": "..." }` to the user-code endpoint.
2. Validate nonempty `device_auth_id`, nonempty `user_code`, and a finite,
   nonnegative polling interval.
3. Print only the verification URI and user code.
4. Poll the token endpoint with `device_auth_id` and `user_code`.
5. Treat HTTP 403/404 and `deviceauth_authorization_pending` as pending.
6. Increase the interval on `slow_down`.
7. Abort after 15 minutes, on SIGINT, or when the supplied stop token fires.
8. On success, validate `authorization_code` and `code_verifier` and exchange
   them at the OAuth token endpoint using the device redirect URI.

Use a monotonic clock for deadlines and waits. Each poll wait must be
interruptible; do not use one long blocking sleep.

### Code exchange and refresh

POST `application/x-www-form-urlencoded` bodies.

Authorization-code exchange fields:

```text
grant_type=authorization_code
client_id=<verified client id>
code=<authorization code>
code_verifier=<PKCE verifier>
redirect_uri=<browser or device redirect URI>
```

Refresh fields:

```text
grant_type=refresh_token
refresh_token=<stored refresh token>
client_id=<verified client id>
```

For both responses:

- Require a nonempty `access_token`, nonempty `refresh_token`, and numeric
  positive `expires_in`.
- Compute expiration using checked integer arithmetic.
- Never put a response body containing tokens into an exception.
- For non-2xx responses, extract a safe OAuth error code/message if present,
  redact all token-shaped fields, and cap displayed error text.
- A refresh response replaces both access and refresh tokens. Never retain the
  old refresh token merely because the new one differs.

### JWT account ID extraction

The access token is used as a bearer token and also carries the ChatGPT account
ID required by the backend.

- Split into exactly three JWT segments.
- Base64url-decode the payload with strict validation.
- Parse the payload as a JSON object.
- Extract the nonempty string claim currently located at
  `https://api.openai.com/auth.chatgpt_account_id`, matching the vendored
  implementation's nested object.
- Reject credentials without the claim.
- Do not treat local JWT parsing as signature verification. The token was
  obtained over TLS directly from the token endpoint, and the backend remains
  responsible for authorization.
- Never display any other JWT claims.

## Credential resolution and refresh

Add `src/auth/auth_resolver.{h,cpp}` with a long-lived resolver captured by
both CLI and ACP agent options.

The resolver's provider policy is exact:

1. If `--api-key` is present and provider is not `openai-codex`, return it as
   API-key auth with source `--api-key`.
2. If provider is `openai-codex`, ignore ordinary provider env keys and resolve
   the stored OAuth credential.
3. Otherwise use `get_env_api_key(provider)` and identify the environment
   variable source without exposing the value.
4. If no auth exists, return `std::nullopt` for providers that permit no auth
   (local endpoints), or let the concrete remote client produce an actionable
   missing-auth error.

For `openai-codex`:

- Refresh when `expires_at_ms <= now + 5 minutes`.
- Enter `CredentialStore::modify_oauth`, re-read under the exclusive lock, and
  check expiration again. Another process may already have refreshed it.
- If still expiring, call the refresh endpoint while holding the cross-process
  lock, validate the replacement, and atomically persist it.
- Return `RequestAuth` with OAuth kind, access bearer, account-ID header, and
  safe source `pici auth`.
- If refresh fails, preserve the stored credential unchanged and throw a typed
  `AuthError` instructing the user to run login again.
- If cancellation occurs, preserve the stored credential and surface an
  aborted turn, not an authentication failure.

Clock access and the refresh HTTP operation must be injectable in unit tests.
Tests must not wait for wall-clock expiration or contact OpenAI.

## Agent-loop integration

Update:

- `src/core/agent.h`
- `src/core/agent.cpp`
- `src/core/agent_loop.h`
- `src/core/agent_loop.cpp`
- `src/core/llm_client.h`
- `src/main.cpp`
- `src/acp/main.cpp`

Resolve auth after context preparation but before the provider's network call.
Ensure resolution is inside the same exception-to-terminal-event boundary as
the client invocation. A refresh exception must not escape the background
worker and terminate the process.

Map errors as follows:

| Condition | Stop reason | User-facing message |
|---|---|---|
| No stored `openai-codex` credential | error | `Not logged in to openai-codex; run pi-cli auth login openai-codex` |
| Refresh rejected | error | `OpenAI login expired or was revoked; run ... auth login ...` plus safe provider error |
| Credential file malformed/insecure | error | Name the file and remediation; never print contents |
| Waiting/request cancelled | aborted | Existing cancellation wording |
| Provider returns unauthorized | error | Suggest status/login without dumping response headers or token |

Do not persist `RequestAuth` in `AgentState`, `AgentContext`, session headers,
events, Lua hook context, or diagnostics.

## `openai-codex-responses` client

Add:

- `src/core/providers/openai_codex_responses.h`
- `src/core/providers/openai_codex_responses.cpp`
- `test/test_openai_codex_responses.cpp`

Class contract:

```cpp
class OpenAICodexResponsesClient final : public LLMClient {
public:
  std::shared_ptr<AssistantMessage>
  stream(const Model &, const AgentContext &, const StreamOptions &,
         AssistantEventCallback, std::stop_token) override;

  std::string_view provider_name() const override {
    return "openai-codex";
  }
  std::string_view api_id() const override {
    return "openai-codex-responses";
  }

  static nlohmann::json build_request_json(...);
};
```

Register `openai-codex-responses` explicitly in both executable startup paths,
following the Muse registration pattern. Do not add another static
self-registration initializer.

### URL and transport

- Default base URL: `https://chatgpt.com/backend-api`.
- Normalize trailing slashes.
- If a supplied base URL already ends in `/codex/responses`, use it.
- If it ends in `/codex`, append `/responses`.
- Otherwise append `/codex/responses`.
- Support SSE only in the first version.
- `Transport::auto_transport` and `Transport::sse` select SSE.
- Explicit WebSocket transports return a clear unsupported-transport error.
- Preserve the existing 10-second connect timeout and configurable request
  timeout.

Required headers, after verifying against the vendored implementation:

```text
Authorization: Bearer <OAuth access token>
chatgpt-account-id: <credential account ID>
originator: pi
User-Agent: pici/<version> (<platform>; <architecture>)
OpenAI-Beta: responses=experimental
Accept: text/event-stream
Content-Type: application/json
session-id: <session ID, when present>
x-client-request-id: <session ID, when present>
```

Do not log any of these headers. The `originator` value is a compatibility
field and must be verified rather than aesthetically renamed.

### Request body

Build this baseline shape:

```json
{
  "model": "<model id>",
  "store": false,
  "stream": true,
  "instructions": "<system prompt or fallback>",
  "input": [],
  "text": { "verbosity": "low" },
  "include": ["reasoning.encrypted_content"],
  "prompt_cache_key": "<session id when present>",
  "tool_choice": "auto",
  "parallel_tool_calls": true
}
```

Rules:

- Omit `prompt_cache_key` when no session ID exists.
- Omit `tools` when there are no tools.
- Add `temperature` only when supplied.
- Add `reasoning: {"effort": ..., "summary": "auto"}` only when the selected
  model/level mapping yields a value.
- Do not add `max_output_tokens` merely because `StreamOptions::max_tokens`
  exists until live/backend compatibility is verified. If supported, add it
  deliberately with a request-shape test.
- Do not add service tier, background, previous response ID, store=true,
  server tools, or WebSocket fields in this phase.
- Run `options.on_payload` after constructing the provider payload, consistent
  with the existing clients. Validate that the hook still returns an object.
- Invoke `options.on_response` when HTTP status/headers are available. If the
  current streaming transport cannot expose them, extend it with a response
  callback rather than silently skipping the established hook.

### Input conversion

Run existing `transform_messages()` first. Preserve thinking only for an
appropriate same-model/provider/API history, consistent with its current
rules.

Convert pici messages to Responses input items:

- `UserMessage` text -> a `message` item with role `user` and `input_text`
  content.
- User image -> `input_image` with
  `data:<mime_type>;base64,<data>` and `detail: auto`.
- Assistant `TextContent` -> a completed assistant `message` output item with
  `output_text`, empty annotations, and a stable item ID.
- Assistant `ThinkingContent` with a valid `thinking_signature` -> parse the
  signature as a complete saved Responses reasoning item and replay it.
- Assistant thinking without a valid signature -> do not send it as a
  reasoning item. Let `transform_messages()` downgrade safe readable thinking
  to text where appropriate.
- Assistant `ToolCall` -> a `function_call` item with `call_id`, optional item
  `id`, name, and JSON-string arguments.
- `ToolResultMessage` -> `function_call_output` with matching `call_id` and
  string or mixed text/image output.
- Empty tool output -> `(no tool output)`.
- Images sent to a model without image capability -> replace with a textual
  marker, consistent with existing cross-model conversion behavior.

Responses tool-call identifiers require special care. Persist pici tool-call
IDs as `<call_id>|<item_id>` when both are present. When replaying:

- Split on the first `|`.
- Normalize IDs to `[A-Za-z0-9_-]` and the backend length limit used by the
  vendored client.
- Ensure Responses function item IDs begin with `fc_` when retained.
- Drop a foreign or invalid item ID rather than pairing it with unrelated
  encrypted reasoning.
- Preserve `call_id`, because tool results match on it.

Use `TextContent::text_signature` to persist the Responses message item ID and
optional phase. Use a versioned JSON encoding such as:

```json
{"v":1,"id":"msg_...","phase":"final_answer"}
```

Do not overload visible text or response IDs for this metadata.

### Tool conversion

Convert each pici `ToolDefinition` to:

```json
{
  "type": "function",
  "name": "...",
  "description": "...",
  "parameters": { "type": "object", "properties": {} },
  "strict": false
}
```

Parse `ToolSchema::serialize()` as JSON and require an object. If schema
serialization is invalid, fail before the network request with the tool name
and a safe error. Preserve current strict-mode behavior only after checking
backend compatibility; do not force `strict: true` globally.

### SSE parser seam

Implement the parser as a testable class with no curl dependency:

```cpp
class OpenAICodexResponsesParser {
public:
  void feed_line(std::string_view line);
  void finish();
};
```

It owns:

- The in-progress `AssistantMessage`.
- A map from `output_index` to a slot containing pici content index and block
  type.
- Partial function-call JSON buffers.
- Reasoning blocks keyed by Responses reasoning item ID.
- Whether a terminal response event was observed.

Accept `data:` SSE lines, ignore comments/keepalives, trim an optional CR, and
handle `[DONE]` only as a transport terminator. `[DONE]` does not replace the
required terminal Responses event.

Handle at least these events:

| Event | Action |
|---|---|
| `response.created` | Save response ID |
| `response.output_item.added` with reasoning | Create `ThinkingContent`; emit thinking start |
| `response.output_item.added` with message | Create `TextContent`; emit text start |
| `response.output_item.added` with function call | Create `ToolCall`; emit tool-call start |
| `response.reasoning_summary_text.delta` | Append visible thinking summary; emit delta |
| `response.reasoning_summary_part.done` | Preserve paragraph separation when needed |
| `response.reasoning_text.delta` | Append reasoning text if delivered |
| `response.output_text.delta` | Append visible answer; emit text delta |
| `response.refusal.delta` | Append visible refusal text; terminal status determines error |
| `response.function_call_arguments.delta` | Append partial JSON and emit tool-call delta |
| `response.function_call_arguments.done` | Replace/finalize arguments deterministically |
| `response.output_item.done` reasoning | Save the complete item JSON in `thinking_signature`; emit thinking end |
| `response.output_item.done` message | Replace with authoritative completed text, save text signature, emit text end |
| `response.output_item.done` function call | Parse final arguments, clear scratch buffer, emit tool-call end |
| `response.completed` | Save usage/status/ID, compute cost, emit done |
| `response.incomplete` | Map reason, save usage, emit done or error as appropriate |
| `response.failed` | Extract safe error and emit error |
| top-level `error` | Extract safe code/message and emit error |

Unknown event types must be ignored without corrupting known slot state. In
verbose diagnostics, record only the event type and byte count, never raw
payload text.

At `response.output_item.done`, authoritative completed content may be longer
than received deltas. Reconcile without emitting duplicate visible text:

- If completed text starts with accumulated text, emit only the suffix delta.
- If it differs, update the stored final block but do not replay the entire
  content to the renderer; record a privacy-safe parser diagnostic.
- Apply the equivalent rule to function-call argument completion.

On `finish()`:

- Error if no terminal `response.completed`, `response.incomplete`, or
  `response.failed` event was seen.
- Finalize no open block as successful merely because the socket closed.
- Clear partial scratch buffers only after producing the terminal error event.

### Reasoning replay

For `store:false` multi-turn behavior, encrypted reasoning must round-trip:

- Save the entire completed reasoning output item JSON in
  `ThinkingContent::thinking_signature`.
- Ensure `reasoning.encrypted_content` is requested.
- If encrypted content appears only in the terminal response's `output` array,
  backfill it into the corresponding saved reasoning item before completing.
- Replay signed/encrypted reasoning only to the same provider/API/model
  combination allowed by `transform_messages()`.
- Never render `encrypted_content`, include it in logs, or expose it to Lua as
  visible thinking text. Existing session serialization may persist the
  signature because it is needed for replay; document that session files can
  contain provider-generated opaque state and must be protected accordingly.

### Usage and stop reasons

Parse terminal usage:

- `input_tokens`
- `output_tokens`
- `input_tokens_details.cached_tokens`
- optional cache-write token details if present
- `output_tokens_details.reasoning_tokens`
- `total_tokens`

Match current pici accounting semantics: if the provider's input total already
includes cached/cache-write tokens, subtract those components from uncached
input without going below zero. Call `compute_cost` using the model entry.

Map terminal status:

| Status | Incomplete reason | pici stop reason |
|---|---|---|
| `completed` | none | `stop`, or `tool_use` if tool calls exist |
| `incomplete` | `max_output_tokens` | `length` |
| `incomplete` | anything else/missing | `error` with safe reason |
| `failed` | any | `error` |

If a message item phase is `final_answer`, it may establish a normal stop, but
a terminal failed/incomplete response still wins.

## HTTP transport changes needed by both OAuth and Responses

Extend `HttpClient` carefully rather than embedding curl calls in OAuth:

1. Keep `post()` able to return non-2xx status and response body.
2. Add a response-header callback or fill `HttpClient::Response::headers`;
   the current struct declares headers but the implementation does not collect
   them.
3. For streaming responses, retain a bounded non-2xx response body even when
   it is newline terminated. The current line callback can consume the buffer
   before the synthetic status error is built.
4. Cap retained error bodies, for example at 64 KiB.
5. Add an option to suppress raw streaming delivery until a successful HTTP
   status is known, or otherwise guarantee that an HTML/JSON auth error is not
   handed to the SSE parser as model output.
6. Preserve cancellation via `CURLOPT_XFERINFOFUNCTION`.
7. Preserve privacy-safe `StreamDiagnostics`; do not record bodies or headers.
8. Add a form-encoding helper for OAuth token requests and a JSON POST helper
   for device endpoints. Both can use the same generic `post` transport.

Add HTTP tests with a local httplib server for status, headers, body retention,
case-insensitive header precedence, cancellation, and streaming error bodies.
No test should contact the public internet.

## Model registry and model resolution

Add at least one verified `openai-codex` model entry so generic model
resolution can infer the provider base URL and API. Do not guess a current
model name, context window, output limit, pricing, or reasoning map from this
plan.

Implementation procedure:

1. Recover or update the vendored `openai-codex` model data referenced by
   `vendor/pi/packages/ai/src/providers/openai-codex.models.ts`; the backing
   JSON is absent from the current checkout.
2. Record the exact upstream/vendored source and date in the PR.
3. Copy only entries actually supported by the ChatGPT Codex provider.
4. Set `.api = "openai-codex-responses"`,
   `.provider = "openai-codex"`, and
   `.base_url = "https://chatgpt.com/backend-api"`.
5. Preserve per-model reasoning-level mappings and image capability.
6. Use verified context/output limits and prices. If subscription pricing does
   not map to token pricing, do not display a fabricated dollar cost. Add an
   explicit cost-availability representation to `Model` rather than treating
   all-zero prices as confirmed free usage.
7. Add model-resolution tests for `openai-codex/<id>` and for
   `--provider openai-codex --model <id>`.

Consider adding a provider-template registry separate from model entries so an
unknown explicitly supplied model can still infer API/base URL. Do this only
if it improves existing provider resolution generally; do not add a fake
`default` model to the user-visible model list.

Do not change pici's default model to `openai-codex` in this feature.

## CLI parsing and command dispatch

Update `src/cli/args.h` with explicit command state rather than inferring auth
commands later from positional prompt messages:

```cpp
enum class Command {
  run,
  auth_login,
  auth_status,
  auth_logout,
};

struct AuthArgs {
  std::string provider;
  enum class LoginMode { browser, device } login_mode{LoginMode::browser};
};
```

Parsing rules:

- Recognize `auth` only when it is the first non-option token.
- Once in auth-command grammar, reject ordinary run-only flags such as
  `--model`, `--print`, prompt text, session resume, and tool flags.
- Allow global `--config`, `--verbose`, `--help`, and the auth login-mode
  flags where relevant.
- Do not merge auth command/provider/mode from TOML. Authentication actions
  are CLI-only.
- Dispatch auth commands in `main()` before model resolution, tool loading,
  renderer setup, session creation, or OTel agent initialization.
- `pi-cli auth --help` prints auth-specific help.
- `pi-cli auth login` without a provider is an error; do not prompt from an
  evolving provider menu in the first release.

Update `test/test_config.cpp` or split argument tests into a new
`test/test_args.cpp`. Cover every valid command and invalid combination.

## ACP behavior

`pi-acp` must not parse or execute `auth` subcommands.

When configured with an `openai-codex` model:

- Construct the same `CredentialStore` and `AuthResolver` as `pi-cli`.
- Resolve/refresh once during startup to fail fast before listening, then use
  the resolver per request so long-running servers refresh again as needed.
- A startup auth failure prints a safe message and exits nonzero.
- Concurrent ACP requests share the in-process resolver and store lock.
- No access/refresh token may enter ACP JSON responses or logs.

Add an ACP test with an injected fake resolver for success, missing auth,
refresh failure, and concurrent request resolution. Do not put real-looking
JWTs in golden fixtures; use obvious test tokens.

## Error handling and redaction

Create typed exceptions or result errors for:

- credential storage
- malformed credential
- OAuth protocol
- login cancellation/timeout
- refresh required/rejected
- missing authentication
- provider protocol

Each error should carry a safe user-facing message and, optionally, a machine
category. It must not carry raw request/response bodies after token fields have
been parsed.

Implement a narrow redaction helper for OAuth error JSON that replaces values
for at least:

```text
access_token
refresh_token
id_token
authorization_code
code
code_verifier
client_secret
```

Do not attempt a generic regex that prints the rest of a JWT. Prefer structured
JSON extraction and fixed safe messages.

Verbose mode may report:

- Auth method and safe source.
- OAuth operation name (`authorization exchange` or `refresh`).
- HTTP status.
- Responses event type.
- Retry/poll count and delay.

It must not report:

- Authorization URLs after the interactive login command has finished, because
  they contain state/challenge values.
- Callback URLs or query strings.
- Request/response headers.
- Any token, code, verifier, state, account ID, encrypted reasoning content, or
  raw OAuth response body.

## Test plan

Use the repository's existing hand-written test executables unless there is a
separate approved test-framework migration.

### Authentication type and header tests

- Existing env API keys still become bearer authentication.
- `--api-key` still wins over env for ordinary providers.
- `OPENAI_API_KEY` is not used for `openai-codex`.
- Case-insensitive header merging emits one Authorization header.
- Auth headers override model/options headers.
- Missing local auth remains allowed.
- No auth type is JSON serializable or printed.

### Credential-store tests

- HOME, XDG, and `PICI_AUTH_FILE` path precedence.
- No HOME/XDG fails closed.
- First write creates `0700` parent and `0600` file.
- Existing insecure file/directory handling.
- Round-trip valid version-1 data.
- Missing file returns no credential.
- Malformed JSON and every missing/wrongly typed field.
- Unknown version rejection.
- Unknown provider preservation.
- Logout one provider without deleting another.
- Symlink destination rejection.
- Atomic replacement leaves either old or new valid JSON under an injected
  failure.
- Two threads and two forked processes serialize modification.
- Cancelled lock wait leaves data unchanged.

### PKCE/JWT tests

- RFC 7636 S256 known vector.
- Base64url padding and invalid-character cases.
- Secure random helper exact-read behavior via injectable byte source.
- Authorization URL includes each expected decoded parameter once.
- State exact match, missing state, and mismatch.
- JWT wrong segment count, invalid base64url, invalid JSON, missing nested auth
  object, missing/empty account ID, and valid extraction.
- Ensure parse errors do not include the token.

### Browser callback tests

- Correct route/code/state succeeds.
- Wrong state, missing code, and unrelated route fail.
- Second callback cannot replace the first.
- Bind failure suggests device login.
- Stop token and deadline stop the server promptly.
- Returned HTML does not reflect query values.

Use a configurable callback bind address/port only in the test constructor;
production remains fixed to the registered redirect.

### OAuth HTTP tests

Use a local fake server and injectable endpoint set:

- Exact exchange form fields and encoding.
- Exact refresh form fields.
- Valid token response and rotated refresh token.
- Missing token fields and invalid expiration.
- Non-2xx JSON OAuth error is safely summarized.
- Non-JSON/HTML failure is capped and does not leak bodies.
- Cancellation.
- Device pending, slow-down, completion, timeout, and malformed responses.
- Refresh threshold: outside window, inside window, already expired.
- Double-checked refresh under concurrent callers invokes network refresh once.
- Failed refresh preserves old file byte-for-byte.

### Request-conversion tests

- Required top-level body and omitted optional fields.
- System prompt goes to `instructions`, not an input system message.
- User text and image.
- Assistant text with item signature.
- Same-model encrypted reasoning replay.
- Cross-model reasoning handling through `transform_messages()`.
- Function call and function output IDs.
- Text-only, image-only, mixed, error, and empty tool results.
- Tool schema conversion and invalid schema error.
- Temperature and reasoning effort mapping.
- Session ID in cache key and headers.
- `on_payload` composition and invalid hook result.
- No unsupported service/WebSocket/server-tool fields.

### SSE parser tests

- Complete text response event sequence.
- Multiple output items and noncontiguous output indices.
- Reasoning summary deltas and encrypted-content backfill.
- Function-call argument fragments that are temporarily invalid JSON.
- Function-call authoritative done arguments longer than accumulated deltas.
- Tool-call stop reason.
- Refusal delta followed by completed/incomplete/failed terminal status.
- Usage details and cost calculation.
- `max_output_tokens` maps to length.
- Other incomplete reasons map to error.
- Top-level error and response.failed.
- Unknown events, keepalives, CRLF, blank lines, malformed JSON, and `[DONE]`.
- Socket close without terminal event is an error.
- Cancellation after partial content emits one terminal aborted/error path.
- Emitted content indices correspond exactly to stored blocks.
- Stream diagnostics contain event names/counts but no payload text.

### CLI and integration tests

- Every auth command and help form.
- Missing/unknown provider and conflicting modes.
- Run-only flags rejected under auth commands.
- Existing positional prompt `authenticating` is still a prompt; only exact
  first token `auth` is reserved.
- Login success atomically replaces credentials.
- Login failure/cancellation preserves existing credentials.
- Status never refreshes or performs network access.
- Logout is idempotent.
- A fake authenticated `openai-codex` end-to-end turn with text and tool use.
- Existing OpenAI Completions and Muse tests remain unchanged and passing.
- ACP fail-fast and successful injected-auth startup.

No normal test may call `auth.openai.com`, `chatgpt.com`, or `api.openai.com`.
An optional manual smoke-test procedure may be documented separately and must
never be part of `ctest`.

## CMake and target layout

Update `CMakeLists.txt` deliberately:

- Add `src/core/auth_types.h` to the include surface; headers need not be listed
  as sources unless the project chooses to do so consistently.
- Add path and non-network credential-store code to `pi-core` only if it has no
  dependency on curl/httplib.
- Add OAuth network code and `openai_codex_responses.cpp` to `pi-http`.
- Link `pi-http` to `httplib::httplib` if the callback server lives in that
  target, or create a small `pi-auth` target depending on `pi-core`, curl, and
  httplib. Avoid a `pi-core` -> `pi-http` cycle.
- Add the selected SHA-256 dependency as a pinned, excluded-from-all helper
  target if system crypto is not used.
- Add each test executable and `add_test` entry.
- Keep dependency helper binaries `EXCLUDE_FROM_ALL`.

Preferred dependency direction:

```text
pi-core
  ^       ^
  |       |
pi-http  pi-auth (optional separate target)
  ^       ^
  +---+---+
      |
  pi-cli / pi-acp
```

If OAuth uses `HttpClient`, placing it in `pi-http` is simpler and avoids a
second curl wrapper. The callback-server dependency then becomes part of
`pi-http`; that is acceptable because both executables already use the target.

## Documentation changes

Update:

- `README.md`
- `config.toml.example`
- CLI help text
- ACP startup/help text where relevant

Document:

- The difference between `openai` API-key billing and `openai-codex`
  subscription/workspace access.
- Exact login/status/logout commands.
- Browser and headless/device flows.
- Credential path and `PICI_AUTH_FILE` override.
- File-storage security and logout behavior.
- Experimental compatibility status.
- API keys remain recommended for CI and ordinary programmatic API use.
- `pi-acp` requires login to have been completed separately.
- Session files may contain opaque encrypted reasoning replay state, though not
  access or refresh tokens.
- Troubleshooting for revoked login, expired refresh, callback port in use,
  device login disabled, workspace entitlement, and insecure credential file.

Do not put the public OAuth client ID into general README examples. Keep it an
implementation constant with provenance.

## Phased execution and commit boundaries

Use these phases in order. Each phase should be independently reviewable and
must pass `make test` before its commit.

### Phase 0: compatibility spike, no production behavior

1. Complete the authority gate above.
2. Restore/locate the missing vendored model catalog data.
3. Capture sanitized fixture streams and request shapes from the vendored test
   suite or an owner-approved manual account smoke test.
4. Decide the SHA-256 dependency and verify macOS/Linux builds.
5. Record exact supported model entries and OAuth constants.

Deliverable: notes in the PR or a small checked-in fixture provenance README;
no tokens or live account IDs.

### Phase 1: auth-neutral request plumbing

1. Add `RequestAuth` and migrate callbacks/options.
2. Implement case-insensitive header precedence.
3. Expand HTTP response header and bounded error-body handling.
4. Keep all existing providers green.

Suggested commit: `refactor: generalize provider request authentication`.

### Phase 2: secure credential store

1. Add shared config/auth paths.
2. Add versioned JSON parsing.
3. Add permission, symlink, atomic-write, and lock handling.
4. Add exhaustive store tests.

Suggested commit: `feat: add secure provider credential storage`.

### Phase 3: OAuth protocol and auth commands

1. Add PKCE, state, form encoding, JWT claim extraction.
2. Add browser callback and device flow.
3. Add token exchange/refresh.
4. Add CLI subcommands, status, and logout.
5. Add fake-server tests.

Suggested commit: `feat: add openai codex login and token refresh`.

### Phase 4: Responses request conversion and parser

1. Add model/tool/message conversion.
2. Add parser seam and all fixture tests.
3. Add usage, stop reasons, reasoning replay, and error mapping.
4. Do not wire live model selection until parser fixtures pass.

Suggested commit: `feat: add openai codex responses client`.

### Phase 5: registry, CLI, and ACP integration

1. Add verified model/provider entries.
2. Register the client in both executables.
3. Wire the long-lived resolver in CLI and ACP.
4. Add startup and end-to-end fake-server tests.

Suggested commit: `feat: wire openai codex provider into cli and acp`.

### Phase 6: documentation and manual smoke test

1. Update help, README, and example config.
2. Run a browser login, one text turn, one tool turn, refresh simulation, status,
   and logout with an owner-approved account.
3. Verify logs, session files, diagnostics, and terminal output contain no
   credentials/account ID.
4. Run final quality gates.

Suggested commit: `docs: document openai codex authentication`.

Avoid collapsing these into one giant commit. The credential plumbing and
store are security-sensitive and deserve separate review.

## Manual smoke-test checklist

Run only after unit/integration tests pass and only with explicit permission to
use the account:

1. Set `PICI_AUTH_FILE` to an isolated temporary path outside the repository.
2. Run browser login and verify callback success.
3. Check directory/file modes without printing file contents.
4. Run `auth status` and confirm it shows no account ID or token fragment.
5. Run one print-mode text request.
6. Run a tool-calling request and verify the second turn replays tool output.
7. Run a reasoning model twice in one session and confirm encrypted reasoning
   replay does not fail.
8. Force `expires_at_ms` near expiry using a dedicated test helper, not manual
   editing of production credentials, and verify one refresh occurs.
9. Start two pici processes at the refresh boundary and verify the credential
   remains valid JSON and only one refresh is accepted.
10. Run ACP with the same isolated store and complete a request.
11. Run logout; verify status reports not logged in and subsequent model use
    produces the documented remediation.
12. Search captured stdout/stderr, stream diagnostics, sessions, and logs for
    the test token/account ID using a local secure procedure. Do not paste the
    values into shell history or the PR.

Delete the isolated credential file after the test using an explicit path.

## Final verification

During the edit loop, prefer:

```sh
make dev
cmake --build build --target test-openai_codex_oauth --parallel
cmake --build build --target test-openai_codex_responses --parallel
ctest --test-dir build -R 'credential|openai_codex|http' --output-on-failure
```

Before every commit:

```sh
make format
make test
```

Before the final handoff:

```sh
make format-check
make lint
make test
```

If the touched credential/resolver code has meaningful concurrent execution,
also run the repository's TSAN target on a supported Linux environment. Treat
new warnings in touched code as failures even though the broader tidy target
is advisory.

## Definition of done

The feature is complete only when all of the following are true:

- Existing API-key and unauthenticated local providers pass unchanged tests.
- Browser login persists a valid owner-only OAuth credential.
- Device login works or is explicitly deferred behind a separately approved
  scope change; it may not be silently half-implemented.
- Status and logout work without network access or secret disclosure.
- Expiring tokens refresh atomically and concurrent refresh is serialized.
- `openai-codex` requests use the Codex Responses endpoint and required account
  headers, never Chat Completions.
- Text, images, reasoning replay, tools, tool results, usage, errors, and
  cancellation all have fixture coverage.
- CLI and ACP share one resolver implementation.
- No credential is serialized into sessions/events/diagnostics or logged.
- Model metadata is verified and its provenance recorded.
- README/help clearly distinguish Platform API keys from ChatGPT subscription
  authentication and label the latter experimental.
- `make format-check`, `make lint`, and `make test` complete successfully.
- The manual smoke test has been completed or explicitly waived in the PR by
  the repository owner.

## Risk register

| Risk | Consequence | Mitigation |
|---|---|---|
| ChatGPT OAuth/backend is not a public third-party contract | Sudden breakage or unacceptable client-ID reuse | Mandatory compatibility/owner gate; experimental provider; stable API-key default |
| Refresh-token rotation races | User is logged out or credential file is corrupted | Cross-process lock, re-read under lock, refresh once, atomic write |
| Secret leakage through errors/verbose diagnostics | Account compromise | Structured redaction, no header/body logging, secret-scanning tests/manual audit |
| Current HTTP streaming discards useful non-2xx body context | Poor auth diagnostics | Bounded retained error body and explicit HTTP tests before provider work |
| Responses event variants evolve | Missing/corrupted output | Ignore unknown events safely, require terminal event, authoritative done reconciliation, fixture provenance |
| Model catalog becomes stale | Selection failures or misleading limits/cost | Verified provenance, no fabricated values, explicit experimental status |
| Callback port unavailable or remote shell cannot receive loopback redirect | Login failure | Clear bind error and device-code path |
| Credential file permissions/symlink attack | Token theft or overwrite | Owner-only creation, `O_NOFOLLOW`/`lstat`, same-directory atomic rename |
| ACP concurrent refresh | Duplicate rotation | Shared resolver plus file lock and double-check under lock |
| Encrypted reasoning stored in session files | Sensitive opaque provider state at rest | Document it, preserve existing session protections, never render/log it |

## Implementation discipline for Luna

- Start with Phase 0 and report the compatibility-gate result before writing
  OAuth production code.
- Use the vendored implementation as a behavioral oracle, not as permission to
  copy stale constants blindly.
- Keep each parser and protocol helper independently unit-testable; do not make
  tests reach anonymous-namespace internals through live networking.
- Prefer dependency injection for clock, endpoints, random-byte source, token
  exchange, and credential path. Production defaults remain closed and simple.
- Do not “simplify” by reading Codex's auth cache in place, passing OAuth access
  tokens as ordinary API keys, or sending ChatGPT credentials to the Platform
  API.
- Do not update unrelated generated decks or user-owned files.
- If a phase reveals a required behavior not covered here, update this plan or
  record the deviation and rationale before broadening the implementation.
