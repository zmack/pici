#include <functional>
#include <iostream>
#include <source_location>
#include <string>
#include <vector>

#include "core/agent_state.h"
#include "core/message_types.h"
#include "core/providers/openai_completions.h"

using namespace pi::core;

namespace tests {

int passed{0};
int failed{0};
int total{0};
int current_failed{0};

struct TestResult {
    std::string name;
    bool ok;
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

#define CHECK(cond) \
    (::tests::CHECK_impl(static_cast<bool>(cond), true, #cond, \
                         std::source_location::current()))

#define CHECK_EQ(a, b) \
    (::tests::CHECK_impl((a) == (b), true, #a " == " #b, \
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
    results.push_back({name, current_failed == 0});
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

static Model make_model(std::string id = "gpt-4o", std::string provider = "openai") {
    Model m;
    m.id = std::move(id);
    m.provider = std::move(provider);
    m.api = "openai-completions";
    return m;
}

static AgentContext make_context() {
    AgentContext ctx;
    return ctx;
}

int main() {
    tests::register_test("build_request_json: basic request structure", []() {
        OpenAICompatibleClient client;
        auto model = make_model("gpt-4o");
        auto ctx = make_context();
        StreamOptions opts;

        auto json = client.build_request_json(model, ctx, opts);

        CHECK_EQ(json["model"].get<std::string>(), std::string("gpt-4o"));
        CHECK_EQ(json["stream"].get<bool>(), true);
        CHECK(json["messages"].is_array());
    });

    tests::register_test("build_request_json: includes system prompt", []() {
        OpenAICompatibleClient client;
        auto model = make_model();
        AgentContext ctx;
        ctx.system_prompt = "You are helpful";
        StreamOptions opts;

        auto json = client.build_request_json(model, ctx, opts);

        CHECK(json["messages"].is_array());
        CHECK(!json["messages"].empty());
        CHECK_EQ(json["messages"][0]["role"].get<std::string>(), std::string("system"));
        CHECK_EQ(json["messages"][0]["content"].get<std::string>(), std::string("You are helpful"));
    });

    tests::register_test("build_request_json: user message becomes role=user", []() {
        OpenAICompatibleClient client;
        auto model = make_model();
        AgentContext ctx;
        ctx.system_prompt = "sys";
        UserMessage um;
        um.content.push_back(TextContent{.text = "hello"});
        ctx.messages.push_back(std::move(um));
        StreamOptions opts;

        auto json = client.build_request_json(model, ctx, opts);
        auto& msgs = json["messages"];

        bool found_user = false;
        for (const auto& m : msgs) {
            if (m["role"] == "user") {
                CHECK_EQ(m["content"].get<std::string>(), std::string("hello"));
                found_user = true;
            }
        }
        CHECK(found_user);
    });

    tests::register_test("build_request_json: temperature forwarded", []() {
        OpenAICompatibleClient client;
        auto model = make_model();
        auto ctx = make_context();
        StreamOptions opts;
        opts.temperature = 0.5;

        auto json = client.build_request_json(model, ctx, opts);

        CHECK(json.contains("temperature"));
        CHECK_EQ(json["temperature"].get<double>(), 0.5);
    });

    tests::register_test("build_request_json: max_tokens forwarded with compat field", []() {
        OpenAICompatibleClient client;

        {
            auto model = make_model();
            auto ctx = make_context();
            StreamOptions opts;
            opts.max_tokens = 1024;

            auto json = client.build_request_json(model, ctx, opts);
            CHECK(json.contains("max_completion_tokens"));
            CHECK(!json.contains("max_tokens"));
            CHECK_EQ(json["max_completion_tokens"].get<int>(), 1024);
        }

        {
            Model model = make_model("some-model", "other");
            model.base_url = "https://llm.chutes.ai/v1";
            auto ctx = make_context();
            StreamOptions opts;
            opts.max_tokens = 512;

            auto json = client.build_request_json(model, ctx, opts);
            CHECK(json.contains("max_tokens"));
            CHECK(!json.contains("max_completion_tokens"));
            CHECK_EQ(json["max_tokens"].get<int>(), 512);
        }
    });

    tests::register_test("build_request_json: local llama.cpp compatibility", []() {
        OpenAICompatibleClient client;
        auto model = make_model("Qwen3.6-35B-A3B-UD-IQ4_NL.gguf", "llamacpp");
        model.base_url = "http://127.0.0.1:8080/v1";
        auto ctx = make_context();
        StreamOptions opts;
        opts.max_tokens = 128;

        auto json = client.build_request_json(model, ctx, opts);

        CHECK_EQ(json["model"].get<std::string>(),
                 std::string("Qwen3.6-35B-A3B-UD-IQ4_NL.gguf"));
        CHECK(json.contains("max_tokens"));
        CHECK(!json.contains("max_completion_tokens"));
        CHECK(!json.contains("store"));
        CHECK(!json.contains("stream_options"));
        CHECK_EQ(json["stream"].get<bool>(), true);
        CHECK(json.contains("chat_template_kwargs"));
        CHECK_EQ(json["chat_template_kwargs"]["enable_thinking"].get<bool>(), false);
    });

    tests::register_test("build_request_json: fireworks compatibility streams", []() {
        OpenAICompatibleClient client;
        auto model = make_model("accounts/fireworks/models/glm-5p2", "fireworks");
        model.base_url = "https://api.fireworks.ai/inference/v1";
        auto ctx = make_context();
        StreamOptions opts;
        opts.max_tokens = 256;

        auto json = client.build_request_json(model, ctx, opts);

        CHECK_EQ(json["stream"].get<bool>(), true);
        CHECK(json.contains("max_tokens"));
        CHECK(!json.contains("max_completion_tokens"));
        CHECK(json.contains("stream_options"));
    });

    tests::register_test("map_finish_reason: stop", []() {
        CHECK_EQ(OpenAICompatibleClient::map_finish_reason("stop"), StopReason::stop);
        CHECK_EQ(OpenAICompatibleClient::map_finish_reason("end"), StopReason::stop);
    });

    tests::register_test("map_finish_reason: tool_calls", []() {
        CHECK_EQ(OpenAICompatibleClient::map_finish_reason("tool_calls"), StopReason::tool_use);
        CHECK_EQ(OpenAICompatibleClient::map_finish_reason("function_call"), StopReason::tool_use);
    });

    tests::register_test("map_finish_reason: unknown", []() {
        CHECK_EQ(OpenAICompatibleClient::map_finish_reason("xyz"), StopReason::error);
    });

    tests::print_summary();
    return tests::failed > 0 ? 1 : 0;
}
