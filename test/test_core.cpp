#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/agent_state.h"
#include "core/event_json.h"
#include "core/event_types.h"
#include "core/message_types.h"
#include "core/models.h"
#include "core/providers/transform_messages.h"
#include "core/session/session_store.h"
#include "core/stream.h"
#include "core/stream_renderer.h"

using namespace pi::core;

namespace {
Message make_user_message(std::string text) {
    UserMessage message;
    message.content.emplace_back(TextContent{.text = std::move(text)});
    return Message{std::move(message)};
}
} // namespace

// ─── Simple test harness ──────────────────────────────────────────────────

namespace tests {

int passed{0};
int failed{0};
int total{0};
int current_failed{0};

struct TestResult {
    std::string name;
    bool ok;
    std::string message;
};

std::vector<TestResult> results;

bool CHECK_impl(bool cond, bool expected,
                std::string_view expr,
                std::source_location loc = std::source_location::current()) {
    if (cond != expected) {
        current_failed++;
        std::cerr << "  FAIL " << loc.file_name() << ":" << loc.line()
                  << " - " << expr << " (expected " << expected << ", got " << cond << ")\n";
        return false;
    }
    return true;
}

bool CHECK_impl_str(std::string_view actual, std::string_view expected,
                    std::string_view expr,
                    std::source_location loc = std::source_location::current()) {
    if (actual != expected) {
        current_failed++;
        std::cerr << "  FAIL " << loc.file_name() << ":" << loc.line()
                  << " - " << expr << " (expected \"" << expected << "\", got \""
                  << actual << "\")\n";
        return false;
    }
    return true;
}

#define CHECK(cond) \
    (::tests::CHECK_impl(static_cast<bool>(cond), true, #cond, \
                         std::source_location::current()))

#define CHECK_STR(a, b) \
    (::tests::CHECK_impl_str(a, b, #a " == " #b, \
                             std::source_location::current()))

#define CHECK_EQ(a, b) \
    (::tests::CHECK_impl((a) == (b), true, #a " == " #b, \
                         std::source_location::current()))

#define CHECK_NEQ(a, b) \
    (::tests::CHECK_impl((a) != (b), true, #a " != " #b, \
                         std::source_location::current()))

void register_test(std::string name, std::function<void()> fn) {
    total++;
    current_failed = 0;
    fn();
    if (current_failed == 0) {
        passed++;
    } else {
        failed++;
    }
    results.push_back({name, current_failed == 0, ""});
}

void print_summary() {
    std::cout << "\n========================================\n";
    std::cout << "  Tests: " << total << " total, "
              << passed << " passed, "
              << failed << " failed\n";
    std::cout << "========================================\n";

    if (failed > 0) {
        std::cout << "\nFailed tests:\n";
        for (const auto& r : results) {
            if (!r.ok) {
                std::cout << "  - " << r.name << "\n";
            }
        }
    }
}

} // namespace tests

// ─── StopReason tests ─────────────────────────────────────────────────────

void test_stop_reason_conversion() {
    tests::register_test("StopReason: conversion", []() {
        CHECK_EQ(stop_reason_to_string(StopReason::stop), "stop");
        CHECK_EQ(stop_reason_to_string(StopReason::length), "length");
        CHECK_EQ(stop_reason_to_string(StopReason::tool_use), "toolUse");
        CHECK_EQ(stop_reason_to_string(StopReason::error), "error");
        CHECK_EQ(stop_reason_to_string(StopReason::aborted), "aborted");

        CHECK_EQ(stop_reason_from_string("stop"), StopReason::stop);
        CHECK_EQ(stop_reason_from_string("length"), StopReason::length);
        CHECK_EQ(stop_reason_from_string("toolUse"), StopReason::tool_use);
        CHECK_EQ(stop_reason_from_string("error"), StopReason::error);
        CHECK_EQ(stop_reason_from_string("aborted"), StopReason::aborted);
    });
}

void test_stop_reason_output() {
    tests::register_test("StopReason: output", []() {
        std::ostringstream oss;
        oss << StopReason::stop;
        CHECK_EQ(oss.str(), "stop");
    });
}

// ─── ThinkingLevel tests ──────────────────────────────────────────────────

void test_thinking_level_conversion() {
    tests::register_test("ThinkingLevel: conversion", []() {
        CHECK_EQ(thinking_level_to_string(ThinkingLevel::off), "off");
        CHECK_EQ(thinking_level_to_string(ThinkingLevel::minimal), "minimal");
        CHECK_EQ(thinking_level_to_string(ThinkingLevel::low), "low");
        CHECK_EQ(thinking_level_to_string(ThinkingLevel::medium), "medium");
        CHECK_EQ(thinking_level_to_string(ThinkingLevel::high), "high");
        CHECK_EQ(thinking_level_to_string(ThinkingLevel::xhigh), "xhigh");

        CHECK_EQ(thinking_level_from_string("off"), ThinkingLevel::off);
        CHECK_EQ(thinking_level_from_string("minimal"), ThinkingLevel::minimal);
        CHECK_EQ(thinking_level_from_string("low"), ThinkingLevel::low);
        CHECK_EQ(thinking_level_from_string("medium"), ThinkingLevel::medium);
        CHECK_EQ(thinking_level_from_string("high"), ThinkingLevel::high);
        CHECK_EQ(thinking_level_from_string("xhigh"), ThinkingLevel::xhigh);
        CHECK_EQ(thinking_level_from_string("unknown"), ThinkingLevel::off);
    });
}

// ─── Message JSON tests ───────────────────────────────────────────────────

void test_user_message_json() {
    tests::register_test("Message: UserMessage JSON round-trip", []() {
        UserMessage msg;
        msg.timestamp = 1234567890;
        TextContent tc;
        tc.text = "Hello, world!";
        msg.content.push_back(std::move(tc));

        std::string json_str = json::to_json(msg);
        auto parsed = json::from_json(json_str);

        CHECK(parsed.has_value());
        CHECK(std::holds_alternative<UserMessage>(*parsed));

        auto& parsed_msg = std::get<UserMessage>(*parsed);
        CHECK_EQ(parsed_msg.timestamp, msg.timestamp);
        CHECK_EQ(parsed_msg.content.size(), std::size_t(1));
        CHECK_EQ(std::get<TextContent>(parsed_msg.content[0]).text, "Hello, world!");
    });
}

void test_assistant_message_json() {
    tests::register_test("Message: AssistantMessage JSON round-trip", []() {
        AssistantMessage msg;
        msg.api = "openai-completions";
        msg.provider = "openai";
        msg.model = "gpt-4";
        msg.stop_reason = StopReason::stop;
        msg.timestamp = 9876543210;

        TextContent tc;
        tc.text = "I can help you with that.";
        msg.content.push_back(std::move(tc));

        msg.usage.input = 100;
        msg.usage.output = 50;
        msg.usage.cache_read = 80;
        msg.usage.cache_write = 20;
        msg.usage.total_tokens = 150;

        std::string json_str = json::to_json(msg);
        auto parsed = json::from_json(json_str);

        CHECK(parsed.has_value());
        CHECK(std::holds_alternative<AssistantMessage>(*parsed));

        auto& parsed_msg = std::get<AssistantMessage>(*parsed);
        CHECK_EQ(parsed_msg.api, "openai-completions");
        CHECK_EQ(parsed_msg.provider, "openai");
        CHECK_EQ(parsed_msg.model, "gpt-4");
        CHECK_EQ(parsed_msg.stop_reason, StopReason::stop);
        CHECK_EQ(parsed_msg.usage.input, 100ULL);
        CHECK_EQ(parsed_msg.usage.output, 50ULL);
        CHECK_EQ(parsed_msg.usage.cache_read, 80ULL);
        CHECK_EQ(parsed_msg.usage.cache_write, 20ULL);
        CHECK_EQ(parsed_msg.usage.total_tokens, 150ULL);
    });
}

void test_tool_result_message_json() {
    tests::register_test("Message: ToolResultMessage JSON round-trip", []() {
        ToolResultMessage msg;
        msg.tool_call_id = "call_abc123";
        msg.tool_name = "bash";
        msg.is_error = false;
        msg.timestamp = 1111111111;

        TextContent tc;
        tc.text = "Command executed successfully";
        msg.content.push_back(std::move(tc));

        std::string json_str = json::to_json(msg);
        auto parsed = json::from_json(json_str);

        CHECK(parsed.has_value());
        CHECK(std::holds_alternative<ToolResultMessage>(*parsed));

        auto& parsed_msg = std::get<ToolResultMessage>(*parsed);
        CHECK_EQ(parsed_msg.tool_call_id, "call_abc123");
        CHECK_EQ(parsed_msg.tool_name, "bash");
        CHECK(!parsed_msg.is_error);
    });
}

void test_context_compaction_message_json() {
    tests::register_test("Message: ContextCompactionMessage JSON round-trip", []() {
        ContextCompactionMessage msg;
        msg.api = "openai-codex-responses";
        msg.provider = "openai-codex";
        msg.model = "gpt-5.3-codex";
        msg.encrypted_content = "opaque-server-bytes-not-plaintext";
        msg.item_id = "cmp_123";
        msg.response_id = "resp_456";
        msg.timestamp = 42;

        std::string json_str = json::to_json(msg);
        // The opaque payload must round-trip byte-for-byte and must never be
        // silently dropped, truncated, or reinterpreted.
        CHECK(json_str.find("opaque-server-bytes-not-plaintext") !=
              std::string::npos);

        auto parsed = json::from_json(json_str);
        CHECK(parsed.has_value());
        CHECK(std::holds_alternative<ContextCompactionMessage>(*parsed));

        auto& parsed_msg = std::get<ContextCompactionMessage>(*parsed);
        CHECK_STR(parsed_msg.api, "openai-codex-responses");
        CHECK_STR(parsed_msg.provider, "openai-codex");
        CHECK_STR(parsed_msg.model, "gpt-5.3-codex");
        CHECK_STR(parsed_msg.encrypted_content,
                  "opaque-server-bytes-not-plaintext");
        CHECK(parsed_msg.item_id.has_value());
        CHECK_STR(*parsed_msg.item_id, "cmp_123");
        CHECK(parsed_msg.response_id.has_value());
        CHECK_STR(*parsed_msg.response_id, "resp_456");
        CHECK_EQ(parsed_msg.timestamp, std::int64_t(42));

        // Also verify the compact jsonl form used for session persistence.
        auto line = json::to_jsonl_line(msg);
        auto parsed_line = json::from_json(line);
        CHECK(parsed_line.has_value());
        CHECK(std::holds_alternative<ContextCompactionMessage>(*parsed_line));
    });
}

void test_transform_messages_drops_orphan_tool_result() {
    tests::register_test("transform_messages: drops ToolResultMessage with no matching ToolCall", []() {
        // Simulates the state after compaction filtering has dropped the
        // AssistantMessage that owned this tool call: the orphaned result
        // must not survive, since sending it produces a bare
        // function_call_output with no matching function_call (rejected by
        // the Responses API).
        std::vector<Message> messages;
        messages.push_back(make_user_message("hello"));
        ToolResultMessage orphan;
        orphan.tool_call_id = "call_missing";
        orphan.tool_name = "bash";
        orphan.content = {TextContent{.text = "leftover output"}};
        messages.push_back(Message{orphan});
        messages.push_back(make_user_message("world"));

        Model model{.id = "gpt-5.3-codex",
                    .api = "openai-codex-responses",
                    .provider = "openai-codex"};
        auto result = transform_messages(messages, model);

        for (const auto& msg : result)
            CHECK(!std::holds_alternative<ToolResultMessage>(msg));
        CHECK_EQ(result.size(), std::size_t(2));
    });
}

void test_transform_messages_keeps_paired_tool_result() {
    tests::register_test("transform_messages: keeps ToolResultMessage paired with its ToolCall", []() {
        std::vector<Message> messages;
        messages.push_back(make_user_message("hello"));
        AssistantMessage am;
        am.api = "openai-codex-responses";
        am.provider = "openai-codex";
        am.model = "gpt-5.3-codex";
        am.stop_reason = StopReason::tool_use;
        ToolCall call;
        call.id = "call_1";
        call.name = "bash";
        am.content.emplace_back(std::move(call));
        messages.push_back(Message{am});
        ToolResultMessage result_msg;
        result_msg.tool_call_id = "call_1";
        result_msg.tool_name = "bash";
        result_msg.content = {TextContent{.text = "ok"}};
        messages.push_back(Message{result_msg});

        Model model{.id = "gpt-5.3-codex",
                    .api = "openai-codex-responses",
                    .provider = "openai-codex"};
        auto result = transform_messages(messages, model);

        bool found = false;
        for (const auto& msg : result) {
            if (const auto* trm = std::get_if<ToolResultMessage>(&msg)) {
                CHECK_STR(trm->tool_call_id, "call_1");
                found = true;
            }
        }
        CHECK(found);
    });
}

void test_transform_messages_compaction_same_model_gate() {
    tests::register_test("transform_messages: drops ContextCompactionMessage for a different model", []() {
        ContextCompactionMessage opaque;
        opaque.api = "openai-codex-responses";
        opaque.provider = "openai-codex";
        opaque.model = "gpt-5.3-codex";
        opaque.encrypted_content = "opaque-bytes";
        std::vector<Message> messages{Message{opaque}};

        Model same_model{.id = "gpt-5.3-codex",
                         .api = "openai-codex-responses",
                         .provider = "openai-codex"};
        auto kept = transform_messages(messages, same_model);
        CHECK_EQ(kept.size(), std::size_t(1));
        CHECK(std::holds_alternative<ContextCompactionMessage>(kept[0]));

        Model other_model{.id = "gpt-4",
                          .api = "openai-completions",
                          .provider = "openai"};
        auto dropped = transform_messages(messages, other_model);
        CHECK_EQ(dropped.size(), std::size_t(0));
    });
}

namespace {
// Minimal recording Renderer used to prove opaque compaction content is
// never surfaced through the renderer dispatch path.
class RecordingRenderer : public Renderer {
public:
    std::string text_deltas;
    std::vector<std::string> requests_text;
    int message_end_count{0};

    void on_text_delta(std::string_view delta) override {
        text_deltas += delta;
    }
    void on_request(const RendererRequest& request) override {
        requests_text.push_back(request.text);
    }
    void on_message_end(const TokenUsage&) override { ++message_end_count; }
};
} // namespace

void test_compaction_message_never_rendered() {
    tests::register_test("Renderer: ContextCompactionMessage is never rendered", []() {
        ContextCompactionMessage opaque;
        opaque.api = "openai-codex-responses";
        opaque.provider = "openai-codex";
        opaque.model = "gpt-5.3-codex";
        opaque.encrypted_content = "top-secret-opaque-bytes";

        RecordingRenderer renderer;
        MessageStartEvent start(Message{opaque});
        dispatch_event(AgentEvent{start}, renderer);
        MessageEndEvent end(Message{opaque});
        dispatch_event(AgentEvent{end}, renderer);

        CHECK(renderer.text_deltas.empty());
        CHECK(renderer.requests_text.empty());
        // MessageEndEvent only calls on_message_end_presentation for
        // AssistantMessage; a compaction item must not trigger it either.
        CHECK_EQ(renderer.message_end_count, 0);
    });
}

void test_token_usage_json() {
    tests::register_test("TokenUsage: JSON", []() {
        TokenUsage usage;
        usage.input = 100;
        usage.output = 50;
        usage.cache_read = 80;
        usage.cache_write = 20;
        usage.total_tokens = 150;

        std::string json_str = json::to_json(usage);
        auto parsed = json::from_json(json_str);
        // TokenUsage doesn't have from_json, just verify to_json doesn't crash
        CHECK(!json_str.empty());
    });
}

// ─── Event stream tests ───────────────────────────────────────────────────

void test_event_stream_push_consume() {
    tests::register_test("EventStream: push and consume", []() {
        EventStream<AgentEvent, std::vector<Message>> stream(
            [](const AgentEvent& ev) {
                return std::holds_alternative<AgentEndEvent>(ev);
            },
            [](const AgentEvent& ev) -> std::vector<Message> {
                if (auto* e = std::get_if<AgentEndEvent>(&ev)) {
                    return e->messages;
                }
                return {};
            });

        bool pushed = stream.push(AgentStartEvent());
        CHECK(pushed);

        auto ev = stream.next();
        CHECK(ev.has_value());
        CHECK(std::holds_alternative<AgentStartEvent>(*ev));

        AgentEndEvent end_event(std::vector<Message>{});
        pushed = stream.push_and_check(std::move(end_event));
        CHECK(!pushed); // Should return false since stream is done

        CHECK(stream.is_done());
    });
}

void test_event_stream_for_each() {
    tests::register_test("EventStream: for_each", []() {
        EventStream<AgentEvent, std::monostate> stream(
            [](const AgentEvent&) { return false; },
            [](const AgentEvent&) { return std::monostate{}; });

        int count = 0;

        std::thread t([&stream, &count]() {
            stream.push(AgentStartEvent());
            stream.push(TurnStartEvent());
            stream.finish();
        });

        stream.for_each([&count](const AgentEvent&) { count++; });
        t.join();

        CHECK(count >= 2);
    });
}

void test_event_stream_wait_result() {
    tests::register_test("EventStream: wait returns result", []() {
        std::vector<Message> test_messages;
        UserMessage msg;
        msg.timestamp = 1;
        test_messages.push_back(std::move(msg));

        EventStream<AgentEvent, std::vector<Message>> stream(
            [](const AgentEvent& ev) {
                return std::holds_alternative<AgentEndEvent>(ev);
            },
            [](const AgentEvent& ev) -> std::vector<Message> {
                if (auto* e = std::get_if<AgentEndEvent>(&ev)) {
                    return e->messages;
                }
                return {};
            });

        stream.push(AgentStartEvent());

        AgentEndEvent end_event(std::move(test_messages));
        stream.push_and_check(std::move(end_event));

        auto [result, error] = stream.wait();
        CHECK(!error.has_value());
        CHECK(result.has_value());
        CHECK_EQ(result->size(), std::size_t(1));
    });
}

void test_event_stream_error() {
    tests::register_test("EventStream: error path", []() {
        EventStream<AgentEvent, std::monostate> stream(
            [](const AgentEvent&) { return false; },
            [](const AgentEvent&) { return std::monostate{}; });

        stream.finish_error("Something went wrong");

        auto [result, error] = stream.wait();
        CHECK(error.has_value());
        CHECK_EQ(*error, "Something went wrong");
    });
}

void test_async_event_stream() {
    tests::register_test("AsyncEventStream: push and wait", []() {
        AsyncEventStream stream;

        stream.push(AgentStartEvent());
        stream.push(TurnStartEvent());
        stream.push(AgentEndEvent(std::vector<Message>{}));

        stream.wait();
        CHECK(stream.is_complete());

        auto events = stream.drain();
        CHECK_EQ(events.size(), std::size_t(3));
    });
}

// ─── AgentState tests ─────────────────────────────────────────────────────

void test_agent_state_basic() {
    tests::register_test("AgentState: basic operations", []() {
        AgentState state;

        CHECK(state.system_prompt().empty());
        CHECK(!state.is_streaming());
        CHECK(state.messages().empty());
        CHECK(state.tools().empty());

        state.set_system_prompt("You are helpful");
        CHECK_EQ(state.system_prompt(), "You are helpful");

        Model model;
        model.id = "test-model";
        model.name = "Test";
        state.set_model(model);
        CHECK_EQ(state.model().id, "test-model");
    });
}

void test_agent_state_messages() {
    tests::register_test("AgentState: message operations", []() {
        AgentState state;

        UserMessage msg;
        msg.timestamp = 1;
        TextContent tc;
        tc.text = "Hello";
        msg.content.push_back(std::move(tc));

        state.append_message(std::move(msg));
        CHECK_EQ(state.messages().size(), std::size_t(1));

        AssistantMessage asm_;
        asm_.timestamp = 2;
        state.append_message(std::move(asm_));
        CHECK_EQ(state.messages().size(), std::size_t(2));
    });
}

void test_agent_state_reset() {
    tests::register_test("AgentState: reset", []() {
        AgentState state;
        state.set_system_prompt("test");
        state.append_message(UserMessage{});
        state.add_pending_tool_call("call_123");
        state.set_error_message("err");

        state.reset();

        CHECK(state.system_prompt().empty());
        CHECK(state.messages().empty());
        CHECK(state.pending_tool_calls().empty());
        CHECK(!state.error_message().has_value());
        CHECK(!state.is_streaming());
    });
}

void test_agent_state_thread_safety() {
    tests::register_test("AgentState: thread safety", []() {
        AgentState state;

        std::vector<std::thread> writers;
        for (int i = 0; i < 10; i++) {
            writers.emplace_back([&state, i]() {
                UserMessage msg;
                msg.timestamp = i;
                TextContent tc;
                tc.text = "Hello from thread " + std::to_string(i);
                msg.content.push_back(std::move(tc));
                state.append_message(std::move(msg));
            });
        }

        for (auto& t : writers) {
            t.join();
        }

        CHECK_EQ(state.messages().size(), std::size_t(10));
    });
}

void test_find_model() {
    tests::register_test("find_model: slash-heavy ID matches before splitting", []() {
        // Regression: "accounts/fireworks/models/glm-5p2" was being split on the
        // first slash, yielding provider="accounts", id="fireworks/models/glm-5p2",
        // which never matched the registry and produced an empty base_url.
        auto m = find_model("accounts/fireworks/models/glm-5p2", "fireworks");
        CHECK(m.has_value());
        CHECK_STR(m->provider, "fireworks");
        CHECK(!m->base_url.empty());
    });

    tests::register_test("find_model: simple ID with provider hint", []() {
        auto m = find_model("gpt-4o", "openai");
        CHECK(m.has_value());
        CHECK_STR(m->provider, "openai");
    });

    tests::register_test("find_model: provider/id shorthand", []() {
        auto m = find_model("openai/gpt-4o", "");
        CHECK(m.has_value());
        CHECK_STR(m->provider, "openai");
    });

    tests::register_test("find_model: unknown ID produces generic with empty base_url", []() {
        auto m = find_model("no-such-model", "");
        CHECK(m.has_value());
        CHECK(m->base_url.empty());
    });
}

void test_model_registry() {
    tests::register_test("ModelRegistry: configured resolution and merge", []() {
        ProviderConfig local;
        local.id = "local";
        local.api = "registry-faux";
        local.base_url = "http://local.test/v1";
        local.auth = ProviderAuthPolicy::none;
        local.headers["X-Provider"] = "local";
        ConfiguredModel local_model;
        local_model.id = "same";
        local_model.headers["X-Model"] = "yes";
        local.models.push_back(local_model);
        ConfiguredModel slash_model;
        slash_model.id = "accounts/company/models/coder";
        local.models.push_back(slash_model);

        ProviderConfig remote;
        remote.id = "remote";
        remote.api = "registry-faux";
        remote.base_url = "http://remote.test/v1";
        remote.auth = ProviderAuthPolicy::none;
        ConfiguredModel remote_model;
        remote_model.id = "same";
        remote.models.push_back(remote_model);

        ModelRegistry registry({{"local", local}, {"remote", remote}});
        const auto *merged = registry.exact("LOCAL", "same");
        CHECK(merged != nullptr);
        CHECK_STR(merged->base_url, "http://local.test/v1");
        CHECK_STR(merged->headers.at("X-Provider"), "local");
        CHECK_STR(merged->headers.at("X-Model"), "yes");

        auto explicit_model = registry.resolve(
            {.provider = "local", .model = "local/same", .source = "test"});
        CHECK(explicit_model);
        CHECK_STR(explicit_model.model->id, "same");

        auto ambiguous = registry.resolve({.model = "same", .source = "test"});
        CHECK(!ambiguous);
        CHECK(ambiguous.error.find("local/same") != std::string::npos);
        CHECK(ambiguous.error.find("remote/same") != std::string::npos);

        auto slash = registry.resolve(
            {.provider = "local",
             .model = "accounts/company/models/coder",
             .source = "test"});
        CHECK(slash);
        CHECK_STR(slash.model->id, "accounts/company/models/coder");

        auto unknown_provider = registry.resolve(
            {.provider = "missing", .model = "model", .source = "test"});
        CHECK(!unknown_provider);
        auto custom = registry.resolve({.provider = "missing",
                                        .model = "model",
                                        .base_url = "http://missing.test/v1",
                                        .source = "test"});
        CHECK(custom);
        CHECK_STR(custom.model->provider, "missing");
        CHECK_STR(custom.model->base_url, "http://missing.test/v1");

        ProviderConfig override;
        override.id = "openai";
        ConfiguredModel sparse;
        sparse.context_window = 999;
        override.model_overrides.emplace("gpt-4o", sparse);
        ModelRegistry overridden({{"openai", override}});
        const auto *gpt = overridden.exact("openai", "gpt-4o");
        CHECK(gpt != nullptr);
        CHECK_EQ(gpt->context_window, 999ULL);
        CHECK_EQ(gpt->max_tokens, 16384ULL);
        CHECK(!gpt->base_url.empty());
    });

    tests::register_test("Thinking resolution: clamps unsupported levels", []() {
        Model model;
        model.provider = "local";
        model.id = "reasoning";
        model.reasoning = true;
        model.thinking_level_map = {{"off", std::nullopt},
                                    {"low", std::string("low")},
                                    {"high", std::string("high")}};
        const auto result = resolve_thinking_level(model, ThinkingLevel::medium);
        CHECK_EQ(result.level, ThinkingLevel::low);
        CHECK(result.warning.has_value());

        model.reasoning = false;
        model.thinking_level_map.clear();
        const auto off = resolve_thinking_level(model, ThinkingLevel::high);
        CHECK_EQ(off.level, ThinkingLevel::off);
        CHECK(off.warning.has_value());
    });
}

void test_model_json() {
    tests::register_test("Model: JSON serialization", []() {
        Model model;
        model.id = "gpt-4";
        model.name = "GPT-4";
        model.api = "openai-completions";
        model.provider = "openai";
        model.base_url = "https://api.openai.com/v1";
        model.context_window = 128000;
        model.max_tokens = 4096;

        std::string json_str = json::to_json(model);
        CHECK(!json_str.empty());
        CHECK(json_str.find("\"id\"") != std::string::npos);
        CHECK(json_str.find("\"gpt-4\"") != std::string::npos);
    });
}

void test_event_json() {
    tests::register_test("AgentEvent: canonical JSON envelope", []() {
        AgentStartEvent event;
        event.sequence = 42;
        auto value = event_to_json(event);

        CHECK_STR(value.value("type", ""), "event");
        CHECK_STR(value.value("event", ""), "agent_start");
        CHECK_EQ(value.value("sequence", 0ULL), 42ULL);
        CHECK(value.contains("timestamp"));
        CHECK(value.contains("data"));
    });

    tests::register_test("MessageStartEvent: request provenance JSON", []() {
      UserMessage user;
      user.content.emplace_back(TextContent{.text = "hello"});
      auto ordinary = event_to_json(MessageStartEvent{Message{user}});
      CHECK(!ordinary["data"].contains("request"));

      AssistantMessage assistant;
      assistant.content.emplace_back(TextContent{.text = "answer"});
      const auto assistant_json =
          event_to_json(MessageStartEvent{Message{std::move(assistant)}});
      CHECK(!assistant_json["data"].contains("request"));

      RequestPresentation presentation{.source = RequestSource::mailbox,
                                       .message_id = "msg-1",
                                       .message_kind = "request",
                                       .sender_agent_id = "agent-1",
                                       .sender_session_id = "session-1",
                                       .sender_task_path = "/task",
                                       .sender_session_name = "session-name"};
      auto mailbox = event_to_json(
          MessageStartEvent{Message{std::move(user)}, presentation});
      const auto &request = mailbox["data"]["request"];
      CHECK_STR(request.value("source", ""), "mailbox");
      CHECK_STR(request.value("message_id", ""), "msg-1");
      CHECK_STR(request.value("message_kind", ""), "request");
      CHECK_STR(request.value("sender_agent_id", ""), "agent-1");
      CHECK_STR(request.value("sender_session_id", ""), "session-1");
      CHECK_STR(request.value("sender_task_path", ""), "/task");
      CHECK_STR(request.value("sender_session_name", ""), "session-name");
    });

    tests::register_test("ToolEvent: structured status JSON", []() {
        ToolExecutionEndEvent event("call-1", "bash", nullptr, true,
                                    ToolExecutionStatus::blocked);
        auto value = event_to_json(event);
        CHECK_STR(value["data"].value("status", ""), "blocked");
        CHECK(value["data"].value("is_error", false));
    });

    tests::register_test("ToolPresentationEvent: mailbox receipt JSON", []() {
        MailboxReplyQueuedNotice notice{
            .request_message_id = "request-1",
            .recipient_session_id = "session-b",
            .recipient_agent_id = "agent-b",
            .reply_text = "queued text"};
        ToolPresentationEvent event("call-7", notice);
        const auto value = event_to_json(event);
        CHECK_STR(value["event"].get<std::string>(), "tool_presentation");
        CHECK_STR(value["data"].value("kind", ""), "mailbox_reply_queued");
        CHECK_STR(value["data"].value("tool_call_id", ""), "call-7");
        CHECK_STR(value["data"].value("request_message_id", ""), "request-1");
        CHECK_STR(value["data"].value("state", ""), "queued");
        CHECK_STR(value["data"].value("recipient_agent_id", ""), "agent-b");
    });
}

void test_session_journal_replay() {
    tests::register_test("SessionStore: ordered messages and truncation replay", []() {
        const auto dir = std::filesystem::temp_directory_path() /
                         "pici-session-journal-test";
        std::filesystem::remove_all(dir);

        SessionStore store(dir);
        SessionHeader header{.id = "session-1"};
        const auto id = store.create(header);

        auto make_user = [](std::string text) {
            UserMessage message;
            message.content.emplace_back(TextContent{.text = std::move(text)});
            return Message{std::move(message)};
        };

        store.append_message(id, make_user("one"));
        store.append_message(id, make_user("two"));
        store.append_truncate(id, 1);
        store.append_message(id, make_user("three"));

        store.set_model(id, "remote", "accounts/company/models/coder");

        auto record = store.load(id);
        CHECK(record.has_value());
        CHECK_STR(record->header.provider, "remote");
        CHECK_STR(record->header.model, "accounts/company/models/coder");
        CHECK_EQ(record->messages.size(), std::size_t(2));
        CHECK_EQ(std::get<UserMessage>(record->messages[0]).content.size(),
                 std::size_t(1));
        CHECK_EQ(std::get<TextContent>(
                     std::get<UserMessage>(record->messages[0]).content[0])
                     .text,
                 "one");
        CHECK_EQ(std::get<TextContent>(
                     std::get<UserMessage>(record->messages[1]).content[0])
                     .text,
                 "three");
        const auto listed = store.list();
        CHECK_EQ(listed.size(), std::size_t(1));
        CHECK_STR(listed.front().provider, "remote");
        CHECK_STR(listed.front().model, "accounts/company/models/coder");

        std::filesystem::remove_all(dir);
    });
}

void test_session_compaction_replay() {
    tests::register_test("SessionStore: compaction record replaces transcript wholesale", []() {
        const auto dir = std::filesystem::temp_directory_path() /
                         "pici-session-compaction-test";
        std::filesystem::remove_all(dir);

        SessionStore store(dir);
        SessionHeader header{.id = "session-compact-1"};
        const auto id = store.create(header);

        store.append_message(id, make_user_message("one"));
        store.append_message(id, make_user_message("two"));
        store.append_message(id, make_user_message("three"));

        SessionCompactionRecord compaction;
        compaction.messages.push_back(make_user_message("retained summary"));
        ContextCompactionMessage opaque;
        opaque.api = "openai-codex-responses";
        opaque.provider = "openai-codex";
        opaque.model = "gpt-5.3-codex";
        opaque.encrypted_content = "opaque-bytes";
        compaction.messages.push_back(Message{opaque});
        compaction.provider = "openai-codex";
        compaction.model = "gpt-5.3-codex";
        compaction.summary = "server";
        compaction.timestamp = 100;
        store.append_compaction(id, compaction);

        store.append_message(id, make_user_message("after compaction"));

        auto record = store.load(id);
        CHECK(record.has_value());
        CHECK_EQ(record->messages.size(), std::size_t(3));
        CHECK_STR(std::get<TextContent>(
                      std::get<UserMessage>(record->messages[0]).content[0])
                      .text,
                  "retained summary");
        CHECK(std::holds_alternative<ContextCompactionMessage>(
            record->messages[1]));
        CHECK_STR(std::get<ContextCompactionMessage>(record->messages[1])
                      .encrypted_content,
                  "opaque-bytes");
        CHECK_STR(std::get<TextContent>(
                      std::get<UserMessage>(record->messages[2]).content[0])
                      .text,
                  "after compaction");
        CHECK_EQ(record->header.min_schema_version, 2);

        std::filesystem::remove_all(dir);
    });
}

void test_session_compaction_malformed_throws() {
    tests::register_test("SessionStore: malformed compaction record fails loudly", []() {
        const auto dir = std::filesystem::temp_directory_path() /
                         "pici-session-compaction-malformed-test";
        std::filesystem::remove_all(dir);

        SessionStore store(dir);
        SessionHeader header{.id = "session-compact-bad"};
        const auto id = store.create(header);
        store.append_message(id, make_user_message("one"));

        // Hand-write a malformed compaction record directly (missing
        // "messages"), bypassing append_compaction's validation, to
        // simulate a corrupted file on disk.
        {
            std::ofstream f(dir / (id + ".jsonl"), std::ios::app);
            f << "{\"type\":\"compaction\",\"minVersion\":2}\n";
        }

        bool threw = false;
        try {
            (void)store.load(id);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);

        std::filesystem::remove_all(dir);
    });
}

void test_session_compaction_future_version_throws() {
    tests::register_test("SessionStore: compaction record from a newer schema fails closed", []() {
        const auto dir = std::filesystem::temp_directory_path() /
                         "pici-session-compaction-future-test";
        std::filesystem::remove_all(dir);

        SessionStore store(dir);
        SessionHeader header{.id = "session-compact-future"};
        const auto id = store.create(header);

        {
            std::ofstream f(dir / (id + ".jsonl"), std::ios::app);
            f << "{\"type\":\"compaction\",\"minVersion\":99,\"messages\":[]}\n";
        }

        bool threw = false;
        std::string what;
        try {
            (void)store.load(id);
        } catch (const std::exception& e) {
            threw = true;
            what = e.what();
        }
        CHECK(threw);
        CHECK(what.find("99") != std::string::npos);

        std::filesystem::remove_all(dir);
    });
}

void test_session_compaction_refuses_with_children() {
    tests::register_test("SessionStore: refuses to compact a session with forked children", []() {
        const auto dir = std::filesystem::temp_directory_path() /
                         "pici-session-compaction-fork-test";
        std::filesystem::remove_all(dir);

        SessionStore store(dir);
        SessionHeader parent_header{.id = "session-parent"};
        const auto parent_id = store.create(parent_header);
        store.append_message(parent_id, make_user_message("parent msg"));

        CHECK(!store.has_children(parent_id));

        SessionHeader child_header{.id = "session-child",
                                   .parent_id = parent_id,
                                   .parent_offset = std::size_t(1)};
        const auto child_id = store.create(child_header);
        (void)child_id;

        CHECK(store.has_children(parent_id));

        SessionCompactionRecord compaction;
        compaction.messages.push_back(make_user_message("summary"));
        bool threw = false;
        try {
            store.append_compaction(parent_id, compaction);
        } catch (const std::exception&) {
            threw = true;
        }
        CHECK(threw);

        // The child itself has no children of its own, so compacting it
        // (the supported "fork, then compact the fork" flow) succeeds and
        // does not touch the parent's file.
        auto parent_before = store.load(parent_id);
        store.append_compaction(child_id, compaction);
        auto parent_after = store.load(parent_id);
        CHECK(parent_before.has_value());
        CHECK(parent_after.has_value());
        CHECK_EQ(parent_before->messages.size(), parent_after->messages.size());

        auto child_record = store.load(child_id);
        CHECK(child_record.has_value());
        CHECK_EQ(child_record->messages.size(), std::size_t(1));

        std::filesystem::remove_all(dir);
    });
}

// ─── Main ──────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== pi-cpp core tests ===\n\n";

    // Type conversion tests
    test_stop_reason_conversion();
    test_stop_reason_output();
    test_thinking_level_conversion();

    // JSON tests
    test_user_message_json();
    test_assistant_message_json();
    test_tool_result_message_json();
    test_context_compaction_message_json();
    test_transform_messages_drops_orphan_tool_result();
    test_transform_messages_keeps_paired_tool_result();
    test_transform_messages_compaction_same_model_gate();
    test_compaction_message_never_rendered();
    test_token_usage_json();
    test_find_model();
    test_model_registry();
    test_model_json();
    test_event_json();
    test_session_journal_replay();
    test_session_compaction_replay();
    test_session_compaction_malformed_throws();
    test_session_compaction_future_version_throws();
    test_session_compaction_refuses_with_children();

    // Stream tests
    test_event_stream_push_consume();
    test_event_stream_for_each();
    test_event_stream_wait_result();
    test_event_stream_error();
    test_async_event_stream();

    // AgentState tests
    test_agent_state_basic();
    test_agent_state_messages();
    test_agent_state_reset();
    test_agent_state_thread_safety();

    tests::print_summary();

    return tests::failed > 0 ? 1 : 0;
}
