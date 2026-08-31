// Tests for the per-session memory accounting module (plan:
// plans/session-memory-stats.md Phase 2) and the content-composition report
// (§Design 3, Phase 1).
//
// Two build configurations are exercised by CI:
//   default (no PI_CPP_MEMSTATS): memory_stats_available() must be false and
//     every call a safe no-op; composition math is still fully verified.
//   -DPI_CPP_MEMSTATS=ON + LD_PRELOAD=libjemalloc.so.2: the canary accepts
//     jemalloc as the global allocator and arena acquire/bind/read/release
//     round-trips work end to end.

#include "core/agent_task.h"
#include "core/memory_stats.h"
#include "core/session/session_runtime.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pi::core;

namespace {
Message user_message(std::string text) {
  UserMessage message;
  message.content.emplace_back(TextContent{.text = std::move(text)});
  return Message{std::move(message)};
}

Message tool_result_message(std::string text, std::string call_id) {
  ToolResultMessage message;
  message.tool_call_id = std::move(call_id);
  message.tool_name = "read";
  message.content.emplace_back(TextContent{.text = std::move(text)});
  return Message{std::move(message)};
}

TEST(MemoryStats, CompositionSplit) {
  // A single user message with only text: everything lands in text_bytes.
  const auto report =
      composition_report_for_messages({user_message("hello world")});
  EXPECT_TRUE(report.message_count == 1);
  EXPECT_TRUE(report.transcript_bytes > 0);
  EXPECT_TRUE(report.text_bytes + report.tool_use_bytes +
                  report.tool_result_bytes + report.other_bytes ==
              report.transcript_bytes);
  EXPECT_TRUE(report.text_bytes == report.transcript_bytes);

  // A tool result's text counts as tool_result bytes, not text bytes.
  const auto result_report = composition_report_for_messages(
      {tool_result_message(std::string(5000, 'x'), "call_1")});
  EXPECT_TRUE(result_report.tool_result_bytes ==
              result_report.transcript_bytes);
  EXPECT_TRUE(result_report.text_bytes == 0);

  // Mixed transcripts keep the split additive.
  std::vector<Message> messages;
  messages.push_back(user_message(std::string(1000, 'a')));
  AssistantMessage assistant;
  assistant.model = "test-model";
  TextContent call_text{.text = ""};
  (void)call_text;
  ToolCall call;
  call.id = "call_1";
  call.name = "read";
  call.arguments = nlohmann::json::object();
  assistant.content.emplace_back(call); // tool_use
  assistant.content.emplace_back(TextContent{.text = std::string(300, 'b')});
  messages.emplace_back(std::move(assistant));
  messages.push_back(tool_result_message(std::string(4000, 'c'), "call_1"));

  const auto mixed = composition_report_for_messages(messages);
  EXPECT_TRUE(mixed.message_count == 3);
  EXPECT_TRUE(mixed.text_bytes + mixed.tool_use_bytes +
                  mixed.tool_result_bytes + mixed.other_bytes ==
              mixed.transcript_bytes);
  EXPECT_TRUE(mixed.text_bytes > 0);
  EXPECT_TRUE(mixed.tool_use_bytes > 0);
  EXPECT_TRUE(mixed.tool_result_bytes > mixed.text_bytes);

  // Empty transcript: all zeros, no crash.
  const auto empty = composition_report_for_messages({});
  EXPECT_TRUE(empty.message_count == 0);
  EXPECT_TRUE(empty.transcript_bytes == 0);
}

TEST(MemoryStats, StubSafety) {
#if !PI_MEMSTATS_HAVE_MALLCTL
  EXPECT_TRUE(!memory_stats_available());
#else
  // Real build: availability depends on the runtime environment; both
  // outcomes are valid here, but nothing may crash either way.
  if (!memory_stats_available())
    std::cerr << "note: built with memstats but jemalloc is not the active "
                 "allocator in this run\n";
#endif

  // Every call must be a safe no-op / nullopt when unavailable.
  const auto arena = acquire_session_arena();
  if (!memory_stats_available()) {
    EXPECT_TRUE(!arena.has_value());
    EXPECT_TRUE(!current_arena().has_value());
    EXPECT_TRUE(!read_arena_stats(SessionArena{0}).has_value());
    release_session_arena(arena); // must not crash on nullopt
    unbind_current_thread();
    bind_current_thread(SessionArena{7}); // unknown index: ignored
    const auto snapshot = read_process_snapshot();
    EXPECT_TRUE(snapshot.rss_bytes > 0);
    EXPECT_TRUE(!snapshot.allocator_allocated_bytes.has_value());
    EXPECT_TRUE(!snapshot.allocator_resident_bytes.has_value());

    // inherit_arena is a passthrough: invoking it runs the callable.
    int ran = 0;
    inherit_arena([&] { ++ran; })();
    EXPECT_TRUE(ran == 1);
  }
}

#if PI_MEMSTATS_HAVE_MALLCTL
TEST(MemoryStats, ArenaRoundTrip) {
  if (!memory_stats_available()) {
    GTEST_SKIP() << "arena round-trip needs LD_PRELOAD=libjemalloc.so.2";
  }

  const auto arena = acquire_session_arena();
  EXPECT_TRUE(arena.has_value());

  // Reading stats for a fresh (empty) arena works.
  const auto fresh = read_arena_stats(*arena);
  EXPECT_TRUE(fresh.has_value());

  // Binding this thread routes allocations into the session arena.
  bind_current_thread(*arena);
  EXPECT_TRUE(current_arena().has_value());
  EXPECT_TRUE(*current_arena() == *arena);

  const auto before = read_arena_stats(*arena);
  std::vector<std::byte *> keep;
  for (int i = 0; i < 16; ++i)
    keep.push_back(static_cast<std::byte *>(std::malloc(64 * 1024)));
  const auto after = read_arena_stats(*arena);
  EXPECT_TRUE(before.has_value() && after.has_value());
  // At least one of the ~1 MiB of allocations must be visible in the arena
  // (tcache batching can hide some transiently, hence the loose floor).
  EXPECT_TRUE(after->allocated_bytes >= before->allocated_bytes + 512 * 1024);
  for (auto *p : keep)
    std::free(p);

  // inherit_arena carries the binding onto a fresh thread. The wrap must
  // happen on the parent thread (§Design 2: "reads current_arena() at spawn
  // time, on the parent thread") — exactly how the Agent/EventStream/
  // AgentTaskManager spawn sites use it.
  std::optional<SessionArena> observed_on_thread;
  std::optional<SessionArena> observed_after_restore;
  auto wrapped = inherit_arena([&] { observed_on_thread = current_arena(); });
  std::thread worker([&] {
    wrapped();
    observed_after_restore = current_arena();
  });
  worker.join();
  // The spawned thread restored to "no explicit binding", not our arena —
  // restore semantics, not leak-across-threads.
  EXPECT_TRUE(observed_on_thread.has_value());
  EXPECT_TRUE(*observed_on_thread == *arena);
  EXPECT_TRUE(!observed_after_restore.has_value());

  // Release recycles the index; acquiring again hands out an arena that
  // reads fine (stale tail allowed, plan §Risks 4).
  unbind_current_thread();
  release_session_arena(arena);
  const auto recycled = acquire_session_arena();
  EXPECT_TRUE(recycled.has_value());
  EXPECT_TRUE(read_arena_stats(*recycled).has_value());
  release_session_arena(recycled);
}
#endif // PI_MEMSTATS_HAVE_MALLCTL

// Phase 3 wiring must be safe in EVERY build: bind_root_arena() and
// heap_reports() are no-ops / return nothing useful without jemalloc, but
// may never crash.
TEST(MemoryStats, HeapReportsSafety) {
  Model model;
  model.id = "memstats-model";
  model.api = "unused";
  model.provider = "unused";
  Agent::Options options;
  options.model = model;
  SessionRuntime root({.agent_options = options});
  AgentTaskManager manager(root, options);
  manager.bind_root_arena();
  manager.bind_root_arena(); // idempotent
  const auto heaps = manager.heap_reports();
  if (!memory_stats_available()) {
    EXPECT_TRUE(heaps.empty());
    return;
  }
  // With jemalloc active: exactly the root row, with readable stats.
  EXPECT_TRUE(heaps.size() == 1);
  EXPECT_TRUE(heaps.front().label == "/root");
  EXPECT_TRUE(heaps.front().arena.has_value());
}

#if PI_MEMSTATS_HAVE_MALLCTL
// THE Phase 3 acceptance test (plan §Phases): spawn a child agent task, run
// a tool-heavy-equivalent turn on it, and confirm the resulting allocation
// delta appears in THAT TASK'S ARENA — not the root's, not shared. This is
// the test that catches the original design's flaw (binding only fixed
// threads would have dumped nearly everything into shared).
TEST(MemoryStats, ChildTaskArenaAttribution) {
  if (!memory_stats_available()) {
    GTEST_SKIP() << "arena-attribution needs jemalloc as the active allocator";
  }

  constexpr std::size_t kResponseBytes = 2u << 20; // 2 MiB of live text

  // Serves the child's turn by allocating several MiB ON THE TURN THREAD —
  // whichever arena that thread is bound to gets charged.
  class GrowingClient : public LLMClient {
  public:
    std::shared_ptr<AssistantMessage>
    stream(const Model &model, const AgentContext &, const StreamOptions &,
           AssistantEventCallback, std::stop_token) override {
      // Burn the bytes then fold them into the response so a chunk survives
      // in the child's transcript after the turn ends.
      const std::string text(kResponseBytes, 'x');
      auto message = std::make_shared<AssistantMessage>();
      message->api = model.api;
      message->provider = model.provider;
      message->model = model.id;
      message->stop_reason = StopReason::stop;
      message->content.emplace_back(TextContent{.text = text});
      return message;
    }
    std::string_view provider_name() const override { return "growing-test"; }
    std::string_view api_id() const override { return "growing-test"; }
  };

  LLMClientRegistry::instance().register_client(
      "growing-test", [] { return std::make_shared<GrowingClient>(); });

  Model model;
  model.id = "growing-model";
  model.api = "growing-test";
  model.provider = "growing-test";
  Agent::Options options;
  options.model = model;
  SessionRuntime root({.agent_options = options});
  AgentTaskManager manager(root, options);
  manager.bind_root_arena(); // same call main.cpp makes before the REPL loop

  const auto before = manager.heap_reports();
  const auto root_before_it = std::ranges::find(before, std::string{"/root"},
                                                &SessionHeapReport::label);
  EXPECT_TRUE(root_before_it != before.end());
  const auto root_before =
      root_before_it->arena ? root_before_it->arena->allocated_bytes : 0;

  const auto spawned =
      manager.spawn({.task_name = "grow", .prompt = "grow the child arena"});

  AgentTaskSnapshot done;
  for (int i = 0; i < 200; ++i) {
    auto snap = manager.get(spawned.id);
    if (snap && (snap->status == AgentTaskStatusKind::completed ||
                 snap->status == AgentTaskStatusKind::errored ||
                 snap->status == AgentTaskStatusKind::interrupted)) {
      done = *snap;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_TRUE(done.status == AgentTaskStatusKind::completed);

  const auto after = manager.heap_reports();
  const auto child_it =
      std::ranges::find(after, spawned.task_path, &SessionHeapReport::label);
  const auto root_after_it =
      std::ranges::find(after, std::string{"/root"}, &SessionHeapReport::label);
  EXPECT_TRUE(child_it != after.end());
  EXPECT_TRUE(root_after_it != after.end());
  EXPECT_TRUE(child_it->arena.has_value());
  const auto child_after = child_it->arena->allocated_bytes;
  const auto root_after = root_after_it->arena
                              ? root_after_it->arena->allocated_bytes
                              : root_before;
  // The child's arena absorbed the multi-MiB turn...
  EXPECT_TRUE(child_after >= kResponseBytes / 2);
  // ...while the root stayed roughly flat (bookkeeping noise only).
  EXPECT_TRUE(root_after - root_before < kResponseBytes / 4);

  // Close joins the runner, purges, and recycles the arena: the task leaves
  // heap_reports and the release path is crash-free.
  static_cast<void>(manager.close(spawned.id));
  const auto post_close = manager.heap_reports();
  EXPECT_TRUE(std::ranges::find(post_close, spawned.task_path,
                                &SessionHeapReport::label) == post_close.end());
  EXPECT_TRUE(std::ranges::find(post_close, std::string{"/root"},
                                &SessionHeapReport::label) != post_close.end());
}
#endif // PI_MEMSTATS_HAVE_MALLCTL

} // namespace
