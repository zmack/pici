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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace pi::core;

namespace {
int failed = 0;
#define CHECK(value)                                                           \
  do {                                                                         \
    if (!(value)) {                                                            \
      ++failed;                                                                \
      std::cerr << "FAIL: " << #value << " at " << __LINE__ << "\n";           \
    }                                                                          \
  } while (false)

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

void test_composition_split() {
  // A single user message with only text: everything lands in text_bytes.
  const auto report =
      composition_report_for_messages({user_message("hello world")});
  CHECK(report.message_count == 1);
  CHECK(report.transcript_bytes > 0);
  CHECK(report.text_bytes + report.tool_use_bytes +
            report.tool_result_bytes + report.other_bytes ==
        report.transcript_bytes);
  CHECK(report.text_bytes == report.transcript_bytes);

  // A tool result's text counts as tool_result bytes, not text bytes.
  const auto result_report = composition_report_for_messages(
      {tool_result_message(std::string(5000, 'x'), "call_1")});
  CHECK(result_report.tool_result_bytes == result_report.transcript_bytes);
  CHECK(result_report.text_bytes == 0);

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
  assistant.content.emplace_back(call);       // tool_use
  assistant.content.emplace_back(TextContent{.text = std::string(300, 'b')});
  messages.emplace_back(std::move(assistant));
  messages.push_back(
      tool_result_message(std::string(4000, 'c'), "call_1"));

  const auto mixed = composition_report_for_messages(messages);
  CHECK(mixed.message_count == 3);
  CHECK(mixed.text_bytes + mixed.tool_use_bytes + mixed.tool_result_bytes +
            mixed.other_bytes ==
        mixed.transcript_bytes);
  CHECK(mixed.text_bytes > 0);
  CHECK(mixed.tool_use_bytes > 0);
  CHECK(mixed.tool_result_bytes > mixed.text_bytes);

  // Empty transcript: all zeros, no crash.
  const auto empty = composition_report_for_messages({});
  CHECK(empty.message_count == 0);
  CHECK(empty.transcript_bytes == 0);
}

void test_stub_safety() {
#if !PI_MEMSTATS_HAVE_MALLCTL
  CHECK(!memory_stats_available());
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
    CHECK(!arena.has_value());
    CHECK(!current_arena().has_value());
    CHECK(!read_arena_stats(SessionArena{0}).has_value());
    release_session_arena(arena); // must not crash on nullopt
    unbind_current_thread();
    bind_current_thread(SessionArena{7}); // unknown index: ignored
    const auto snapshot = read_process_snapshot();
    CHECK(snapshot.rss_bytes > 0);
    CHECK(!snapshot.allocator_allocated_bytes.has_value());
    CHECK(!snapshot.allocator_resident_bytes.has_value());

    // inherit_arena is a passthrough: invoking it runs the callable.
    int ran = 0;
    inherit_arena([&] { ++ran; })();
    CHECK(ran == 1);
  }
}

#if PI_MEMSTATS_HAVE_MALLCTL
void test_arena_round_trip() {
  if (!memory_stats_available()) {
    std::cerr << "skip: arena round-trip needs LD_PRELOAD=libjemalloc.so.2\n";
    return;
  }

  const auto arena = acquire_session_arena();
  CHECK(arena.has_value());

  // Reading stats for a fresh (empty) arena works.
  const auto fresh = read_arena_stats(*arena);
  CHECK(fresh.has_value());

  // Binding this thread routes allocations into the session arena.
  bind_current_thread(*arena);
  CHECK(current_arena().has_value());
  CHECK(*current_arena() == *arena);

  const auto before = read_arena_stats(*arena);
  std::vector<std::byte *> keep;
  for (int i = 0; i < 16; ++i)
    keep.push_back(static_cast<std::byte *>(std::malloc(64 * 1024)));
  const auto after = read_arena_stats(*arena);
  CHECK(before.has_value() && after.has_value());
  // At least one of the ~1 MiB of allocations must be visible in the arena
  // (tcache batching can hide some transiently, hence the loose floor).
  CHECK(after->allocated_bytes >= before->allocated_bytes + 512 * 1024);
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
  CHECK(observed_on_thread.has_value());
  CHECK(*observed_on_thread == *arena);
  CHECK(!observed_after_restore.has_value());

  // Release recycles the index; acquiring again hands out an arena that
  // reads fine (stale tail allowed, plan §Risks 4).
  unbind_current_thread();
  release_session_arena(arena);
  const auto recycled = acquire_session_arena();
  CHECK(recycled.has_value());
  CHECK(read_arena_stats(*recycled).has_value());
  release_session_arena(recycled);
}
#endif // PI_MEMSTATS_HAVE_MALLCTL

} // namespace

int main() {
  test_composition_split();
  test_stub_safety();
#if PI_MEMSTATS_HAVE_MALLCTL
  test_arena_round_trip();
#endif
  if (failed == 0) {
    std::cout << "memory_stats tests passed\n";
    return 0;
  }
  std::cout << failed << " memory_stats test(s) FAILED\n";
  return 1;
}
