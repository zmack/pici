#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
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
#include "core/session/session_runtime.h"
#include "core/session/session_id.h"
#include "core/session/session_store.h"
#include <gtest/gtest.h>

using namespace pi::core;

namespace {

UserMessage make_user_message(std::string text) {
  UserMessage msg;
  msg.timestamp = 1;
  msg.content.push_back(TextContent{.text = std::move(text)});
  return msg;
}

AssistantMessage make_assistant_message(std::string text,
                                        std::string api = "test",
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
      : stream_client_(
            std::vector<FauxClient::Script>{std::move(stream_script)}),
        result_(std::move(result)) {}

  std::shared_ptr<AssistantMessage> stream(const Model &model,
                                           const AgentContext &context,
                                           const StreamOptions &options,
                                           AssistantEventCallback on_event,
                                           std::stop_token stop_tok) override {
    return stream_client_.stream(model, context, options, std::move(on_event),
                                 stop_tok);
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

TEST(Compaction, Supports_remote_compaction_only_openai_codex_responses) {
  Model codex;
  codex.api = "openai-codex-responses";
  EXPECT_TRUE(supports_remote_compaction(codex));

  Model completions;
  completions.api = "openai-completions";
  EXPECT_TRUE(!supports_remote_compaction(completions));

  Model faux;
  faux.api = "faux";
  EXPECT_TRUE(!supports_remote_compaction(faux));
}

TEST(Compaction,
     Filter_compacted_history_retained_user_assistant_text_is_valid) {
  std::vector<Message> messages{Message{make_user_message("hi")},
                                Message{make_assistant_message("hello")}};
  auto result = filter_compacted_history(messages);
  EXPECT_TRUE(result.ok);
}

TEST(Compaction, Filter_compacted_history_a_bare_compaction_item_is_valid) {
  ContextCompactionMessage opaque;
  opaque.api = "openai-codex-responses";
  opaque.provider = "openai-codex";
  opaque.model = "gpt-5.3-codex";
  opaque.encrypted_content = "opaque-bytes";
  std::vector<Message> messages{Message{opaque}};
  auto result = filter_compacted_history(messages);
  EXPECT_TRUE(result.ok);
}

TEST(Compaction, Filter_compacted_history_empty_result_is_rejected) {
  std::vector<Message> messages;
  auto result = filter_compacted_history(messages);
  EXPECT_TRUE(!result.ok);
  EXPECT_TRUE(!result.error.empty());
}

TEST(Compaction,
     Filter_compacted_history_retained_ToolResultMessage_is_rejected) {
  ToolResultMessage trm;
  trm.tool_call_id = "call-1";
  trm.tool_name = "echo";
  trm.content.push_back(TextContent{.text = "result"});
  std::vector<Message> messages{Message{make_user_message("hi")}, Message{trm}};
  auto result = filter_compacted_history(messages);
  EXPECT_TRUE(!result.ok);
}

TEST(Compaction,
     Filter_compacted_history_retained_assistant_ToolCall_block_is_rejected) {
  auto assistant = make_assistant_message("thinking about it");
  ToolCall tc;
  tc.id = "call-1";
  tc.name = "echo";
  assistant.content.push_back(tc);
  std::vector<Message> messages{Message{assistant}};
  auto result = filter_compacted_history(messages);
  EXPECT_TRUE(!result.ok);
}

TEST(
    Compaction,
    Filter_compacted_history_transform_messages_no_orphaned_ToolResultMessage_survives) {
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
      EXPECT_TRUE(has_matching_call);
    }
  }
}

TEST(Compaction, Run_compaction_retries_a_503_then_succeeds) {
  CompactionResult failure;
  failure.error_message = "server error";
  failure.http_status = 503;

  CompactionResult success;
  success.messages.push_back(Message{make_user_message("summary")});

  auto client = std::make_shared<FauxClient>(
      std::vector<FauxClient::Script>{},
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

  EXPECT_TRUE(outcome.success);
  EXPECT_EQ(events.size(), std::size_t(2));
  EXPECT_TRUE(std::holds_alternative<CompactionEvent>(events.back()));
  EXPECT_TRUE(std::get<CompactionEvent>(events.back()).kind ==
              CompactionEventKind::complete);
}

TEST(Compaction, Run_compaction_does_not_retry_a_400) {
  CompactionResult failure;
  failure.error_message = "bad request";
  failure.http_status = 400;

  // A second, successful result is queued. If the retry policy
  // incorrectly retried the 400, this would be consumed and the
  // outcome would report success.
  CompactionResult success;
  success.messages.push_back(Message{make_user_message("summary")});

  auto client = std::make_shared<FauxClient>(
      std::vector<FauxClient::Script>{},
      std::vector<CompactionResult>{failure, success});

  CompactionRunRequest request;
  request.context.model = make_faux_model();
  request.llm_client = client;
  request.max_retries = 3;
  request.max_retry_delay_ms = 20;

  auto outcome = run_compaction(
      request, [](AgentEvent) { return true; }, std::stop_token{});

  EXPECT_TRUE(!outcome.success);
  EXPECT_TRUE(outcome.error.has_value());
}

TEST(Compaction, Agent_compact_rejects_while_streaming) {
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
  EXPECT_TRUE(threw);
  agent.state().set_streaming(false);
}

TEST(Compaction, Agent_compact_rejects_while_a_tool_call_is_pending) {
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
  EXPECT_TRUE(threw);
  agent.state().remove_pending_tool_call("call-1");
}

TEST(Compaction, Agent_compact_rejects_with_a_queued_steering_message) {
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
  EXPECT_TRUE(threw);
  agent.clear_steering_queue();
}

TEST(Compaction, Agent_compact_resets_a_stop_source_left_stopped_by_a_prior) {
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
  EXPECT_TRUE(agent.state().stop_token().stop_requested());

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
  EXPECT_TRUE(!agent.state().stop_token().stop_requested());

  // Drain to let the worker finish so the test does not leak a
  // detached compaction attempt past this test's scope.
  stream.wait();
}

TEST(Compaction,
     Agent_compact_honors_a_pending_interrupt_requested_just_before_it) {
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
  EXPECT_TRUE(agent.state().stop_token().stop_requested());
  stream.wait();
}

TEST(Compaction,
     A_cancelled_compaction_s_pending_interrupt_does_not_abort_the_next) {
  AssistantMessage reply = make_assistant_message(
      "hello again", "compaction-interrupt-leak-test", "faux", "faux-model");
  FauxClient::Script script;
  script.events = {
      AssistantMessageEvent{AssistantMessageStartEvent{}},
      AssistantMessageEvent{AssistantMessageDoneEvent{StopReason::stop, reply}},
  };
  CompactionResult compact_success;
  compact_success.messages.emplace_back(make_user_message("summary"));
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
  EXPECT_TRUE(agent.state().stop_token().stop_requested());
  compact_stream.wait();

  auto prompt_stream = agent.prompt("hi again");
  auto [messages, prompt_error] = prompt_stream.wait();
  EXPECT_TRUE(!prompt_error.has_value());
  EXPECT_TRUE(messages.has_value());
  if (messages) {
    bool found_reply = false;
    for (const auto &msg : *messages) {
      if (const auto *am = std::get_if<AssistantMessage>(&msg)) {
        if (am->stop_reason == StopReason::stop && !am->content.empty()) {
          found_reply = true;
        }
        EXPECT_TRUE(am->stop_reason != StopReason::aborted);
      }
    }
    EXPECT_TRUE(found_reply);
  }
}

TEST(Compaction,
     A_Ctrl_C_landing_during_compaction_s_in_flight_network_call_does) {
  AssistantMessage reply = make_assistant_message(
      "hello again", "compaction-mid-flight-test", "faux", "faux-model");
  FauxClient::Script script;
  script.events = {
      AssistantMessageEvent{AssistantMessageStartEvent{}},
      AssistantMessageEvent{AssistantMessageDoneEvent{StopReason::stop, reply}},
  };
  CompactionResult compact_success;
  compact_success.messages.emplace_back(make_user_message("summary"));

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
  EXPECT_TRUE(!prompt_error.has_value());
  EXPECT_TRUE(messages.has_value());
  if (messages) {
    bool found_reply = false;
    for (const auto &msg : *messages) {
      if (const auto *am = std::get_if<AssistantMessage>(&msg)) {
        if (am->stop_reason == StopReason::stop && !am->content.empty()) {
          found_reply = true;
        }
        EXPECT_TRUE(am->stop_reason != StopReason::aborted);
      }
    }
    EXPECT_TRUE(found_reply);
  }
}

TEST(Compaction, Agent_commit_compaction_rejects_a_stale_snapshot) {
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

  EXPECT_TRUE(!commit.installed);
  EXPECT_TRUE(commit.error.has_value());
  // The concurrent prompt's message must survive untouched.
  EXPECT_EQ(agent.state().messages().size(), std::size_t(2));
}

TEST(Compaction,
     Agent_commit_compaction_installs_when_the_epoch_still_matches) {
  Agent::Options opts;
  opts.model = make_faux_model();
  Agent agent(opts);
  agent.state().append_message(Message{make_user_message("original")});

  const auto snapshot_epoch = agent.state().transcript_epoch();
  std::vector<Message> replacement{Message{make_user_message("summary")}};
  auto commit = agent.commit_compaction(snapshot_epoch, replacement);

  EXPECT_TRUE(commit.installed);
  EXPECT_EQ(agent.state().messages().size(), std::size_t(1));
}

TEST(Compaction,
     Agent_commit_compaction_a_throwing_persist_callback_installs_nothing) {
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

  EXPECT_TRUE(threw);
  EXPECT_TRUE(persist_called);
  // Nothing installed: the original message survives unchanged.
  const auto messages_after = agent.state().messages();
  EXPECT_EQ(messages_after.size(), std::size_t(1));
  EXPECT_TRUE(std::holds_alternative<UserMessage>(messages_after[0]));
}

TEST(
    Compaction,
    SessionRuntime_compact_active_session_writes_the_journal_record_and_installs) {
  const auto session_dir =
      std::filesystem::temp_directory_path() /
      ("pici-compaction-session-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  auto store = std::make_shared<SessionStore>(session_dir);

  LLMClientRegistry::instance().register_client(
      "compaction-active-session-test", [] {
        CompactionResult success;
        success.messages.push_back(Message{make_user_message("summary")});
        success.messages.push_back(Message{make_assistant_message(
            "ack", "compaction-active-session-test", "faux", "faux-model")});
        return std::make_shared<FauxClient>(
            std::vector<FauxClient::Script>{},
            std::vector<CompactionResult>{success});
      });

  SessionRuntime::Config config;
  config.agent_options.model.id = "faux-model";
  config.agent_options.model.api = "compaction-active-session-test";
  config.agent_options.model.provider = "faux";
  config.session_store = store;
  SessionRuntime session(std::move(config));

  SessionHeader header;
  const auto session_id = session.create_session(header);
  session.agent().state().append_message(
      Message{make_user_message("before compaction")});

  auto result = session.compact_active_session();
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(!result.error.has_value());

  EXPECT_EQ(session.agent().state().messages().size(), std::size_t(2));

  auto record = store->load(session_id);
  EXPECT_TRUE(record.has_value());
  if (record) {
    // The pre-compaction message must not resurrect on reload:
    // the journal record replaces the transcript wholesale.
    EXPECT_EQ(record->messages.size(), std::size_t(2));
    EXPECT_TRUE(std::holds_alternative<UserMessage>(record->messages[0]));
  }

  store.reset();
  std::filesystem::remove_all(session_dir);
}

// --- Phase 5: automatic pre-turn flow -------------------------------------

TEST(Compaction,
     ContextBudgetPolicy_threshold_degenerate_and_unknown_window_handling) {
  AgentContext context;
  context.model.context_window = 1000;
  ContextBudgetPolicy policy{.threshold_pct = 0.8};

  EXPECT_TRUE(!exceeds_context_budget(context, policy));
  EXPECT_TRUE(!is_degenerate_oversized_context(context, policy));
  EXPECT_TRUE(!has_compactable_history(context));

  UserMessage huge;
  huge.content.push_back(TextContent{.text = std::string(5000, 'x')});
  context.messages.push_back(Message{huge});
  EXPECT_TRUE(exceeds_context_budget(context, policy));
  EXPECT_TRUE(is_degenerate_oversized_context(context, policy));
  EXPECT_TRUE(!has_compactable_history(context));

  context.messages.push_back(Message{make_assistant_message("ok")});
  EXPECT_TRUE(has_compactable_history(context));
  EXPECT_TRUE(exceeds_context_budget(context, policy));
  EXPECT_TRUE(!is_degenerate_oversized_context(context, policy));

  AgentContext error_only;
  error_only.model.context_window = 1000;
  error_only.messages.push_back(Message{huge});
  AssistantMessage errored = make_assistant_message("");
  errored.stop_reason = StopReason::error;
  errored.content.clear();
  error_only.messages.push_back(Message{errored});
  EXPECT_TRUE(!has_compactable_history(error_only));
  EXPECT_TRUE(is_degenerate_oversized_context(error_only, policy));

  AgentContext unknown_window;
  unknown_window.messages.push_back(Message{huge});
  EXPECT_TRUE(!exceeds_context_budget(unknown_window, policy));
  EXPECT_TRUE(!is_degenerate_oversized_context(unknown_window, policy));
}

TEST(Compaction, Looks_like_context_window_error_heuristic_vocabulary_match) {
  EXPECT_TRUE(looks_like_context_window_error(
      "This model's maximum context length is 128000 tokens."));
  EXPECT_TRUE(looks_like_context_window_error("context_length_exceeded"));
  EXPECT_TRUE(looks_like_context_window_error(
      "Please reduce the length of the messages."));
  EXPECT_TRUE(looks_like_context_window_error(
      "Your input is too long for this model."));
  EXPECT_TRUE(!looks_like_context_window_error("invalid api key"));
  EXPECT_TRUE(!looks_like_context_window_error("rate limit exceeded"));
  EXPECT_TRUE(!looks_like_context_window_error("internal server error"));
}

TEST(Compaction,
     SessionRuntime_automatic_compaction_triggers_once_after_a_completed) {
  const auto session_dir =
      std::filesystem::temp_directory_path() /
      ("pici-auto-compaction-session-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  auto store = std::make_shared<SessionStore>(session_dir);

  AssistantMessage reply = make_assistant_message(
      "hello", "openai-codex-responses", "openai-codex", "gpt-5.3-codex");
  FauxClient::Script script;
  script.events = {
      AssistantMessageEvent{AssistantMessageStartEvent{}},
      AssistantMessageEvent{AssistantMessageDoneEvent{StopReason::stop, reply}},
  };
  CompactionResult compact_success;
  compact_success.messages.push_back(Message{make_user_message("summary")});

  LLMClientRegistry::instance().register_client(
      "openai-codex-responses", [script, compact_success] {
        return std::make_shared<FauxClient>(
            std::vector<FauxClient::Script>{script},
            std::vector<CompactionResult>{compact_success});
      });

  SessionRuntime::Config config;
  config.agent_options.model.id = "gpt-5.3-codex";
  config.agent_options.model.api = "openai-codex-responses";
  config.agent_options.model.provider = "openai-codex";
  // A context window of 1 token guarantees the post-turn estimate
  // crosses any nonzero threshold, regardless of message content.
  config.agent_options.model.context_window = 1;
  config.session_store = store;
  config.auto_compaction = {.enabled = true, .threshold_pct = 0.85};
  SessionRuntime session(std::move(config));

  SessionHeader header;
  session.create_session(header);

  std::size_t compaction_event_count = 0;
  auto result = session.run_prompt("hi", [&](const AgentEvent &ev) {
    if (std::holds_alternative<CompactionEvent>(ev))
      compaction_event_count++;
  });

  EXPECT_TRUE(!result.error.has_value());
  // Exactly one compaction attempt: a `start` and a `complete`
  // CompactionEvent, not a loop.
  EXPECT_EQ(compaction_event_count, std::size_t(2));
  // The compacted replacement (a single retained user message)
  // replaced the original prompt+reply pair.
  EXPECT_EQ(session.agent().state().messages().size(), std::size_t(1));

  store.reset();
  std::filesystem::remove_all(session_dir);
}

TEST(Compaction,
     SessionRuntime_automatic_compaction_stays_off_unless_explicitly) {
  const auto session_dir =
      std::filesystem::temp_directory_path() /
      ("pici-auto-compaction-off-session-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  auto store = std::make_shared<SessionStore>(session_dir);

  AssistantMessage reply = make_assistant_message(
      "hello", "openai-codex-responses-off", "openai-codex", "gpt-5.3-codex");
  FauxClient::Script script;
  script.events = {
      AssistantMessageEvent{AssistantMessageStartEvent{}},
      AssistantMessageEvent{AssistantMessageDoneEvent{StopReason::stop, reply}},
  };
  LLMClientRegistry::instance().register_client(
      "openai-codex-responses-off", [script] {
        return std::make_shared<FauxClient>(
            std::vector<FauxClient::Script>{script});
      });

  SessionRuntime::Config config;
  config.agent_options.model.id = "gpt-5.3-codex";
  config.agent_options.model.api = "openai-codex-responses-off";
  config.agent_options.model.provider = "openai-codex";
  config.agent_options.model.context_window = 1;
  config.session_store = store;
  // auto_compaction left at its default: enabled == false.
  SessionRuntime session(std::move(config));

  SessionHeader header;
  session.create_session(header);

  std::size_t compaction_event_count = 0;
  auto result = session.run_prompt("hi", [&](const AgentEvent &ev) {
    if (std::holds_alternative<CompactionEvent>(ev))
      compaction_event_count++;
  });

  EXPECT_TRUE(!result.error.has_value());
  EXPECT_EQ(compaction_event_count, std::size_t(0));
  // Both the prompt and the reply remain — nothing was compacted.
  EXPECT_EQ(session.agent().state().messages().size(), std::size_t(2));

  store.reset();
  std::filesystem::remove_all(session_dir);
}

TEST(Compaction, SessionRuntime_a_context_window_error_triggers_exactly_one) {
  const auto session_dir =
      std::filesystem::temp_directory_path() /
      ("pici-cw-retry-session-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  auto store = std::make_shared<SessionStore>(session_dir);

  AssistantMessage error_msg = make_assistant_message(
      "", "openai-codex-responses-retry", "openai-codex", "gpt-5.3-codex");
  error_msg.stop_reason = StopReason::error;
  error_msg.error_message =
      "This model's maximum context length is 128000 tokens.";
  error_msg.content.clear();
  FauxClient::Script error_script;
  error_script.events = {AssistantMessageEvent{
      AssistantMessageErrorEvent{StopReason::error, error_msg}}};

  AssistantMessage retry_reply =
      make_assistant_message("ok after retry", "openai-codex-responses",
                             "openai-codex", "gpt-5.3-codex");
  FauxClient::Script success_script;
  success_script.events = {
      AssistantMessageEvent{AssistantMessageStartEvent{}},
      AssistantMessageEvent{
          AssistantMessageDoneEvent{StopReason::stop, retry_reply}},
  };

  CompactionResult compact_success;
  compact_success.messages.push_back(
      Message{make_user_message("summary of prior turn")});

  // Every LLMClient::create() call re-invokes this factory, so the
  // *same* client instance (not a fresh one) must be returned each
  // time — otherwise the retry's call_count_ would restart at 0
  // and replay the error script instead of the success script.
  // Registered under the real "openai-codex-responses" api id
  // (rather than a test-local suffix) because
  // supports_remote_compaction() checks that exact api string.
  auto client = std::make_shared<FauxClient>(
      std::vector<FauxClient::Script>{error_script, success_script},
      std::vector<CompactionResult>{compact_success});
  LLMClientRegistry::instance().register_client("openai-codex-responses",
                                                [client] { return client; });

  SessionRuntime::Config config;
  config.agent_options.model.id = "gpt-5.3-codex";
  config.agent_options.model.api = "openai-codex-responses";
  config.agent_options.model.provider = "openai-codex";
  config.session_store = store;
  config.auto_compaction = {.enabled = true, .threshold_pct = 0.85};
  SessionRuntime session(std::move(config));

  SessionHeader header;
  session.create_session(header);
  // A genuinely completed prior turn (not just a user message) is
  // required for has_compactable_history() to be true; otherwise
  // this reads as the degenerate no-prior-history case and the
  // retry is (correctly) refused instead of attempted.
  session.agent().state().append_message(
      Message{make_user_message("earlier turn")});
  session.agent().state().append_message(
      Message{make_assistant_message("earlier reply", "openai-codex-responses",
                                     "openai-codex", "gpt-5.3-codex")});

  std::size_t compaction_event_count = 0;
  auto result = session.run_prompt("please help", [&](const AgentEvent &ev) {
    if (std::holds_alternative<CompactionEvent>(ev))
      compaction_event_count++;
  });

  EXPECT_TRUE(!result.error.has_value());
  EXPECT_EQ(compaction_event_count, std::size_t(2));

  bool found_retry_reply = false;
  for (const auto &msg : session.agent().state().messages()) {
    if (const auto *am = std::get_if<AssistantMessage>(&msg)) {
      if (am->stop_reason == StopReason::stop && !am->content.empty())
        found_retry_reply = true;
      EXPECT_TRUE(am->stop_reason != StopReason::error);
    }
  }
  EXPECT_TRUE(found_retry_reply);

  store.reset();
  std::filesystem::remove_all(session_dir);
}

TEST(Compaction,
     SessionRuntime_a_second_compaction_is_not_recursively_triggered_by) {
  const auto session_dir =
      std::filesystem::temp_directory_path() /
      ("pici-cw-no-loop-session-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  auto store = std::make_shared<SessionStore>(session_dir);

  AssistantMessage error_msg = make_assistant_message(
      "", "openai-codex-responses", "openai-codex", "gpt-5.3-codex");
  error_msg.stop_reason = StopReason::error;
  error_msg.error_message = "context window exceeded";
  error_msg.content.clear();
  FauxClient::Script error_script;
  error_script.events = {AssistantMessageEvent{
      AssistantMessageErrorEvent{StopReason::error, error_msg}}};

  // The retried request also fails with a context-window error —
  // the retry must not spawn a second compaction attempt.
  AssistantMessage still_too_big = error_msg;
  FauxClient::Script still_error_script;
  still_error_script.events = {AssistantMessageEvent{
      AssistantMessageErrorEvent{StopReason::error, still_too_big}}};

  CompactionResult compact_success;
  compact_success.messages.push_back(Message{make_user_message("summary")});

  // Same client instance reused across every LLMClient::create()
  // call in this test — see the comment in the retry test above.
  auto client = std::make_shared<FauxClient>(
      std::vector<FauxClient::Script>{error_script, still_error_script},
      std::vector<CompactionResult>{compact_success});
  LLMClientRegistry::instance().register_client("openai-codex-responses",
                                                [client] { return client; });

  SessionRuntime::Config config;
  config.agent_options.model.id = "gpt-5.3-codex";
  config.agent_options.model.api = "openai-codex-responses";
  config.agent_options.model.provider = "openai-codex";
  config.session_store = store;
  config.auto_compaction = {.enabled = true, .threshold_pct = 0.85};
  SessionRuntime session(std::move(config));

  SessionHeader header;
  session.create_session(header);
  session.agent().state().append_message(
      Message{make_user_message("earlier turn")});
  session.agent().state().append_message(
      Message{make_assistant_message("earlier reply", "openai-codex-responses",
                                     "openai-codex", "gpt-5.3-codex")});

  std::size_t compaction_event_count = 0;
  auto result = session.run_prompt("please help", [&](const AgentEvent &ev) {
    if (std::holds_alternative<CompactionEvent>(ev))
      compaction_event_count++;
  });

  // Still an error (the retried request failed too), but only one
  // compaction attempt (start + complete) was made, not two.
  EXPECT_TRUE(result.error.has_value());
  EXPECT_EQ(compaction_event_count, std::size_t(2));

  store.reset();
  std::filesystem::remove_all(session_dir);
}

TEST(Compaction,
     SessionRuntime_an_oversized_first_turn_with_no_prior_history_fails) {
  AssistantMessage error_msg = make_assistant_message(
      "", "openai-codex-responses", "openai-codex", "gpt-5.3-codex");
  error_msg.stop_reason = StopReason::error;
  error_msg.error_message = "maximum context length exceeded for this request";
  error_msg.content.clear();
  FauxClient::Script error_script;
  error_script.events = {AssistantMessageEvent{
      AssistantMessageErrorEvent{StopReason::error, error_msg}}};

  // No compact result is queued: a degenerate first turn must
  // never even attempt a remote compaction call.
  LLMClientRegistry::instance().register_client(
      "openai-codex-responses", [error_script] {
        return std::make_shared<FauxClient>(
            std::vector<FauxClient::Script>{error_script});
      });

  SessionRuntime::Config config;
  config.agent_options.model.id = "gpt-5.3-codex";
  config.agent_options.model.api = "openai-codex-responses";
  config.agent_options.model.provider = "openai-codex";
  config.auto_compaction = {.enabled = true, .threshold_pct = 0.85};
  SessionRuntime session(std::move(config));

  std::size_t compaction_event_count = 0;
  auto result =
      session.run_prompt("a giant pasted file", [&](const AgentEvent &ev) {
        if (std::holds_alternative<CompactionEvent>(ev))
          compaction_event_count++;
      });

  EXPECT_TRUE(result.error.has_value());
  EXPECT_EQ(compaction_event_count, std::size_t(0));
  if (result.error) {
    EXPECT_TRUE(result.error->find("no prior") != std::string::npos);
  }
}

TEST(Compaction,
     Agent_set_model_during_an_in_flight_compaction_does_not_corrupt) {
  CompactionResult compact_success;
  compact_success.messages.push_back(Message{make_user_message("summary")});
  ContextCompactionMessage opaque;
  opaque.api = "compaction-model-switch-test";
  opaque.provider = "faux";
  opaque.model = "old-model";
  opaque.encrypted_content = "old-model-opaque-bytes";
  compact_success.messages.push_back(Message{opaque});

  auto blocking_client = std::make_shared<BlockingCompactionClient>(
      FauxClient::Script{}, compact_success);
  LLMClientRegistry::instance().register_client(
      "compaction-model-switch-test",
      [blocking_client] { return blocking_client; });

  Agent::Options opts;
  opts.model.id = "old-model";
  opts.model.api = "compaction-model-switch-test";
  opts.model.provider = "faux";
  Agent agent(opts);
  agent.state().append_message(Message{make_user_message("original")});

  auto compact_stream = agent.compact();
  blocking_client->wait_until_started();

  // set_model must not deadlock or be rejected while compaction's
  // network call is in flight: with_idle_transition only holds
  // worker_mutex_ for compact()'s brief snapshot phase, not for
  // the network call, and set_model does not consider "a
  // compaction is in flight" part of its idle check.
  Model new_model;
  new_model.id = "new-model";
  new_model.api = "some-other-api";
  new_model.provider = "other";
  auto switch_result = agent.set_model(new_model, ThinkingLevel::off);
  EXPECT_EQ(switch_result.current.id, std::string("new-model"));
  EXPECT_EQ(agent.state().model().id, std::string("new-model"));

  blocking_client->release();

  std::optional<CompactionEvent> complete_event;
  for (const auto &event : compact_stream) {
    if (const auto *ce = std::get_if<CompactionEvent>(&event))
      if (ce->kind == CompactionEventKind::complete)
        complete_event = *ce;
  }
  EXPECT_TRUE(complete_event.has_value());
  if (!complete_event)
    return;

  // set_model does not touch transcript_epoch, so the snapshot
  // taken before the switch is still valid: the install succeeds
  // rather than being rejected as stale.
  auto commit = agent.commit_compaction(complete_event->snapshot_epoch,
                                        complete_event->replacement_messages);
  EXPECT_TRUE(commit.installed);

  // The installed opaque item still carries the OLD model's
  // identity (it was never rewritten by the switch). Building a
  // request against the NEW (now-active) model must drop it
  // rather than forward provider-A bytes to provider B — this is
  // the same same-model gate transform_messages.cpp already
  // applies (see test_transform_messages_compaction_same_model_gate
  // in test_core.cpp), verified here end-to-end through an actual
  // compaction install rather than a hand-built message list.
  auto sent =
      transform_messages(agent.state().messages(), agent.state().model());
  for (const auto &msg : sent)
    EXPECT_TRUE(!std::holds_alternative<ContextCompactionMessage>(msg));
}

TEST(
    Compaction,
    SessionRuntime_compact_active_session_a_durable_write_failure_installs_nothing) {
  const auto session_dir =
      std::filesystem::temp_directory_path() /
      ("pici-compaction-fork-session-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  auto store = std::make_shared<SessionStore>(session_dir);

  LLMClientRegistry::instance().register_client(
      "compaction-durable-failure-test", [] {
        CompactionResult success;
        success.messages.push_back(Message{make_user_message("summary")});
        return std::make_shared<FauxClient>(
            std::vector<FauxClient::Script>{},
            std::vector<CompactionResult>{success});
      });

  SessionRuntime::Config config;
  config.agent_options.model.id = "faux-model";
  config.agent_options.model.api = "compaction-durable-failure-test";
  config.agent_options.model.provider = "faux";
  config.session_store = store;
  SessionRuntime session(std::move(config));

  SessionHeader parent_header;
  const auto parent_id = session.create_session(parent_header);
  session.agent().state().append_message(
      Message{make_user_message("parent message")});

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
  EXPECT_TRUE(!result.success);
  EXPECT_TRUE(result.error.has_value());

  // Nothing installed: the in-memory transcript is untouched.
  EXPECT_EQ(session.agent().state().messages().size(), messages_before.size());

  store.reset();
  std::filesystem::remove_all(session_dir);
}

// A client that never overrides compact(), so every call falls through to
// LLMClient's own default body: CompactionResult{.supported = false, ...}.
// stream() is never expected to be called by these tests; it aborts the
// process if it is, which would surface immediately as a test failure
// rather than silently returning a bogus assistant message.
class UnsupportedCompactionClient : public LLMClient {
public:
  std::shared_ptr<AssistantMessage> stream(const Model &, const AgentContext &,
                                           const StreamOptions &,
                                           AssistantEventCallback,
                                           std::stop_token) override {
    std::abort();
  }
  std::string_view provider_name() const override { return "unsupported-test"; }
  std::string_view api_id() const override {
    return "unsupported-compaction-test";
  }
};

TEST(Compaction,
     SessionRuntime_compact_active_session_an_unsupported_provider_reports) {
  const auto session_dir =
      std::filesystem::temp_directory_path() /
      ("pici-compaction-unsupported-session-" +
       std::to_string(
           std::chrono::steady_clock::now().time_since_epoch().count()));
  auto store = std::make_shared<SessionStore>(session_dir);

  LLMClientRegistry::instance().register_client(
      "compaction-unsupported-test",
      [] { return std::make_shared<UnsupportedCompactionClient>(); });

  SessionRuntime::Config config;
  config.agent_options.model.id = "unsupported-model";
  config.agent_options.model.api = "compaction-unsupported-test";
  config.agent_options.model.provider = "unsupported-test";
  config.session_store = store;
  SessionRuntime session(std::move(config));

  SessionHeader header;
  const auto session_id = session.create_session(header);
  session.agent().state().append_message(
      Message{make_user_message("only message")});
  const auto messages_before = session.agent().state().messages();
  // append_message only mutates in-memory state; nothing is
  // journaled until a real prompt/compaction drains through
  // SessionRuntime's persistence path. Compare against the actual
  // on-disk state before the attempt, not an assumption about it,
  // so this test asserts "compaction wrote nothing new" rather
  // than a specific message count that happens to depend on how
  // the fixture built the transcript.
  const auto journal_before = store->load(session_id);
  EXPECT_TRUE(journal_before.has_value());

  std::vector<AgentEvent> events;
  auto result = session.compact_active_session(
      CompactionTrigger::manual,
      [&](const AgentEvent &event) { events.push_back(event); });

  EXPECT_TRUE(!result.success);
  EXPECT_TRUE(result.unsupported);
  EXPECT_TRUE(!result.cancelled);
  EXPECT_TRUE(result.error.has_value());
  if (result.error)
    EXPECT_TRUE(result.error->find("not supported") != std::string::npos);

  // Transcript is byte-for-byte the pre-attempt transcript: no
  // partial install, no placeholder compaction item spliced in.
  EXPECT_EQ(session.agent().state().messages().size(), messages_before.size());
  EXPECT_TRUE(std::holds_alternative<UserMessage>(
      session.agent().state().messages()[0]));

  // No durable record was ever written: the session reloads to
  // exactly the same single message, and specifically never
  // observes a `complete` CompactionEvent (only `start`+`error`),
  // which is what would have driven a journal write.
  bool saw_complete = false;
  for (const auto &event : events)
    if (const auto *compaction = std::get_if<CompactionEvent>(&event))
      if (compaction->kind == CompactionEventKind::complete)
        saw_complete = true;
  EXPECT_TRUE(!saw_complete);

  auto reloaded = store->load(session_id);
  EXPECT_TRUE(reloaded.has_value());
  if (reloaded && journal_before)
    EXPECT_EQ(reloaded->messages.size(), journal_before->messages.size());

  store.reset();
  std::filesystem::remove_all(session_dir);
}
