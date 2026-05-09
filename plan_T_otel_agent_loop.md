# Plan T — OpenTelemetry instrumentation in agent_loop.cpp

## Goal

Add OpenTelemetry tracing directly inside `agent_loop.cpp` at the points where work actually happens — LLM API calls, context transformation, tool execution, and hook timing. Instrumentation is opt-in: when no SDK is configured the OTel API compiles to a noop tracer and the instrumentation paths are inlined away. Rendering is entirely uninvolved.

## Non-goals

- Renderer instrumentation
- Metrics (`MeterProvider`) — deferred to a separate plan
- Distributed trace propagation (W3C `traceparent` on outbound LLM calls) — deferred
- Automatic / monkey-patch instrumentation — everything is explicit

---

## Span hierarchy

```
agent.session                       ← one per run_agent_loop() invocation
└── agent.turn                      ← one per inner-loop iteration (mirrors TurnStart/TurnEndEvent)
    ├── llm.transform_context       ← only when config.transform_context is set
    ├── llm.request                 ← one per stream_assistant_response() call
    └── tool.call  [name=bash]      ← one per ToolCall (parallel siblings OK)
    └── tool.call  [name=read_file]
```

**`agent.turn` definition**: one inner-loop iteration = one LLM call + the tool batch that follows (if any). Mirrors the existing `TurnStartEvent`/`TurnEndEvent` in the code. Multiple turns occur within one session when the model keeps calling tools or when steering/follow-up messages arrive.

`tool.batch` is dropped — `agent.turn` already scopes exactly one batch. `tool.execute` is dropped — it is identical in scope to `tool.call`. `tool.prepare` and `tool.finalize` are **span events** on `tool.call` (timestamps without sub-span overhead), unless profiling shows either hook consistently exceeds ~10ms, in which case promote to spans.

---

## Semantic conventions

Following [OTel Gen AI semantic conventions](https://opentelemetry.io/docs/specs/semconv/gen-ai/). Non-standard attributes use `pi.*` or `anthropic.*` namespaces rather than squatting on `gen_ai.*`.

### `agent.session` span
| Attribute | Value |
|---|---|
| `agent.model` | `config.model.id` |
| `agent.provider` | `config.model.provider` |
| `session.id` | `config.session_id` (if set) |

### `agent.turn` span
| Attribute | Value |
|---|---|
| `agent.tool_calls_count` | count of tool calls in this turn |

### `llm.request` span
| Attribute | Value |
|---|---|
| `gen_ai.system` | `config.model.provider` |
| `gen_ai.request.model` | `config.model.id` |
| `gen_ai.response.model` | `final_msg->model` |
| `gen_ai.operation.name` | `"chat"` |
| `gen_ai.usage.input_tokens` | `final_msg->usage.input` |
| `gen_ai.usage.output_tokens` | `final_msg->usage.output` |
| `anthropic.usage.cache_read_input_tokens` | `final_msg->usage.cache_read` |
| `anthropic.usage.cache_creation_input_tokens` | `final_msg->usage.cache_write` |
| `gen_ai.response.finish_reasons` | `string[]` — one-element array of stop_reason string |
| `gen_ai.response.id` | Anthropic message ID (from `on_response` hook) |
| `http.response.status_code` | HTTP status (from `on_response` hook) |
| `llm.has_thinking` | bool — thinking block present in response |
| `pi.llm.time_to_first_token_ms` | ms from span start to first `AssistantMessageStartEvent` |
| Event `pi.llm.first_token` | fired when `AssistantMessageStartEvent` arrives |

### `tool.call` span
| Attribute | Value |
|---|---|
| `gen_ai.tool.name` | `tc.name` |
| `gen_ai.tool.call.id` | `tc.id` |
| `gen_ai.tool.type` | `"function"` |
| `tool.is_error` | `finalized.is_error` |
| `tool.blocked` | true if `before_tool_call` blocked execution |
| `pi.tool.result_bytes` | length in bytes of the tool result content |
| Event `tool.prepare.start` / `tool.prepare.end` | bracket the prepare phase |
| Event `tool.finalize.start` / `tool.finalize.end` | bracket the finalize phase |

### `llm.transform_context` span
| Attribute | Value |
|---|---|
| `pi.transform.input_message_count` | message count before transform |
| `pi.transform.output_message_count` | message count after transform |

---

## Build integration

### Option A vs B — chosen approach

**Use Option B**: always link the OTel C++ *API* (header-only, zero transitive link-time deps). The SDK and exporter are gated behind `PI_CPP_OTEL_SDK`. When only the API is present, `Provider::GetTracerProvider()` returns a `NoopTracerProvider`; all `StartSpan` calls return a `NoopSpan` whose methods inline to nothing.

Split the CMake option into two:
- `PI_CPP_OTEL_API` (default `ON`) — links `opentelemetry-cpp::api`, enables instrumentation code
- `PI_CPP_OTEL_SDK` (default `OFF`) — links the SDK + OTLP/HTTP exporter for real export

This means instrumentation always compiles and is exercised by CI; actual export is an opt-in runtime concern.

### CMakeLists.txt additions

```cmake
option(PI_CPP_OTEL_API "Compile OpenTelemetry API instrumentation" ON)
option(PI_CPP_OTEL_SDK "Link OpenTelemetry SDK + OTLP/HTTP exporter" OFF)

if(PI_CPP_OTEL_API)
  # OVERRIDE_FIND_PACKAGE: otel-cpp's internal find_package(nlohmann_json REQUIRED)
  # resolves to the copy we already fetched, preventing a version conflict.
  # Requires CMake >= 3.24.
  FetchContent_Declare(nlohmann_json          # re-declare with the override flag
    GIT_REPOSITORY https://github.com/nlohmann/json
    GIT_TAG        v3.11.3
    OVERRIDE_FIND_PACKAGE
  )

  # WITH_STL=ON replaces nostd::* with std:: aliases. Must be set before
  # FetchContent_MakeAvailable and must match across all translation units —
  # mismatched STL mode causes silent ABI corruption.
  set(WITH_STL                    ON  CACHE BOOL "" FORCE)
  set(WITH_ABSEIL                 OFF CACHE BOOL "" FORCE)
  set(BUILD_TESTING               OFF CACHE BOOL "" FORCE)
  set(WITH_EXAMPLES               OFF CACHE BOOL "" FORCE)
  set(WITH_BENCHMARK              OFF CACHE BOOL "" FORCE)
  set(WITH_OTLP_HTTP              OFF CACHE BOOL "" FORCE)  # off unless SDK requested
  set(WITH_OTLP_GRPC              OFF CACHE BOOL "" FORCE)
  set(WITH_JAEGER                 OFF CACHE BOOL "" FORCE)
  set(OPENTELEMETRY_INSTALL       OFF CACHE BOOL "" FORCE)

  if(PI_CPP_OTEL_SDK)
    set(WITH_OTLP_HTTP            ON  CACHE BOOL "" FORCE)
  endif()

  FetchContent_MakeAvailable(nlohmann_json opentelemetry-cpp)

  target_link_libraries(pi-core PUBLIC opentelemetry-cpp::api)
  target_compile_definitions(pi-core PUBLIC
    PI_CPP_OTEL_ENABLED
    OPENTELEMETRY_STL_VERSION=2017   # pin STL mode globally
  )

  if(PI_CPP_OTEL_SDK)
    target_link_libraries(pi-core PUBLIC
      opentelemetry-cpp::sdk
      opentelemetry-cpp::otlp_http_exporter
    )
    target_compile_definitions(pi-core PUBLIC PI_CPP_OTEL_SDK_ENABLED)
  endif()
endif()
```

**nlohmann_json conflict**: `OVERRIDE_FIND_PACKAGE` (CMake ≥ 3.24) intercepts otel-cpp's `find_package(nlohmann_json REQUIRED)` and routes it to our already-fetched copy. Verify the project's `cmake_minimum_required` is ≥ 3.24 before using this.

---

## AgentLoopConfig extension

Add to `src/core/agent_loop.h`:

```cpp
#ifdef PI_CPP_OTEL_ENABLED
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/tracer.h>
#endif

struct AgentLoopConfig {
  // ... existing fields ...

#ifdef PI_CPP_OTEL_ENABLED
  // OTel tracer. Defaults to the global provider's tracer — a no-op unless
  // the SDK is initialised at startup via pi::init_otel(endpoint).
  opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> tracer{
      opentelemetry::trace::Provider::GetTracerProvider()
          ->GetTracer("pi-agent", PI_CPP_VERSION)
  };
#endif
};
```

---

## Context passing strategy

The parallel tool path uses `std::async` — child tasks run on arbitrary threads. OTel thread-local context does **not** automatically propagate across `std::async` boundaries.

**Rule**: pass `opentelemetry::context::Context` explicitly through all internal functions that create spans. Never call `RuntimeContext::GetCurrent()` across a thread boundary to retrieve a parent.

**Rule**: at the top of every `std::async` lambda (and the detached worker thread), call `RuntimeContext::Attach(ctx)` to install the context for that thread. This makes `GetCurrent()` return the right parent for any inner instrumentation the tool itself may create.

Affected internal signatures (additions bolded — these functions are `static` in `agent_loop.cpp`, not in the public header):

```cpp
std::shared_ptr<AssistantMessage>
stream_assistant_response(AgentContext &, const AgentLoopConfig &,
                          StreamCallback, const std::stop_token &,
                          opentelemetry::context::Context parent_ctx);   // NEW

ToolCallResult
execute_tool_calls(AgentContext &, const AssistantMessage &,
                   const AgentLoopConfig &, const StreamCallback &,
                   const std::stop_token &,
                   opentelemetry::context::Context parent_ctx);          // NEW

static ToolCallResult execute_tool_calls_sequential(
    ..., opentelemetry::context::Context parent_ctx);

static ToolCallResult execute_tool_calls_parallel(
    ..., opentelemetry::context::Context parent_ctx);
```

Context creation pattern (explicit, avoids designated-initializer fragility):

```cpp
// Create a context that carries span as the active span
opentelemetry::trace::StartSpanOptions child_opts;
child_opts.parent = parent_ctx;
auto span = tracer->StartSpan("name", attrs, child_opts);

// Build a context for passing to children of this span
auto ctx = opentelemetry::trace::SetSpan(
    opentelemetry::context::RuntimeContext::GetCurrent(), span);
```

**Do not use** `StartSpanOptions{.parent = ctx}` designated initializers — field order in `StartSpanOptions` places `parent` after `kind`, `start_system_time`, `start_steady_time`. While C++20 allows skipping fields in designated init, future OTel versions may insert fields before `parent`, causing silent miscompilation. The explicit two-line form is safe across versions.

---

## Instrumentation per function

All snippets below are guarded by `#ifdef PI_CPP_OTEL_ENABLED` in the actual implementation; the macro and the noop guard are elided here for readability.

### `run_agent_loop` worker thread

```cpp
auto &tracer = config.tracer;

// Session span: one per run_agent_loop() invocation
opentelemetry::trace::StartSpanOptions session_opts;
auto session_span = tracer->StartSpan("agent.session", {
    {"agent.model",    config.model.id},
    {"agent.provider", config.model.provider},
}, session_opts);
if (config.session_id)
    session_span->SetAttribute("session.id", *config.session_id);
auto session_ctx = opentelemetry::trace::SetSpan(
    opentelemetry::context::RuntimeContext::GetCurrent(), session_span);
auto session_scope = opentelemetry::context::RuntimeContext::Attach(session_ctx);

// ... outer follow-up loop ...

while (!stop_tok.stop_requested()) {
    // Turn span: one per inner iteration (mirrors TurnStart/TurnEndEvent)
    opentelemetry::trace::StartSpanOptions turn_opts;
    turn_opts.parent = session_ctx;
    auto turn_span = tracer->StartSpan("agent.turn", {}, turn_opts);
    auto turn_ctx = opentelemetry::trace::SetSpan(session_ctx, turn_span);
    auto turn_scope = opentelemetry::context::RuntimeContext::Attach(turn_ctx);

    // ... call stream_assistant_response and execute_tool_calls, passing turn_ctx ...

    if (stop_tok.stop_requested()) {
        turn_span->AddEvent("cancelled");
        turn_span->SetStatus(opentelemetry::trace::StatusCode::kError, "cancelled");
    }
    turn_span->SetAttribute("agent.tool_calls_count", (int64_t)tool_calls.size());
    turn_span->End();
}

session_span->End();
// session_scope destructor detaches context automatically
```

### `stream_assistant_response`

```cpp
// Capture response metadata via on_response hook (compose with user-supplied one)
std::string response_id;
int http_status = 0;
auto prev_on_response = opts.on_response;
opts.on_response = [&, prev = std::move(prev_on_response)](int status,
                       const std::map<std::string, std::string> &headers) {
    http_status = status;
    if (auto it = headers.find("x-request-id"); it != headers.end())
        response_id = it->second;
    if (prev) prev(status, headers);
};

// transform_context sub-span
if (config.transform_context) {
    opentelemetry::trace::StartSpanOptions xf_opts;
    xf_opts.parent = parent_ctx;
    auto xf_span = tracer->StartSpan("llm.transform_context", {}, xf_opts);
    xf_span->SetAttribute("pi.transform.input_message_count", (int64_t)messages.size());
    messages = config.transform_context(messages, stop_tok);
    xf_span->SetAttribute("pi.transform.output_message_count", (int64_t)messages.size());
    xf_span->End();
}

// LLM request span
opentelemetry::trace::StartSpanOptions llm_opts;
llm_opts.parent = parent_ctx;
auto llm_span = tracer->StartSpan("llm.request", {
    {"gen_ai.system",         config.model.provider},
    {"gen_ai.request.model",  config.model.id},
    {"gen_ai.operation.name", "chat"},
}, llm_opts);

// First-token timing
auto req_start = std::chrono::steady_clock::now();
bool first_token_seen = false;

auto on_event_with_trace = [&](const AssistantMessageEvent &ev) {
    if (!first_token_seen) {
        if (std::holds_alternative<AssistantMessageStartEvent>(ev)) {
            first_token_seen = true;
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - req_start).count();
            llm_span->AddEvent("pi.llm.first_token");
            llm_span->SetAttribute("pi.llm.time_to_first_token_ms", elapsed_ms);
        }
    }
    on_event(ev);  // original callback
};

auto final_msg = client->stream(config.model, context, opts, on_event_with_trace, stop_tok);

// Attach response attributes
if (http_status) llm_span->SetAttribute("http.response.status_code", (int64_t)http_status);
if (!response_id.empty()) llm_span->SetAttribute("gen_ai.response.id", response_id);
llm_span->SetAttribute("gen_ai.response.model",         final_msg->model);
llm_span->SetAttribute("gen_ai.usage.input_tokens",     (int64_t)final_msg->usage.input);
llm_span->SetAttribute("gen_ai.usage.output_tokens",    (int64_t)final_msg->usage.output);
llm_span->SetAttribute("anthropic.usage.cache_read_input_tokens",
                        (int64_t)final_msg->usage.cache_read);
llm_span->SetAttribute("anthropic.usage.cache_creation_input_tokens",
                        (int64_t)final_msg->usage.cache_write);
llm_span->SetAttribute("llm.has_thinking", has_thinking_block(final_msg->content));

// finish_reasons is string[] per semconv
std::string_view finish_reason = stop_reason_to_string(final_msg->stop_reason);
llm_span->SetAttribute("gen_ai.response.finish_reasons",
    opentelemetry::nostd::span<const std::string_view>{&finish_reason, 1});

if (final_msg->stop_reason == StopReason::error) {
    llm_span->SetStatus(opentelemetry::trace::StatusCode::kError,
                        final_msg->error_message.value_or("LLM error"));
}
llm_span->End();
```

### `execute_tool_calls_sequential`

```cpp
for (const auto &tc : tool_calls) {
    emit(ToolExecutionStartEvent(...));  // unchanged

    opentelemetry::trace::StartSpanOptions tc_opts;
    tc_opts.parent = parent_ctx;
    auto tool_span = tracer->StartSpan("tool.call", {
        {"gen_ai.tool.name",    tc.name},
        {"gen_ai.tool.call.id", tc.id},
        {"gen_ai.tool.type",    "function"},
    }, tc_opts);
    auto tool_ctx = opentelemetry::trace::SetSpan(parent_ctx, tool_span);

    // Prepare phase — mark with events, not sub-spans
    tool_span->AddEvent("tool.prepare.start");
    auto prepared = prepare_tool_call(context, assistant_message, tc, config, stop_tok);
    tool_span->AddEvent("tool.prepare.end");

    if (auto *immediate = std::get_if<FinalizedToolCall>(&prepared)) {
        tool_span->SetAttribute("tool.blocked", immediate->is_error);
        tool_span->SetAttribute("tool.is_error", immediate->is_error);
        if (immediate->is_error)
            tool_span->SetStatus(opentelemetry::trace::StatusCode::kError, "");
        tool_span->End();
        result.messages.push_back(emit_finalized_tool_call(*immediate, emit));
        finalized_calls.push_back(std::move(*immediate));
        continue;
    }

    auto call = std::get<PreparedToolCall>(std::move(prepared));

    // Execute phase
    auto tool_result = call.tool->execute(
        call.tool_call.id, call.args_json, stop_tok,
        [&](const auto &partial) { emit(ToolExecutionUpdateEvent(...)); });

    // Finalize phase
    tool_span->AddEvent("tool.finalize.start");
    auto finalized = finalize_tool_call(
        context, assistant_message, call.tool_call,
        std::move(tool_result), false, config, call.args_json, stop_tok);
    tool_span->AddEvent("tool.finalize.end");

    // Result attributes — set before End()
    tool_span->SetAttribute("tool.is_error", finalized.is_error);
    if (finalized.is_error)
        tool_span->SetStatus(opentelemetry::trace::StatusCode::kError, "");
    if (finalized.result) {
        tool_span->SetAttribute("pi.tool.result_bytes",
            (int64_t)finalized.result->content().size());
    }
    tool_span->End();

    result.messages.push_back(emit_finalized_tool_call(finalized, emit));
    finalized_calls.push_back(std::move(finalized));
}
```

### `execute_tool_calls_parallel`

Key differences from sequential: `tool_ctx` captured by value into the `std::async` lambda; `RuntimeContext::Attach` called at the top of the lambda to install the context on the async thread.

```cpp
for (std::size_t i = 0; i < n; ++i) {
    const auto &tc = tool_calls[i];
    emit(ToolExecutionStartEvent(...));  // unchanged

    opentelemetry::trace::StartSpanOptions tc_opts;
    tc_opts.parent = parent_ctx;
    auto tool_span = tracer->StartSpan("tool.call", {
        {"gen_ai.tool.name",    tc.name},
        {"gen_ai.tool.call.id", tc.id},
        {"gen_ai.tool.type",    "function"},
    }, tc_opts);
    auto tool_ctx = opentelemetry::trace::SetSpan(parent_ctx, tool_span);

    // Prepare runs on the worker thread
    tool_span->AddEvent("tool.prepare.start");
    auto prepared = prepare_tool_call(context, assistant_message, tc, config, stop_tok);
    tool_span->AddEvent("tool.prepare.end");

    if (auto *immediate = std::get_if<FinalizedToolCall>(&prepared)) {
        tool_span->SetAttribute("tool.blocked", immediate->is_error);
        tool_span->SetAttribute("tool.is_error", immediate->is_error);
        if (immediate->is_error)
            tool_span->SetStatus(opentelemetry::trace::StatusCode::kError, "");
        tool_span->End();
        emit(ToolExecutionEndEvent(...));
        slots[i] = std::move(*immediate);
        continue;
    }

    auto call = std::get<PreparedToolCall>(std::move(prepared));

    // Capture tool_span and tool_ctx by value — keeps the span alive across threads.
    // tool_span->End() is called from the async thread; this is safe per OTel spec
    // (Span::End acquires an internal mutex; the shared_ptr refcount is atomic with
    // WITH_STL=ON).
    pending.push_back(std::async(
        std::launch::async,
        [tracer, tool_span, tool_ctx,
         call = std::move(call), emit, stop_tok, idx = i,
         &context, &assistant_message, &config]() mutable
        {
            // Attach context to this thread so any inner instrumentation
            // (e.g., inside tool->execute) can discover the parent via GetCurrent().
            auto scope = opentelemetry::context::RuntimeContext::Attach(tool_ctx);

            auto tool_result = call.tool->execute(
                call.tool_call.id, call.args_json, stop_tok,
                [emit, call](const auto &partial) {
                    emit(ToolExecutionUpdateEvent(...));
                });

            tool_span->AddEvent("tool.finalize.start");
            auto finalized = finalize_tool_call(
                context, assistant_message, call.tool_call,
                std::move(tool_result), false, config, call.args_json, stop_tok);
            tool_span->AddEvent("tool.finalize.end");

            // All SetAttribute calls must precede End()
            tool_span->SetAttribute("tool.is_error", finalized.is_error);
            if (finalized.is_error)
                tool_span->SetStatus(opentelemetry::trace::StatusCode::kError, "");
            if (finalized.result)
                tool_span->SetAttribute("pi.tool.result_bytes",
                    (int64_t)finalized.result->content().size());
            tool_span->End();

            emit(ToolExecutionEndEvent(...));
            return std::make_pair(idx, std::move(finalized));
        }));
}
```

---

## CLI / ACP wiring

### `src/core/otel_init.h` / `otel_init.cpp` (new files)

Centralise SDK initialisation to avoid duplication between `main.cpp` and `acp/`:

```cpp
// otel_init.h
namespace pi::core {
// Initialise the global OTel tracer provider with an OTLP/HTTP exporter.
// endpoint: base URL, e.g. "http://localhost:4318".
// No-op if PI_CPP_OTEL_SDK_ENABLED is not defined.
void init_otel(std::string_view endpoint);

// Flush and shut down the global tracer provider.
// Must be called before process exit when init_otel was called.
// Safe to call multiple times.
void shutdown_otel();
}
```

```cpp
// otel_init.cpp
#ifdef PI_CPP_OTEL_SDK_ENABLED
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>

void pi::core::init_otel(std::string_view endpoint) {
    namespace otlp = opentelemetry::exporter::otlp;
    otlp::OtlpHttpExporterOptions opts;
    opts.url = std::string(endpoint) + "/v1/traces";

    auto exporter  = otlp::OtlpHttpExporterFactory::Create(opts);

    opentelemetry::sdk::trace::BatchSpanProcessorOptions bp_opts;
    bp_opts.schedule_delay_millis = std::chrono::milliseconds(1000);
    auto processor = opentelemetry::sdk::trace::BatchSpanProcessorFactory::Create(
                         std::move(exporter), bp_opts);
    auto provider  = opentelemetry::sdk::trace::TracerProviderFactory::Create(
                         std::move(processor));
    opentelemetry::trace::Provider::SetTracerProvider(std::move(provider));
}

void pi::core::shutdown_otel() {
    auto p = opentelemetry::trace::Provider::GetTracerProvider();
    if (auto sdk = std::dynamic_pointer_cast<
            opentelemetry::sdk::trace::TracerProvider>(p)) {
        sdk->ForceFlush(std::chrono::seconds(5));
        sdk->Shutdown();
    }
}
#else
void pi::core::init_otel(std::string_view) {}
void pi::core::shutdown_otel() {}
#endif
```

### RAII shutdown guard

Declare once as a local in `main()`:

```cpp
struct OtelGuard {
    ~OtelGuard() { pi::core::shutdown_otel(); }
} otel_guard;

if (!otel_endpoint.empty())
    pi::core::init_otel(otel_endpoint);
```

This handles both clean exit and exceptions. `Ctrl-C` via SIGINT still requires a signal handler or `std::atexit` call — add `std::atexit([]{ pi::core::shutdown_otel(); })` alongside the guard for signal safety. `BatchSpanProcessor::Shutdown` is signal-safe (it only sets an atomic and joins the exporter thread).

### `main.cpp`

```cpp
// Add flag
std::string otel_endpoint;  // --otel-endpoint http://localhost:4318

// In agent config setup:
config.tracer = opentelemetry::trace::Provider::GetTracerProvider()
                    ->GetTracer("pi-agent", PI_CPP_VERSION);
```

Default endpoint: `http://localhost:4318/v1/traces` (OTLP/HTTP default — Jaeger ≥ 1.35, Grafana Tempo, OTel Collector all accept this).

---

## Thread safety summary

| Span | Created on | `End()` called on | Shared refs |
|---|---|---|---|
| `agent.session` | worker thread | worker thread | No |
| `agent.turn` | worker thread | worker thread | No |
| `llm.request` | worker thread | worker thread | No — `on_event` callback also runs on worker thread |
| `llm.transform_context` | worker thread | worker thread | No |
| `tool.call` (sequential) | worker thread | worker thread | No |
| `tool.call` (parallel) | worker thread | async thread | `tool_span` shared_ptr captured by value into lambda |
| No sub-spans for parallel tools | — | — | — |

`Span::End()` acquires an internal mutex in the C++ SDK. `std::shared_ptr` refcount (with `WITH_STL=ON`) is atomic. The pattern of creating a span on thread A and calling `End()` from thread B is spec-correct and SDK-safe. **All `SetAttribute` calls must precede `End()`** — attributes after `End()` are silently dropped.

---

## What this does NOT capture

- **HTTP-level breakdown** (TCP connect, TLS, TTFB): would require instrumenting `cpp-httplib` internals. Not worth it — `pi.llm.time_to_first_token_ms` gives the user-visible latency.
- **Token streaming rate**: per-delta is too granular for spans; belongs in a future metrics plan.
- **`should_stop_after_turn`, `get_steering_messages`, `get_follow_up_messages`**: in-process callbacks, typically microseconds. Added as events on `agent.turn` if they ever block visibly.
- **Retry count**: `LLMClient` handles retries internally. `on_response` fires per attempt; if retries surface there, `pi.llm.retry_count` can be tracked on `llm.request`.

---

## Implementation phases

| Phase | Deliverable | Files | Effort |
|---|---|---|---|
| T1 | CMake: `PI_CPP_OTEL_API` (ON) / `PI_CPP_OTEL_SDK` (OFF); `nlohmann_json` `OVERRIDE_FIND_PACKAGE` | CMakeLists.txt | 2h |
| T2 | `AgentLoopConfig.tracer`; `init_otel` / `shutdown_otel` helpers | agent_loop.h, otel_init.h/cpp | 1h |
| T3 | `agent.session` + `agent.turn` spans in both `run_agent_loop` variants | agent_loop.cpp | 2h |
| T4 | `llm.request` span + first-token timing + `on_response` capture | agent_loop.cpp | 2h |
| T5 | `tool.call` spans (sequential + parallel) with prepare/finalize events | agent_loop.cpp | 3h |
| T6 | `--otel-endpoint` flag + RAII guard in `main.cpp` and `acp/` | main.cpp, acp/handlers.cpp | 1h |

Total: ~1.5 days. Phases are independent after T1+T2.
