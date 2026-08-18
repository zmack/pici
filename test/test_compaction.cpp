#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/agent.h"
#include "core/agent_state.h"
#include "core/compaction.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/providers/faux.h"
#include "core/providers/transform_messages.h"
#include "core/session/agent_session.h"
#include "core/session/session_id.h"
#include "core/session/session_store.h"

using namespace pi::core;

namespace {

UserMessage make_user_message(std::string text) {
    UserMessage msg;
    msg.timestamp = 1;
    msg.content.push_back(TextContent{.text = std::move(text)});
    return msg;
}

AssistantMessage make_assistant_message(std::string text, std::string api = "test",
                                        std::string provider = "test",
                                        std::string model = "test-model") {
    AssistantMessage msg;
    msg.api = std::move(api);
    msg.provider = std::move(provider);
    msg.model = std::move(model);
    msg.stop_reason = StopReason::stop;
    msg.timestamp = 1;
    msg.content.push_back(TextContent{.text = std::move(text)});
    return msg;
}

Model make_faux_model() {
    Model m;
    m.id = "faux-model";
    m.name = "Faux";
    m.api = "compaction-test-faux";
    m.provider = "faux";
    return m;
}

// An LLMClient whose compact() blocks until release() is called, giving a
// test deterministic control over a window during which the compaction's
// network call is "in flight" — needed to test a Ctrl-C landing mid-flight
// without a timing-dependent race. stream() delegates to an internal
// FauxClient so the same instance also serves the prompt that follows.
// stop_tok is deliberately ignored (mirrors FauxClient, which also does not
// consult it for compact()): the point of this client is controlling
// *when* compact() returns, not reacting to cancellation itself.
class BlockingCompactionClient : public LLMClient {
public:
    BlockingCompactionClient(FauxClient::Script stream_script,
                             CompactionResult result)
        : stream_client_(std::vector<FauxClient::Script>{std::move(stream_script)}),
          result_(std::move(result)) {}

    std::shared_ptr<AssistantMessage>
    stream(const Model &model, const AgentContext &context,
          const StreamOptions &options, AssistantEventCallback on_event,
          std::stop_token stop_tok) override {
        return stream_client_.stream(model, context, options,
                                     std::move(on_event), stop_tok);
    }

    CompactionResult compact(const Model &, const AgentContext &,
                             const CompactionOptions &,
                             std::stop_token) override {
        {
            std::scoped_lock lock(mutex_);
            started_ = true;
        }
        cv_.notify_all();
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return release_; });
        return result_;
    }

    // Blocks until compact() has been entered (and is now waiting on
    // release()).
    void wait_until_started() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return started_; });
    }

    void release() {
        {
            std::scoped_lock lock(mutex_);
            release_ = true;
        }
        cv_.notify_all();
    }

    std::string_view provider_name() const override { return "faux"; }
    std::string_view api_id() const override { return "faux"; }

private:
    FauxClient stream_client_;
    CompactionResult result_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool started_{false};
    bool release_{false};
};

} // namespace

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
        std::cerr << "  (in test: " << name << ")\n";
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

void test_supports_remote_compaction() {
    tests::register_test("supports_remote_compaction: only openai-codex-responses", []() {
        Model codex;
        codex.api = "openai-codex-responses";
        CHECK(supports_remote_compaction(codex));

        Model completions;
        completions.api = "openai-completions";
        CHECK(!supports_remote_compaction(completions));

        Model faux;
        faux.api = "faux";
        CHECK(!supports_remote_compaction(faux));
    });
}

void test_filter_compacted_history_valid() {
    tests::register_test("filter_compacted_history: retained user/assistant text is valid", []() {
        std::vector<Message> messages{Message{make_user_message("hi")},
                                      Message{make_assistant_message("hello")}};
        auto result = filter_compacted_history(messages);
        CHECK(result.ok);
    });

    tests::register_test("filter_compacted_history: a bare compaction item is valid", []() {
        ContextCompactionMessage opaque;
        opaque.api = "openai-codex-responses";
        opaque.provider = "openai-codex";
        opaque.model = "gpt-5.3-codex";
        opaque.encrypted_content = "opaque-bytes";
        std::vector<Message> messages{Message{opaque}};
        auto result = filter_compacted_history(messages);
        CHECK(result.ok);
    });
}

void test_filter_compacted_history_rejects_empty() {
    tests::register_test("filter_compacted_history: empty result is rejected", []() {
        std::vector<Message> messages;
        auto result = filter_compacted_history(messages);
        CHECK(!result.ok);
        CHECK(!result.error.empty());
    });
}

void test_filter_compacted_history_rejects_tool_result() {
    tests::register_test("filter_compacted_history: retained ToolResultMessage is rejected", []() {
        ToolResultMessage trm;
        trm.tool_call_id = "call-1";
        trm.tool_name = "echo";
        trm.content.push_back(TextContent{.text = "result"});
        std::vector<Message> messages{Message{make_user_message("hi")}, Message{trm}};
        auto result = filter_compacted_history(messages);
        CHECK(!result.ok);
    });
}

void test_filter_compacted_history_rejects_tool_call_block() {
    tests::register_test("filter_compacted_history: retained assistant ToolCall block is rejected", []() {
        auto assistant = make_assistant_message("thinking about it");
        ToolCall tc;
        tc.id = "call-1";
        tc.name = "echo";
        assistant.content.push_back(tc);
        std::vector<Message> messages{Message{assistant}};
        auto result = filter_compacted_history(messages);
        CHECK(!result.ok);
    });
}

void test_filter_compacted_history_orphan_invariant_via_transform_messages() {
    tests::register_test(
        "filter_compacted_history + transform_messages: no orphaned ToolResultMessage survives",
        []() {
            // Simulates the state after compaction dropped the assistant
            // message that owned this tool call, leaving its result
            // dangling in the transcript that gets sent to the provider.
            Model model;
            model.api = "test";
            model.provider = "test";
            model.id = "test-model";

            ToolResultMessage orphaned;
            orphaned.tool_call_id = "dropped-call";
            orphaned.tool_name = "echo";
            orphaned.content.push_back(TextContent{.text = "result"});

            std::vector<Message> transcript{Message{make_user_message("hi")},
                                            Message{orphaned},
                                            Message{make_assistant_message("ok")}};

            auto sent = transform_messages(transcript, model);
            for (const auto &msg : sent) {
                if (const auto *trm = std::get_if<ToolResultMessage>(&msg)) {
                    bool has_matching_call = false;
                    for (const auto &other : sent) {
                        if (const auto *am = std::get_if<AssistantMessage>(&other)) {
                            for (const auto &block : am->content) {
                                if (const auto *tc = std::get_if<ToolCall>(&block))
                                    if (tc->id == trm->tool_call_id)
                                        has_matching_call = true;
                            }
                        }
                    }
                    CHECK(has_matching_call);
                }
            }
        });
}

void test_run_compaction_retries_transient_failure() {
    tests::register_test("run_compaction: retries a 503 then succeeds", []() {
        CompactionResult failure;
        failure.error_message = "server error";
        failure.http_status = 503;

        CompactionResult success;
        success.messages.push_back(Message{make_user_message("summary")});

        auto client = std::make_shared<FauxClient>(std::vector<FauxClient::Script>{},
                                                    std::vector<CompactionResult>{failure, success});

        CompactionRunRequest request;
        request.context.model = make_faux_model();
        request.llm_client = client;
        request.max_retries = 2;
        request.max_retry_delay_ms = 20;

        std::vector<AgentEvent> events;
        auto outcome = run_compaction(
            request,
            [&events](AgentEvent ev) {
                events.push_back(ev);
                return true;
            },
            std::stop_token{});

        CHECK(outcome.success);
        CHECK_EQ(events.size(), std::size_t(2));
        CHECK(std::holds_alternative<CompactionEvent>(events.back()));
        CHECK(std::get<CompactionEvent>(events.back()).kind == CompactionEventKind::complete);
    });
}

void test_run_compaction_does_not_retry_client_error() {
    tests::register_test("run_compaction: does not retry a 400", []() {
        CompactionResult failure;
        failure.error_message = "bad request";
        failure.http_status = 400;

        // A second, successful result is queued. If the retry policy
        // incorrectly retried the 400, this would be consumed and the
        // outcome would report success.
        CompactionResult success;
        success.messages.push_back(Message{make_user_message("summary")});

        auto client = std::make_shared<FauxClient>(std::vector<FauxClient::Script>{},
                                                    std::vector<CompactionResult>{failure, success});

        CompactionRunRequest request;
        request.context.model = make_faux_model();
        request.llm_client = client;
        request.max_retries = 3;
        request.max_retry_delay_ms = 20;

        auto outcome = run_compaction(
            request, [](AgentEvent) { return true; }, std::stop_token{});

        CHECK(!outcome.success);
        CHECK(outcome.error.has_value());
    });
}

void test_compact_rejects_while_streaming() {
    tests::register_test("Agent::compact rejects while streaming", []() {
        Agent::Options opts;
        opts.model = make_faux_model();
        Agent agent(opts);

        agent.state().set_streaming(true);
        bool threw = false;
        try {
            auto stream = agent.compact();
            (void)stream;
        } catch (const std::runtime_error &) {
            threw = true;
        }
        CHECK(threw);
        agent.state().set_streaming(false);
    });
}

void test_compact_rejects_with_pending_tool_calls() {
    tests::register_test("Agent::compact rejects while a tool call is pending", []() {
        Agent::Options opts;
        opts.model = make_faux_model();
        Agent agent(opts);

        agent.state().add_pending_tool_call("call-1");
        bool threw = false;
        try {
            auto stream = agent.compact();
            (void)stream;
        } catch (const std::runtime_error &) {
            threw = true;
        }
        CHECK(threw);
        agent.state().remove_pending_tool_call("call-1");
    });
}

void test_compact_rejects_with_queued_steering() {
    tests::register_test("Agent::compact rejects with a queued steering message", []() {
        Agent::Options opts;
        opts.model = make_faux_model();
        Agent agent(opts);

        agent.steer({Message{make_user_message("queued")}});
        bool threw = false;
        try {
            auto stream = agent.compact();
            (void)stream;
        } catch (const std::runtime_error &) {
            threw = true;
        }
        CHECK(threw);
        agent.clear_steering_queue();
    });
}

void test_compact_resets_stale_stop_source() {
    tests::register_test(
        "Agent::compact resets a stop_source left stopped by a prior "
        "mid-stream-aborted turn",
        []() {
            Agent::Options opts;
            opts.model = make_faux_model();
            Agent agent(opts);
            agent.state().append_message(Message{make_user_message("original")});

            // Simulate a prior prompt that was interrupted mid-stream: while
            // is_streaming() is true, interrupt() stops the shared
            // stop_source and records interrupt_reason_ (NOT
            // pending_interrupt_ — that is the idle-Ctrl-C case exercised by
            // test_compact_honors_pending_interrupt_at_start below). A
            // std::stop_source can never be un-stopped once stopped, and the
            // turn ending (set_streaming(false), as run_with_lifecycle does
            // on completion) does not replace it — only the next
            // begin_run_locked() or, after this fix, compact() does.
            agent.state().set_streaming(true);
            agent.interrupt(TurnAbortReason::user_interrupt);
            agent.state().set_streaming(false);
            CHECK(agent.state().stop_token().stop_requested());

            // compact()'s idle-transition snapshot runs synchronously on this
            // thread before the worker is launched, so a reset stop_source is
            // observable immediately after the call returns. Without the
            // reset, a subsequent compaction request built on this token
            // would be born already-cancelled even though nothing about
            // *this* compaction was ever interrupted (a real HTTP client
            // checks stop_requested() before/around the request; a faux
            // client that ignores stop_tok would not expose this, hence
            // asserting on the token directly rather than the outcome).
            auto stream = agent.compact();
            CHECK(!agent.state().stop_token().stop_requested());

            // Drain to let the worker finish so the test does not leak a
            // detached compaction attempt past this test's scope.
            stream.wait();
        });
}

void test_compact_honors_pending_interrupt_at_start() {
    tests::register_test(
        "Agent::compact honors a pending interrupt requested just before it "
        "started",
        []() {
            Agent::Options opts;
            opts.model = make_faux_model();
            Agent agent(opts);
            agent.state().append_message(Message{make_user_message("original")});

            // interrupt() called while idle (no active turn) sets
            // pending_interrupt_, the same flag begin_run_locked() applies
            // to whatever prompt starts next. compact() mirrors that exact
            // begin_run_locked() dance, so a Ctrl-C that lands in the window
            // right before a compaction starts is honored as "cancel this
            // compaction" instead of being silently dropped.
            agent.interrupt(TurnAbortReason::user_interrupt);

            // compact()'s idle-transition (which applies the pending
            // interrupt to the freshly-reset stop_source) runs synchronously
            // before the worker launches, so this is observable immediately
            // — checked on the token directly rather than the eventual
            // outcome because FauxClient::compact() (unlike a real HTTP
            // client) does not itself consult stop_tok.
            auto stream = agent.compact();
            CHECK(agent.state().stop_token().stop_requested());
            stream.wait();
        });
}

void test_compact_interrupt_does_not_poison_next_prompt() {
    tests::register_test(
        "a cancelled compaction's pending interrupt does not abort the next "
        "prompt",
        []() {
            AssistantMessage reply = make_assistant_message(
                "hello again", "compaction-interrupt-leak-test", "faux",
                "faux-model");
            FauxClient::Script script;
            script.events = {
                AssistantMessageEvent{AssistantMessageStartEvent{}},
                AssistantMessageEvent{
                    AssistantMessageDoneEvent{StopReason::stop, reply}},
            };
            CompactionResult compact_success;
            compact_success.messages.emplace_back(
                make_user_message("summary"));
            LLMClientRegistry::instance().register_client(
                "compaction-interrupt-leak-test", [script, compact_success] {
                    return std::make_shared<FauxClient>(
                        std::vector<FauxClient::Script>{script},
                        std::vector<CompactionResult>{compact_success});
                });

            Agent::Options opts;
            opts.model.id = "faux-model";
            opts.model.api = "compaction-interrupt-leak-test";
            opts.model.provider = "faux";
            Agent agent(opts);
            agent.state().append_message(Message{make_user_message("original")});

            // A Ctrl-C that arrives before compact() starts is applied to
            // *this* compaction's freshly-reset stop_source (see
            // test_compact_honors_pending_interrupt_at_start above) via
            // pending_interrupt_ — confirmed on the token directly since
            // FauxClient::compact() does not itself consult stop_tok, so it
            // reports success regardless. The real point of this test is
            // what happens *after*: without clearing pending_interrupt_ once
            // consumed, begin_run_locked() would treat it as still pending
            // and immediately abort the very next, unrelated prompt.
            agent.interrupt(TurnAbortReason::user_interrupt);
            auto compact_stream = agent.compact();
            CHECK(agent.state().stop_token().stop_requested());
            compact_stream.wait();

            auto prompt_stream = agent.prompt("hi again");
            auto [messages, prompt_error] = prompt_stream.wait();
            CHECK(!prompt_error.has_value());
            CHECK(messages.has_value());
            if (messages) {
                bool found_reply = false;
                for (const auto &msg : *messages) {
                    if (const auto *am = std::get_if<AssistantMessage>(&msg)) {
                        if (am->stop_reason == StopReason::stop &&
                            !am->content.empty()) {
                            found_reply = true;
                        }
                        CHECK(am->stop_reason != StopReason::aborted);
                    }
                }
                CHECK(found_reply);
            }
        });
}

void test_compact_mid_flight_interrupt_does_not_poison_next_prompt() {
    tests::register_test(
        "a Ctrl-C landing during compaction's in-flight network call does "
        "not abort the next prompt",
        []() {
            AssistantMessage reply = make_assistant_message(
                "hello again", "compaction-mid-flight-test", "faux",
                "faux-model");
            FauxClient::Script script;
            script.events = {
                AssistantMessageEvent{AssistantMessageStartEvent{}},
                AssistantMessageEvent{
                    AssistantMessageDoneEvent{StopReason::stop, reply}},
            };
            CompactionResult compact_success;
            compact_success.messages.emplace_back(
                make_user_message("summary"));

            auto blocking_client = std::make_shared<BlockingCompactionClient>(
                std::move(script), std::move(compact_success));
            LLMClientRegistry::instance().register_client(
                "compaction-mid-flight-test",
                [blocking_client] { return blocking_client; });

            Agent::Options opts;
            opts.model.id = "faux-model";
            opts.model.api = "compaction-mid-flight-test";
            opts.model.provider = "faux";
            Agent agent(opts);
            agent.state().append_message(Message{make_user_message("original")});

            auto compact_stream = agent.compact();
            // Deterministically wait for the worker thread to be inside
            // compact()'s network call (as opposed to
            // test_compact_interrupt_does_not_poison_next_prompt above,
            // where the interrupt lands before compact() starts and is
            // fully consumed by compact()'s own begin_run_locked()-style
            // dance before the worker ever launches).
            blocking_client->wait_until_started();
            agent.interrupt(TurnAbortReason::user_interrupt);
            blocking_client->release();
            compact_stream.wait();

            auto prompt_stream = agent.prompt("hi again");
            auto [messages, prompt_error] = prompt_stream.wait();
            CHECK(!prompt_error.has_value());
            CHECK(messages.has_value());
            if (messages) {
                bool found_reply = false;
                for (const auto &msg : *messages) {
                    if (const auto *am = std::get_if<AssistantMessage>(&msg)) {
                        if (am->stop_reason == StopReason::stop &&
                            !am->content.empty()) {
                            found_reply = true;
                        }
                        CHECK(am->stop_reason != StopReason::aborted);
                    }
                }
                CHECK(found_reply);
            }
        });
}

void test_commit_compaction_rejects_stale_snapshot() {
    tests::register_test("Agent::commit_compaction rejects a stale snapshot", []() {
        Agent::Options opts;
        opts.model = make_faux_model();
        Agent agent(opts);
        agent.state().append_message(Message{make_user_message("original")});

        const auto snapshot_epoch = agent.state().transcript_epoch();

        // Simulates a prompt that appended a message after the snapshot was
        // taken but before the compaction install runs.
        agent.state().append_message(Message{make_user_message("concurrent prompt")});

        std::vector<Message> replacement{Message{make_user_message("summary")}};
        auto commit = agent.commit_compaction(snapshot_epoch, replacement);

        CHECK(!commit.installed);
        CHECK(commit.error.has_value());
        // The concurrent prompt's message must survive untouched.
        CHECK_EQ(agent.state().messages().size(), std::size_t(2));
    });
}

void test_commit_compaction_installs_on_matching_epoch() {
    tests::register_test("Agent::commit_compaction installs when the epoch still matches", []() {
        Agent::Options opts;
        opts.model = make_faux_model();
        Agent agent(opts);
        agent.state().append_message(Message{make_user_message("original")});

        const auto snapshot_epoch = agent.state().transcript_epoch();
        std::vector<Message> replacement{Message{make_user_message("summary")}};
        auto commit = agent.commit_compaction(snapshot_epoch, replacement);

        CHECK(commit.installed);
        CHECK_EQ(agent.state().messages().size(), std::size_t(1));
    });
}

void test_commit_compaction_persist_runs_before_install_and_blocks_on_failure() {
    tests::register_test(
        "Agent::commit_compaction: a throwing persist callback installs nothing",
        []() {
            Agent::Options opts;
            opts.model = make_faux_model();
            Agent agent(opts);
            agent.state().append_message(Message{make_user_message("original")});

            const auto snapshot_epoch = agent.state().transcript_epoch();
            bool persist_called = false;
            std::vector<Message> replacement{Message{make_user_message("summary")}};

            bool threw = false;
            try {
                agent.commit_compaction(snapshot_epoch, replacement, [&] {
                    persist_called = true;
                    throw std::runtime_error("durable write failed");
                });
            } catch (const std::runtime_error &) {
                threw = true;
            }

            CHECK(threw);
            CHECK(persist_called);
            // Nothing installed: the original message survives unchanged.
            const auto messages_after = agent.state().messages();
            CHECK_EQ(messages_after.size(), std::size_t(1));
            CHECK(std::holds_alternative<UserMessage>(messages_after[0]));
        });
}

void test_compact_active_session_persists_then_installs() {
    tests::register_test(
        "AgentSession::compact_active_session writes the journal record and installs",
        []() {
            const auto session_dir = std::filesystem::temp_directory_path() /
                                     ("pici-compaction-session-" +
                                      std::to_string(
                                          std::chrono::steady_clock::now()
                                              .time_since_epoch()
                                              .count()));
            auto store = std::make_shared<SessionStore>(session_dir);

            LLMClientRegistry::instance().register_client(
                "compaction-active-session-test", [] {
                    CompactionResult success;
                    success.messages.push_back(Message{make_user_message("summary")});
                    success.messages.push_back(
                        Message{make_assistant_message("ack", "compaction-active-session-test",
                                                       "faux", "faux-model")});
                    return std::make_shared<FauxClient>(std::vector<FauxClient::Script>{},
                                                        std::vector<CompactionResult>{success});
                });

            AgentSession::Config config;
            config.agent_options.model.id = "faux-model";
            config.agent_options.model.api = "compaction-active-session-test";
            config.agent_options.model.provider = "faux";
            config.session_store = store;
            AgentSession session(std::move(config));

            SessionHeader header;
            const auto session_id = session.create_session(header);
            session.agent().state().append_message(Message{make_user_message("before compaction")});

            auto result = session.compact_active_session();
            CHECK(result.success);
            CHECK(!result.error.has_value());

            CHECK_EQ(session.agent().state().messages().size(), std::size_t(2));

            auto record = store->load(session_id);
            CHECK(record.has_value());
            if (record) {
                // The pre-compaction message must not resurrect on reload:
                // the journal record replaces the transcript wholesale.
                CHECK_EQ(record->messages.size(), std::size_t(2));
                CHECK(std::holds_alternative<UserMessage>(record->messages[0]));
            }

            store.reset();
            std::filesystem::remove_all(session_dir);
        });
}

void test_compact_active_session_durable_failure_installs_nothing() {
    tests::register_test(
        "AgentSession::compact_active_session: a durable-write failure installs nothing",
        []() {
            const auto session_dir = std::filesystem::temp_directory_path() /
                                     ("pici-compaction-fork-session-" +
                                      std::to_string(
                                          std::chrono::steady_clock::now()
                                              .time_since_epoch()
                                              .count()));
            auto store = std::make_shared<SessionStore>(session_dir);

            LLMClientRegistry::instance().register_client(
                "compaction-durable-failure-test", [] {
                    CompactionResult success;
                    success.messages.push_back(Message{make_user_message("summary")});
                    return std::make_shared<FauxClient>(std::vector<FauxClient::Script>{},
                                                        std::vector<CompactionResult>{success});
                });

            AgentSession::Config config;
            config.agent_options.model.id = "faux-model";
            config.agent_options.model.api = "compaction-durable-failure-test";
            config.agent_options.model.provider = "faux";
            config.session_store = store;
            AgentSession session(std::move(config));

            SessionHeader parent_header;
            const auto parent_id = session.create_session(parent_header);
            session.agent().state().append_message(Message{make_user_message("parent message")});

            // Fork a child so the parent has a live child on disk;
            // SessionStore::append_compaction refuses to compact a session
            // with forked children (session_store.cpp), which is the
            // durable-write failure this test exercises.
            SessionHeader child_header;
            child_header.id = generate_session_id();
            child_header.parent_id = parent_id;
            store->create(child_header);

            session.activate_session(*store->load(parent_id));
            const auto messages_before = session.agent().state().messages();

            auto result = session.compact_active_session();
            CHECK(!result.success);
            CHECK(result.error.has_value());

            // Nothing installed: the in-memory transcript is untouched.
            CHECK_EQ(session.agent().state().messages().size(), messages_before.size());

            store.reset();
            std::filesystem::remove_all(session_dir);
        });
}

int main() {
    std::cout << "=== pi-cpp compaction tests ===\n\n";

    test_supports_remote_compaction();
    test_filter_compacted_history_valid();
    test_filter_compacted_history_rejects_empty();
    test_filter_compacted_history_rejects_tool_result();
    test_filter_compacted_history_rejects_tool_call_block();
    test_filter_compacted_history_orphan_invariant_via_transform_messages();
    test_run_compaction_retries_transient_failure();
    test_run_compaction_does_not_retry_client_error();
    test_compact_rejects_while_streaming();
    test_compact_rejects_with_pending_tool_calls();
    test_compact_rejects_with_queued_steering();
    test_compact_resets_stale_stop_source();
    test_compact_honors_pending_interrupt_at_start();
    test_compact_interrupt_does_not_poison_next_prompt();
    test_compact_mid_flight_interrupt_does_not_poison_next_prompt();
    test_commit_compaction_rejects_stale_snapshot();
    test_commit_compaction_installs_on_matching_epoch();
    test_commit_compaction_persist_runs_before_install_and_blocks_on_failure();
    test_compact_active_session_persists_then_installs();
    test_compact_active_session_durable_failure_installs_nothing();

    tests::print_summary();

    return tests::failed > 0 ? 1 : 0;
}
