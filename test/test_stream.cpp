#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <mutex>
#include <source_location>
#include <string>
#include <thread>
#include <vector>

#include "core/event_types.h"
#include "core/message_types.h"
#include "core/stream.h"

using namespace pi::core;

// ─── Simple test harness ──────────────────────────────────────────────────

namespace tests {

int passed{0};
int failed{0};
int total{0};
int current_failed{0};

bool CHECK_impl(bool cond, bool expected,
                std::string_view expr,
                std::source_location loc = std::source_location::current()) {
    if (cond != expected) {
        current_failed++;
        std::cerr << "  FAIL " << loc.file_name() << ":" << loc.line()
                  << " - " << expr << "\n";
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
}

void print_summary() {
    std::cout << "\n========================================\n";
    std::cout << "  Tests: " << total << " total, "
              << passed << " passed, "
              << failed << " failed\n";
    std::cout << "========================================\n";
}

} // namespace tests

// ─── EventStream tests ────────────────────────────────────────────────────

void test_stream_blocking_next() {
    tests::register_test("EventStream: blocking next", []() {
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

        std::atomic<int> consumed{0};

        std::thread consumer([&stream, &consumed]() {
            while (auto ev = stream.next()) {
                consumed++;
                if (consumed >= 3) break;
            }
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stream.push(AgentStartEvent());
        stream.push(TurnStartEvent());
        stream.push(AgentEndEvent(std::vector<Message>{}));

        consumer.join();
        stream.wait();

        CHECK(consumed >= 3);
    });
}

void test_stream_iterator() {
    tests::register_test("EventStream: iterator range", []() {
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
        stream.push(TurnStartEvent());
        stream.push(TurnStartEvent());
        stream.push(AgentEndEvent(std::vector<Message>{}));

        int count = 0;
        for (auto& ev : stream) {
            count++;
        }

        CHECK(count >= 4);
    });
}

void test_stream_concurrent_pushes() {
    tests::register_test("EventStream: concurrent pushes", []() {
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

        std::atomic<int> pushed{0};
        std::vector<std::thread> writers;

        for (int i = 0; i < 10; i++) {
            writers.emplace_back([&stream, &pushed, i]() {
                switch (i % 3) {
                    case 0: stream.push(AgentStartEvent()); break;
                    case 1: stream.push(TurnStartEvent()); break;
                    case 2: stream.push(AgentEndEvent(std::vector<Message>{})); break;
                }
                pushed++;
            });
        }

        for (auto& t : writers) {
            t.join();
        }

        CHECK_EQ(pushed, 10);
    });
}

void test_stream_drain() {
    tests::register_test("EventStream: drain", []() {
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
        stream.push(TurnStartEvent());
        stream.push(MessageStartEvent(
            UserMessage{}, std::source_location::current()));

        auto events = stream.drain();
        CHECK_EQ(events.size(), std::size_t(3));
    });
}

void test_stream_finish_empty() {
    tests::register_test("EventStream: finish empty", []() {
        EventStream<AgentEvent, std::monostate> stream(
            [](const AgentEvent&) { return false; },
            [](const AgentEvent&) {});

        stream.finish();

        auto [result, error] = stream.wait();
        CHECK(!error.has_value());
    });
}

void test_stream_second_push_after_finish() {
    tests::register_test("EventStream: second push after finish", []() {
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
        stream.finish(std::vector<Message>{});

        bool pushed = stream.push(AgentEndEvent(std::vector<Message>{}));
        CHECK(!pushed);
    });
}

void test_async_stream_callback() {
    tests::register_test("AsyncEventStream: completion callback", []() {
        std::atomic<bool> callback_called{false};
        AsyncEventStream stream([&callback_called]() {
            callback_called = true;
        });

        stream.push(AgentStartEvent());
        stream.push(AgentEndEvent(std::vector<Message>{}));
        stream.wait();

        CHECK(callback_called);
        CHECK(stream.is_complete());
    });
}

void test_async_stream_drain() {
    tests::register_test("AsyncEventStream: drain after complete", []() {
        AsyncEventStream stream;
        stream.push(AgentStartEvent());
        stream.push(TurnStartEvent());
        stream.push(AgentEndEvent(std::vector<Message>{}));
        stream.wait();

        auto events = stream.drain();
        CHECK_EQ(events.size(), std::size_t(3));
    });
}

void test_stream_many_events() {
    tests::register_test("EventStream: many events", []() {
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

        for (int i = 0; i < 100; i++) {
            switch (i % 3) {
                case 0: stream.push(AgentStartEvent()); break;
                case 1: stream.push(TurnStartEvent()); break;
                case 2: stream.push(AgentEndEvent(std::vector<Message>{})); break;
            }
        }

        int count = 0;
        for (auto& ev : stream) {
            count++;
        }

        CHECK(count >= 100);
    });
}

// ─── Main ──────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== pi-cpp stream tests ===\n\n";

    test_stream_blocking_next();
    test_stream_iterator();
    test_stream_concurrent_pushes();
    test_stream_drain();
    test_stream_finish_empty();
    test_stream_second_push_after_finish();
    test_async_stream_callback();
    test_async_stream_drain();
    test_stream_many_events();

    tests::print_summary();

    return failed > 0 ? 1 : 0;
}
