#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <source_location>
#include <stop_token>
#include <string>
#include <vector>

#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/providers/faux.h"

using namespace pi::core;

// ─── Test harness ─────────────────────────────────────────────────────────

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

// ─── EventRecorder helper ─────────────────────────────────────────────────

struct EventRecorder {
    std::vector<AssistantMessageEvent> events;
    AssistantEventCallback callback() {
        return [this](const AssistantMessageEvent& ev) { events.push_back(ev); };
    }
};

// ─── Helpers to build test messages/models ────────────────────────────────

static Model make_model() {
    Model m;
    m.id = "faux-model";
    m.api = "faux";
    m.provider = "faux";
    return m;
}

static AgentContext make_ctx() {
    return AgentContext{};
}

static StreamOptions make_opts() {
    return StreamOptions{};
}

// ─── Tests ────────────────────────────────────────────────────────────────

int main() {
    tests::register_test(
        "FauxClient: emits start and done events for single script",
        []() {
            AssistantMessage partial;
            partial.model = "faux-model";

            AssistantMessage final_msg;
            final_msg.model = "faux-model";
            final_msg.stop_reason = StopReason::stop;

            FauxClient::Script script;
            script.events.push_back(AssistantMessageStartEvent{partial});
            script.events.push_back(AssistantMessageDoneEvent{StopReason::stop, final_msg});

            FauxClient client({script});
            EventRecorder rec;

            auto result = client.stream(make_model(), make_ctx(), make_opts(),
                                        rec.callback(), std::stop_token{});

            CHECK(rec.events.size() == 2);
            CHECK(std::get_if<AssistantMessageStartEvent>(&rec.events[0]) != nullptr);
            CHECK(std::get_if<AssistantMessageDoneEvent>(&rec.events[1]) != nullptr);
            CHECK(result != nullptr);
            CHECK(result->stop_reason == StopReason::stop);
        });

    tests::register_test(
        "FauxClient: emits text deltas in order",
        []() {
            AssistantMessage partial;
            partial.model = "faux-model";

            AssistantMessage final_msg;
            final_msg.model = "faux-model";
            final_msg.stop_reason = StopReason::stop;

            FauxClient::Script script;
            script.events.push_back(AssistantMessageStartEvent{partial});
            script.events.push_back(AssistantMessageTextStartEvent{0, partial});
            script.events.push_back(AssistantMessageTextDeltaEvent{0, "hello", partial});
            script.events.push_back(AssistantMessageTextDeltaEvent{0, " world", partial});
            script.events.push_back(AssistantMessageTextEndEvent{0, "hello world", partial});
            script.events.push_back(AssistantMessageDoneEvent{StopReason::stop, final_msg});

            FauxClient client({script});
            EventRecorder rec;

            client.stream(make_model(), make_ctx(), make_opts(),
                          rec.callback(), std::stop_token{});

            std::vector<std::string> deltas;
            for (const auto& ev : rec.events) {
                if (const auto* d = std::get_if<AssistantMessageTextDeltaEvent>(&ev)) {
                    deltas.push_back(d->delta);
                }
            }

            CHECK(deltas.size() == 2);
            CHECK(deltas[0] == "hello");
            CHECK(deltas[1] == " world");
        });

    tests::register_test(
        "FauxClient: returns error when no scripts remain",
        []() {
            FauxClient client({});
            EventRecorder rec;

            auto result = client.stream(make_model(), make_ctx(), make_opts(),
                                        rec.callback(), std::stop_token{});

            CHECK(result != nullptr);
            CHECK(result->stop_reason == StopReason::error);
            CHECK(rec.events.size() == 1);
            CHECK(std::get_if<AssistantMessageErrorEvent>(&rec.events[0]) != nullptr);
        });

    tests::register_test(
        "FauxClient: advances call_count_ across sequential calls",
        []() {
            AssistantMessage msg1;
            msg1.model = "faux-model";
            msg1.stop_reason = StopReason::stop;
            msg1.content.push_back(TextContent{"first"});

            AssistantMessage msg2;
            msg2.model = "faux-model";
            msg2.stop_reason = StopReason::stop;
            msg2.content.push_back(TextContent{"second"});

            FauxClient::Script s1, s2;
            s1.events.push_back(AssistantMessageDoneEvent{StopReason::stop, msg1});
            s2.events.push_back(AssistantMessageDoneEvent{StopReason::stop, msg2});

            FauxClient client({s1, s2});

            auto r1 = client.stream(make_model(), make_ctx(), make_opts(), {}, std::stop_token{});
            auto r2 = client.stream(make_model(), make_ctx(), make_opts(), {}, std::stop_token{});

            CHECK(r1 != nullptr && r1->stop_reason == StopReason::stop);
            CHECK(r2 != nullptr && r2->stop_reason == StopReason::stop);

            const auto* t1 = r1->content.empty() ? nullptr : std::get_if<TextContent>(&r1->content[0]);
            const auto* t2 = r2->content.empty() ? nullptr : std::get_if<TextContent>(&r2->content[0]);
            CHECK(t1 != nullptr && t1->text == "first");
            CHECK(t2 != nullptr && t2->text == "second");
        });

    tests::register_test(
        "FauxClient: respects stop_token",
        []() {
            AssistantMessage partial;
            partial.model = "faux-model";

            AssistantMessage final_msg;
            final_msg.model = "faux-model";
            final_msg.stop_reason = StopReason::stop;

            FauxClient::Script script;
            script.delay_between = std::chrono::milliseconds(1);
            for (int i = 0; i < 5; ++i) {
                script.events.push_back(AssistantMessageTextDeltaEvent{0, "x", partial});
            }
            script.events.push_back(AssistantMessageDoneEvent{StopReason::stop, final_msg});

            FauxClient client({script});

            std::stop_source src;
            std::atomic<int> count{0};

            auto cb = [&](const AssistantMessageEvent& ev) {
                count++;
                if (count >= 2) {
                    src.request_stop();
                }
            };

            client.stream(make_model(), make_ctx(), make_opts(), cb, src.get_token());

            CHECK(count < 5);
        });

    tests::print_summary();
    return tests::failed > 0 ? 1 : 0;
}
