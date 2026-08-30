#include "core/message_types.h"
#include "core/region_renderer.h"
#include "core/stream_renderer.h"
#include "core/terminal.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

int failed = 0;

void expect(bool condition, std::string_view message) {
  if (!condition) {
    ++failed;
    std::cerr << "FAIL: " << message << '\n';
  }
}

void test_ordered_transcript_blocks() {
  pi::core::RegionState state;
  state.blocks = {pi::core::RegionTextBlock{"first"},
                  pi::core::RegionTextBlock{"second"}};
  const auto frame = pi::core::build_region_frame(state, 80, 10);
  expect(frame.lines.size() >= 2, "ordered blocks produce rows");
  expect(frame.lines[0].starts_with("first"), "first block stays first");
  expect(frame.lines[1].starts_with("second"), "second block stays second");
  expect(frame.lines[0].ends_with("\033[0m"), "rows close SGR state");
}

void test_tail_anchor_and_scroll() {
  pi::core::RegionState state;
  state.blocks = {
      pi::core::RegionTextBlock{"one"}, pi::core::RegionTextBlock{"two"},
      pi::core::RegionTextBlock{"three"}, pi::core::RegionTextBlock{"four"}};
  auto frame = pi::core::build_region_frame(state, 80, 2);
  expect(frame.total_rows == 4, "frame counts transcript rows");
  expect(frame.max_scroll_rows == 2, "frame reports scroll extent");
  expect(frame.lines[0].starts_with("three"),
         "tail anchor starts at latest rows");
  state.scroll_offset_rows = 1;
  frame = pi::core::build_region_frame(state, 80, 2);
  expect(frame.lines[0].starts_with("two"),
         "scroll offset moves toward transcript head");
}

void test_thinking_is_inserted_at_the_current_turn_boundary() {
  pi::core::RegionState state;
  state.blocks = {pi::core::RegionTextBlock{"previous answer"},
                  pi::core::RegionTextBlock{"current answer"}};
  state.thinking = "current reasoning";
  state.thinking_block_index = 1;
  const auto frame = pi::core::build_region_frame(state, 80, 10);
  std::size_t previous = frame.lines.size();
  std::size_t reasoning = frame.lines.size();
  std::size_t current = frame.lines.size();
  for (std::size_t index = 0; index < frame.lines.size(); ++index) {
    if (frame.lines[index].find("previous answer") != std::string::npos)
      previous = index;
    if (frame.lines[index].find("current reasoning") != std::string::npos)
      reasoning = index;
    if (frame.lines[index].find("current answer") != std::string::npos)
      current = index;
  }
  expect(previous < reasoning, "previous transcript precedes current thinking");
  expect(reasoning < current, "current answer follows its thinking block");
}

void test_sgr_reopens_after_wrap() {
  pi::core::RegionState state;
  state.blocks = {pi::core::RegionTextBlock{"\033[31mabcdef\033[0m"}};
  const auto frame = pi::core::build_region_frame(state, 3, 4);
  expect(frame.lines.size() == 2, "colored text wraps into two physical rows");
  expect(frame.lines[0].starts_with("\033[31mabc"), "first row includes color");
  expect(frame.lines[1].starts_with("\033[31mdef"),
         "wrapped row reopens color");
  expect(frame.lines[1].ends_with("\033[0m"), "wrapped row closes color");

  state.blocks = {pi::core::RegionTextBlock{"\033[31mabc\033[mdef"}};
  const auto reset_frame = pi::core::build_region_frame(state, 3, 4);
  expect(reset_frame.lines.size() == 2, "empty SGR reset still wraps normally");
  expect(reset_frame.lines[1].starts_with("def"),
         "empty SGR reset does not leak color to continuation");

  state.blocks = {pi::core::RegionTextBlock{"\033[0;31mabcdef"}};
  const auto combined_reset_frame = pi::core::build_region_frame(state, 3, 4);
  expect(combined_reset_frame.lines[1].starts_with("\033[0;31mdef"),
         "reset-plus-color reopens color on continuation");
}

void test_word_wrap_breaks_on_spaces() {
  pi::core::RegionState state;
  state.blocks = {pi::core::RegionTextBlock{"alpha beta gamma delta"}};
  // Each word is 5, 4, 5, 5 columns wide; at width 9 a character-boundary
  // wrap would split "beta"/"gamma"/"delta" mid-word (e.g. "alpha bet" /
  // "a gamm" / ...). Word-boundary wrapping instead breaks before whatever
  // word wouldn't fit, landing one whole word per row here.
  const auto frame = pi::core::build_region_frame(state, 9, 10);
  expect(frame.lines.size() == 4, "one word per row at this width");
  expect(frame.lines[0] == "alpha\033[0m", "first word is not split mid-word");
  expect(frame.lines[1] == "beta\033[0m", "second word is not split mid-word");
  expect(frame.lines[2] == "gamma\033[0m", "third word is not split mid-word");
  expect(frame.lines[3] == "delta\033[0m", "fourth word is not split mid-word");
  // Trailing whitespace at a word-wrap break is dropped, not carried to the
  // next row -- none of the rows above start or end with a space, and none
  // is shorter than its full word plus the trailing SGR reset would suggest
  // a stray space snuck in.
  expect(std::none_of(frame.lines.begin(), frame.lines.end(),
                      [](const auto &line) {
                        return line.starts_with(' ') ||
                               line.starts_with("\033[0m ");
                      }),
         "word-wrap breaks drop whitespace rather than carrying it forward");
}

void test_word_wrap_overlong_token_hard_wraps() {
  pi::core::RegionState state;
  // A single unbroken token wider than the whole row has no word boundary
  // to backtrack to, so it must still fall back to hard character-boundary
  // wrapping (matching the composer's analogous fallback) instead of
  // overflowing the row.
  state.blocks = {pi::core::RegionTextBlock{"abcdefghijklmnop"}};
  const auto frame = pi::core::build_region_frame(state, 5, 10);
  expect(frame.lines.size() == 4,
         "a 16-column token hard-wraps into four 5-wide rows");
  expect(frame.lines[0] == "abcde\033[0m", "first hard-wrapped chunk");
  expect(frame.lines[1] == "fghij\033[0m", "second hard-wrapped chunk");
  expect(frame.lines[2] == "klmno\033[0m", "third hard-wrapped chunk");
  expect(frame.lines[3] == "p\033[0m", "final partial chunk");
}

void test_word_wrap_sgr_reopens_after_word_boundary_break() {
  pi::core::RegionState state;
  // "red " fits within width 5 and "w" still fits on that row, but "wo"
  // would overflow -- so the row breaks between "red" and "word" at the
  // word boundary (not mid-word), and the color that was active when
  // "word" started must still reopen on the continuation row.
  state.blocks = {pi::core::RegionTextBlock{"\033[31mred word\033[0m"}};
  const auto frame = pi::core::build_region_frame(state, 5, 10);
  expect(frame.lines.size() == 2,
         "colored text wraps at the word boundary into two rows");
  expect(frame.lines[0] == "\033[31mred\033[0m",
         "first row holds the whole first word with no trailing space");
  expect(frame.lines[1].starts_with("\033[31mword"),
         "word-boundary wrap reopens the color that was active at the break");
  expect(frame.lines[1].ends_with("\033[0m"), "wrapped row closes color");
}

void test_diff_only_changes_rows() {
  const std::vector<std::string> old_rows = {"one", "two", "three"};
  const std::vector<std::string> new_rows = {"one", "changed", "three"};
  const auto diff = pi::core::diff_region_rows(old_rows, new_rows);
  expect(diff.find("\033[2;1H\033[2Kchanged") != std::string::npos,
         "diff paints changed row");
  expect(diff.find("\033[1;1H") == std::string::npos,
         "diff skips unchanged first row");
  expect(diff.find("\033[3;1H") == std::string::npos,
         "diff skips unchanged last row");
}

void test_diff_scrolls_tail_without_repainting_every_row() {
  const std::vector<std::string> old_rows = {"one", "two", "three"};
  const std::vector<std::string> new_rows = {"two", "three", "four"};
  const auto diff = pi::core::diff_region_rows(old_rows, new_rows);
  expect(diff.starts_with("\033[1;1H\033[1S"),
         "tail growth scrolls the content region");
  expect(diff.find("\033[1;1H\033[2K") == std::string::npos &&
             diff.find("\033[2;1H\033[2K") == std::string::npos,
         "tail growth preserves shifted rows");
  expect(diff.find("\033[3;1H\033[2Kfour") != std::string::npos,
         "tail growth paints only the new bottom row");

  const auto reverse = pi::core::diff_region_rows(new_rows, old_rows);
  expect(reverse.starts_with("\033[1;1H\033[1T"),
         "scrolling toward history shifts the content region down");
  expect(reverse.find("\033[1;1H\033[2Kone") != std::string::npos,
         "history scrolling paints only the new top row");
}

void test_degenerate_sizing() {
  pi::core::RegionState state;
  state.blocks = {pi::core::RegionTextBlock{"text"}};
  expect(pi::core::build_region_frame(state, 80, 0).lines.empty(),
         "zero content rows produce no output");
  expect(pi::core::build_region_frame(state, 0, 2).lines.empty(),
         "zero width produces no output");
}

pi::core::RegionBlock tool_block(std::string call_id, std::string name,
                                 std::string output, bool running = false) {
  pi::core::RegionToolBlock tool;
  tool.call_id = std::move(call_id);
  tool.tool_name = std::move(name);
  tool.args_json = "{}";
  tool.raw_output = std::move(output);
  tool.running = running;
  return tool;
}

void test_tool_regions_preserve_call_order() {
  pi::core::RegionState state;
  state.blocks.emplace_back(pi::core::RegionTextBlock{"before"});
  state.blocks.emplace_back(tool_block("call-a", "first", "a-result"));
  state.blocks.emplace_back(tool_block("call-b", "second", "b-result"));
  state.blocks.emplace_back(pi::core::RegionTextBlock{"after"});
  const auto frame = pi::core::build_region_frame(state, 80, 20);
  const auto first = frame.lines[1].find("[first]");
  const auto second = frame.lines[3].find("[second]");
  expect(first != std::string::npos && second != std::string::npos,
         "tool regions stay in call order");
  expect(frame.lines[2].find("a-result") != std::string::npos,
         "first tool keeps its own output");
  expect(frame.lines[4].find("b-result") != std::string::npos,
         "second tool keeps its own output");
  expect(frame.lines.back().find("after") != std::string::npos,
         "text after tools remains after tool regions");
}

void test_tool_updates_are_isolated() {
  pi::core::RegionState state;
  state.blocks.emplace_back(tool_block("call-a", "first", "first-output"));
  state.blocks.emplace_back(tool_block("call-b", "second", "old-output"));
  auto before = pi::core::build_region_frame(state, 80, 20);
  std::get<pi::core::RegionToolBlock>(state.blocks[1]).raw_output =
      "new-output";
  auto after = pi::core::build_region_frame(state, 80, 20);
  expect(after.lines[1].find("first-output") != std::string::npos,
         "updating one tool does not change another tool");
  expect(before.lines[3] != after.lines[3] &&
             after.lines[3].find("new-output") != std::string::npos,
         "updated tool output is rendered in place");
}

void test_interleaved_text_rounds() {
  pi::core::RegionState state;
  state.blocks.emplace_back(pi::core::RegionTextBlock{"round one"});
  state.blocks.emplace_back(tool_block("call", "tool", "result"));
  state.blocks.emplace_back(pi::core::RegionTextBlock{"round two"});
  const auto frame = pi::core::build_region_frame(state, 80, 20);
  expect(frame.lines[0].find("round one") != std::string::npos,
         "first text round precedes tool");
  expect(frame.lines[3].find("round two") != std::string::npos,
         "second text round follows tool");
}

void test_tool_body_cap_and_expansion_cap() {
  pi::core::RegionState state;
  state.blocks.emplace_back(
      tool_block("call", "tool", "one\ntwo\nthree\nfour\nfive\nsix"));
  const auto body_frame = pi::core::build_region_frame(state, 80, 20);
  expect(body_frame.total_rows == 5, "tool body is capped to four rows");

  state.blocks.clear();
  for (int i = 0; i < 7; ++i) {
    state.blocks.emplace_back(tool_block("call-" + std::to_string(i),
                                         "tool-" + std::to_string(i),
                                         "body-" + std::to_string(i)));
  }
  const auto capped = pi::core::build_region_frame(state, 80, 40);
  expect(capped.lines[0].find("running") == std::string::npos &&
             capped.lines[0].find("done") != std::string::npos,
         "oldest tool collapses to a done summary over the expansion cap");
  expect(capped.lines.back().find("body-6") != std::string::npos,
         "newest tool remains expanded");
}

void test_custom_tool_output_is_rendered() {
  pi::core::RegionState state;
  pi::core::RegionToolBlock tool;
  tool.call_id = "call";
  tool.tool_name = "tool";
  tool.custom_result_output = "custom header\ncustom body";
  tool.running = false;
  state.blocks.emplace_back(std::move(tool));
  const auto frame = pi::core::build_region_frame(state, 80, 10);
  expect(frame.lines[0].find("custom header") != std::string::npos,
         "custom formatted tool output is routed to the region");
  expect(frame.lines[1].find("custom body") != std::string::npos,
         "custom formatted tool body is preserved");
}

void test_formatter_regions_cap_and_reset() {
  pi::core::RegionState state;
  pi::core::RegionToolBlock running;
  running.call_id = "running";
  running.tool_name = "theme";
  running.args_json = "{}";
  running.custom_call_output = "\033[31mCALL first\nCALL second\033[0m";
  state.blocks.emplace_back(running);
  auto frame = pi::core::build_region_frame(state, 80, 20);
  expect(frame.lines.size() == 1,
         "running formatter keeps only its first header row");
  expect(frame.lines[0].find("CALL first") != std::string::npos,
         "running formatter preserves its first header line");
  expect(frame.lines[0].find("CALL second") == std::string::npos,
         "running formatter omits later header lines");
  expect(frame.lines[0].ends_with("\033[0m"),
         "running formatter row resets SGR");

  pi::core::RegionToolBlock completed = running;
  completed.running = false;
  completed.custom_call_output.clear();
  completed.custom_result_output =
      "\033[32mone\ntwo\nthree\nfour\nfive\nsix\033[0m";
  state.blocks.clear();
  state.blocks.emplace_back(std::move(completed));
  frame = pi::core::build_region_frame(state, 80, 20);
  expect(frame.lines.size() == 5, "completed formatter result honors body cap");
  for (const auto &line : frame.lines)
    expect(line.ends_with("\033[0m"), "completed formatter rows reset SGR");
  expect(frame.lines.back().find("five") != std::string::npos,
         "completed formatter keeps the capped final visible line");
  expect(frame.lines.back().find("six") == std::string::npos,
         "completed formatter omits rows beyond the cap");
}

void test_formatter_sanitization_contract() {
  const auto output = pi::core::sanitize_tool_output(
      "\033[31mred\033[2J\033[H\033]0;title\007plain\033[0m");
  expect(output.find("red") != std::string::npos,
         "sanitizer preserves visible formatter text");
  expect(output.find("\033[31m") != std::string::npos &&
             output.find("\033[0m") != std::string::npos,
         "sanitizer preserves SGR formatting");
  expect(output.find("\033[2J") == std::string::npos &&
             output.find("\033[H") == std::string::npos &&
             output.find("\033]0;") == std::string::npos,
         "sanitizer removes cursor and OSC controls");
}

void test_tiny_layout_keeps_tool_body_collapsed() {
  pi::core::RegionState state;
  state.blocks.emplace_back(tool_block("call", "tool", "body"));
  state.blocks.emplace_back(pi::core::RegionTextBlock{"later text"});
  const auto frame = pi::core::build_region_frame(state, 80, 1);
  expect(frame.total_rows == 2, "tiny layout keeps one row per block");
  expect(frame.lines[0].find("body") == std::string::npos,
         "tiny layout omits tool body");
  expect(frame.lines[0].find("later text") != std::string::npos,
         "tiny layout preserves subsequent transcript blocks");
}

class TestToolResult final : public pi::core::ToolResult {
public:
  explicit TestToolResult(std::string content) : content_(std::move(content)) {}

  bool is_error() const override { return false; }
  std::string content() const override { return content_; }
  std::optional<std::string> details() const override { return std::nullopt; }

private:
  std::string content_;
};

void test_tool_callbacks_route_into_regions() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates callback capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    expect(renderer->owns_tool_output(),
           "region renderer owns formatted tool output");
    renderer->on_turn_start();
    renderer->on_text_delta("before");
    renderer->on_tool_start("a", "alpha", "{}");
    renderer->on_tool_start("b", "beta", "{}");
    renderer->on_tool_output_text("a", "formatted [alpha] call");
    renderer->on_tool_output_text("b", "formatted [beta] call");
    renderer->on_tool_update("b", "beta", "b-live");
    renderer->on_tool_update("a", "alpha", "a-live");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    renderer->on_tool_end("b", "beta", TestToolResult{"b-final"}, false);
    renderer->on_tool_end("a", "alpha", TestToolResult{"a-final"}, false);
    renderer->on_text_delta("after");
    renderer->on_tool_update("unknown", "ignored", "ignored");
    renderer->on_turn_end();
  }
  ::close(fds[1]);
  std::string output;
  char buffer[256];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);
  expect(output.find("[alpha]") != std::string::npos,
         "callback-created alpha region is painted");
  expect(output.find("formatted [alpha] call") != std::string::npos,
         "formatted tool call header is preserved while running");
  expect(output.find("[beta]") != std::string::npos,
         "callback-created beta region is painted");
  expect(output.find("a-final") != std::string::npos,
         "alpha completion updates its original region");
  expect(output.find("b-final") != std::string::npos,
         "beta completion updates its original region");
  expect(output.find("a-live") != std::string::npos,
         "live output remains visible after formatted call output");
  expect(output.find("after") != std::string::npos,
         "text after tools is painted after callback-created regions");
  expect(output.find("[alpha]") < output.find("[beta]") &&
             output.find("[beta]") < output.find("after"),
         "callback-created regions preserve transcript order");
}
void test_tool_call_streaming_finalizes_into_single_block() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates streaming capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    renderer->on_turn_start();
    // id/name are still empty on the first delta, then fill in; args grow
    // across deltas exactly as stream_renderer.cpp's dispatch_event feeds
    // AssistantMessageToolCallStartEvent/DeltaEvent through.
    renderer->on_tool_call_streaming(0, "", "", "{\"path\":");
    renderer->on_tool_call_streaming(0, "call-1", "read", "{\"path\":\"README");
    renderer->on_tool_call_streaming(0, "call-1", "read",
                                     "{\"path\":\"README.md\"}");
    // on_tool_start() fires once the whole call has parsed; it must finalize
    // the drafting block in place rather than appending a duplicate.
    renderer->on_tool_start("call-1", "read", "{\"path\":\"README.md\"}");
    renderer->on_tool_end("call-1", "read", TestToolResult{"file contents"},
                          false);
    renderer->on_turn_end();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  ::close(fds[1]);
  std::string output;
  char buffer[512];
  for (ssize_t count; (count = ::read(fds[0], buffer, sizeof(buffer))) > 0;)
    output.append(buffer, static_cast<std::size_t>(count));
  ::close(fds[0]);
  auto count_occurrences = [](const std::string &haystack,
                              std::string_view needle) {
    std::size_t occurrences = 0;
    for (std::size_t pos = haystack.find(needle); pos != std::string::npos;
        pos = haystack.find(needle, pos + needle.size()))
      ++occurrences;
    return occurrences;
  };
  expect(count_occurrences(output, "[read]") == 1,
         "streamed-then-started tool call paints exactly one block");
  expect(output.find("README.md") != std::string::npos,
         "finalized block shows the fully-parsed arguments");
  expect(output.find("file contents") != std::string::npos,
         "completion still attaches to the streamed-in block");
}

void test_tool_call_streaming_interleaved_calls_stay_isolated() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates interleaved streaming capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    renderer->on_turn_start();
    // Two calls streaming concurrently, correlated by content_index rather
    // than call_id (which is still empty on their first delta each).
    renderer->on_tool_call_streaming(0, "", "", "{\"path\":");
    renderer->on_tool_call_streaming(1, "", "", "{\"pattern\":");
    renderer->on_tool_call_streaming(0, "call-1", "read", "{\"path\":\"a.txt");
    renderer->on_tool_call_streaming(1, "call-2", "grep", "{\"pattern\":\"foo");
    renderer->on_tool_call_streaming(0, "call-1", "read",
                                     "{\"path\":\"a.txt\"}");
    renderer->on_tool_call_streaming(1, "call-2", "grep",
                                     "{\"pattern\":\"foo\"}");
    renderer->on_tool_start("call-1", "read", "{\"path\":\"a.txt\"}");
    renderer->on_tool_start("call-2", "grep", "{\"pattern\":\"foo\"}");
    renderer->on_tool_end("call-1", "read", TestToolResult{"file contents"},
                          false);
    renderer->on_tool_end("call-2", "grep", TestToolResult{"3 matches"},
                          false);
    renderer->on_turn_end();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  ::close(fds[1]);
  std::string output;
  char buffer[512];
  for (ssize_t count; (count = ::read(fds[0], buffer, sizeof(buffer))) > 0;)
    output.append(buffer, static_cast<std::size_t>(count));
  ::close(fds[0]);
  expect(output.find("[read]") != std::string::npos &&
             output.find("[grep]") != std::string::npos,
         "both interleaved streaming calls produce their own block");
  expect(output.find("a.txt") != std::string::npos &&
             output.find("foo") != std::string::npos,
         "neither call's arguments leak into the other's block");
  expect(output.find("file contents") != std::string::npos &&
             output.find("3 matches") != std::string::npos,
         "each call's result attaches to its own streamed-in block");
  expect(output.find("[read]") < output.find("[grep]") &&
             output.find("[grep]") < output.find("3 matches"),
         "interleaved calls preserve their content_index arrival order");
}

void test_mailbox_reply_callback_path() {
  int fds[2]{};
  if (::pipe(fds) != 0) {
    expect(false, "pipe creates mailbox callback capture fd");
    return;
  }
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    renderer->on_turn_start();
    pi::core::dispatch_event(
        pi::core::ToolPresentationEvent{
            "call-1", pi::core::MailboxReplyQueuedNotice{
                          .request_message_id = "request-1",
                          .recipient_session_id = "session-b",
                          .recipient_agent_id = "agent-b",
                          .reply_text = "first queued"}},
        *renderer);
    pi::core::dispatch_event(
        pi::core::ToolPresentationEvent{
            "call-2", pi::core::MailboxReplyQueuedNotice{
                          .request_message_id = "request-2",
                          .recipient_session_id = "session-c",
                          .reply_text = "second queued"}},
        *renderer);
    renderer->on_turn_end();
    pi::core::dispatch_event(
        pi::core::ToolPresentationEvent{
            "late", pi::core::MailboxReplyQueuedNotice{
                        .request_message_id = "late",
                        .recipient_session_id = "late",
                        .reply_text = "must be dropped"}},
        *renderer);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
  }
  ::close(fds[1]);
  std::string output;
  char buffer[512];
  for (ssize_t count; (count = ::read(fds[0], buffer, sizeof(buffer))) > 0;)
    output.append(buffer, static_cast<std::size_t>(count));
  ::close(fds[0]);
  expect(output.find("REPLY -> agent-b queued") != std::string::npos,
         "callback paints agent recipient receipt");
  expect(output.find("REPLY -> session-c queued") != std::string::npos,
         "callback falls back to session recipient");
  expect(output.find("first queued") < output.find("second queued"),
         "multiple callback receipts preserve order");
  expect(output.find("must be dropped") == std::string::npos,
         "callback without active turn is dropped");
}

void test_mailbox_reply_receipt_is_persistent_and_safe() {
  pi::core::RegionState state;
  pi::core::RegionTurn turn;
  turn.blocks.emplace_back(pi::core::RegionReplyBlock{
      .request_message_id = "request-1",
      .call_id = "reply-call",
      .recipient_label = "agent-b",
      .raw_text = "queued reply text"});
  state.turns.push_back(std::move(turn));
  const auto frame = pi::core::build_region_frame(state, 40, 10);
  expect(frame.lines.size() >= 2, "reply receipt paints persistent rows");
  expect(frame.lines[0].find("REPLY -> agent-b queued") != std::string::npos,
         "reply heading identifies recipient and queued state");
  expect(frame.lines[1].find("queued reply text") != std::string::npos,
         "reply body remains visible");
  for (const auto &row : frame.lines)
    expect(row.ends_with("\033[0m"), "reply rows close SGR");
}

void test_mailbox_reply_alongside_parallel_unrelated_tools() {
  pi::core::RegionState state;
  pi::core::RegionTurn turn;
  pi::core::RegionToolBlock preceding;
  preceding.call_id = "call-a";
  preceding.tool_name = "unrelated-a";
  preceding.args_json = "{}";
  preceding.raw_output = "a-result";
  turn.blocks.emplace_back(std::move(preceding));
  turn.blocks.emplace_back(pi::core::RegionReplyBlock{
      .request_message_id = "request-1",
      .call_id = "call-b",
      .recipient_label = "agent-b",
      .raw_text = "queued reply text"});
  pi::core::RegionToolBlock following;
  following.call_id = "call-c";
  following.tool_name = "unrelated-c";
  following.args_json = "{}";
  following.raw_output = "c-result";
  turn.blocks.emplace_back(std::move(following));
  state.turns.push_back(std::move(turn));
  const auto frame = pi::core::build_region_frame(state, 80, 20);
  const auto first_tool = std::ranges::find_if(
      frame.lines, [](const auto &line) { return line.find("[unrelated-a]") != std::string::npos; });
  const auto reply = std::ranges::find_if(
      frame.lines, [](const auto &line) { return line.find("REPLY -> agent-b queued") != std::string::npos; });
  const auto second_tool = std::ranges::find_if(
      frame.lines, [](const auto &line) { return line.find("[unrelated-c]") != std::string::npos; });
  expect(first_tool != frame.lines.end() && reply != frame.lines.end() &&
             second_tool != frame.lines.end(),
         "reply and unrelated tools all render");
  expect(first_tool < reply && reply < second_tool,
         "reply block preserves call order between unrelated parallel tools");
  const auto a_result = std::ranges::find_if(
      frame.lines, [](const auto &line) { return line.find("a-result") != std::string::npos; });
  const auto c_result = std::ranges::find_if(
      frame.lines, [](const auto &line) { return line.find("c-result") != std::string::npos; });
  expect(a_result != frame.lines.end() && c_result != frame.lines.end(),
         "unrelated tool output is untouched by the reply block");
}

void test_region_factory_lifecycle() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates lifecycle capture fd");
  if (!pipe_ok)
    return;
  expect(pi::core::StreamRendererRegistry::instance().has("region"),
         "registry exposes region renderer");
  {
    auto renderer =
        pi::core::StreamRendererRegistry::instance().make("region", fds[1]);
    expect(renderer != nullptr, "registry constructs region renderer");
    if (renderer) {
      renderer->on_turn_start();
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      renderer->on_text_delta("a");
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      renderer->on_text_delta("b");
      std::this_thread::sleep_for(std::chrono::milliseconds(40));
      renderer->on_turn_end();
      renderer->on_turn_start();
      renderer->on_text_delta("short");
      renderer->on_turn_end();
    }
  }
  ::close(fds[1]);
  std::string output;
  char buffer[256];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);
  expect(output.find("\033[?1049h") != std::string::npos,
         "region enters alternate screen");
  expect(output.find("\033[?1049l") != std::string::npos,
         "region leaves alternate screen");
  // Default (non-tty) terminal height is 24; the composer now reserves
  // kMaxComposerRows rows at the bottom, so the status row sits
  // kMaxComposerRows rows above the last line instead of just one.
  expect(output.find("\033[18;1H\033[?25h") != std::string::npos,
         "turn end leaves cursor on the status row before readline");
  expect(output.find("\033[H\033[J") == std::string::npos,
         "new turns do not clear the alternate screen");
  expect(output.find("ab") != std::string::npos &&
             output.find("short") != std::string::npos,
         "completed transcript remains available across turns");
}

// M1 (composer-textarea-rewrite plan): the composer now reserves a fixed
// kMaxComposerRows-row budget at the bottom of the terminal (rather than
// the old single row) so a multi-line draft has somewhere to grow into
// without the transcript's scroll region resizing dynamically underneath
// it. Confirms the transcript's DECSTBM range, the status row, and the
// transcript content itself all agree on where that reservation starts.
void test_composer_rows_reserved_in_region_mode() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates composer-reservation capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    renderer->on_turn_start();
    renderer->on_text_delta("distinctive-transcript-content");
    renderer->on_turn_end();
  }
  ::close(fds[1]);
  std::string output;
  char buffer[512];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);

  // Default (non-tty) terminal is 24 rows x 80 columns.
  constexpr int kHeight = 24;
  const int reserved = static_cast<int>(pi::core::kMaxComposerRows);
  const auto expected_scroll_region =
      "\033[1;" + std::to_string(kHeight - 1 - reserved) + "r";
  const auto expected_status_row =
      "\033[" + std::to_string(kHeight - reserved) + ";1H";
  expect(output.find(expected_scroll_region) != std::string::npos,
         "transcript scroll region leaves room for the reserved composer "
         "rows");
  expect(output.find(expected_status_row) != std::string::npos,
         "status row sits directly above the reserved composer rows");
  expect(output.find("distinctive-transcript-content") != std::string::npos,
         "transcript content still renders above the reserved composer "
         "rows");
}

void test_subagent_pane_is_reserved_in_region_mode() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates subagent-pane capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    expect(renderer->owns_subagent_pane(),
           "region renderer advertises subagent pane support");
    renderer->on_turn_start();
    renderer->on_text_delta("root-transcript-content");
    renderer->set_subagent_pane({
        {.id = "child-1",
         .name = "research",
         .status = "running",
         .last_activity = "tool started"}});
    // Region rendering is coalesced on a background paint thread while a
    // turn is active; give that frame a chance to reach the capture pipe.
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
    renderer->on_turn_end();
  }
  ::close(fds[1]);
  std::string output;
  char buffer[512];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);

  expect(output.find("research") != std::string::npos,
         "subagent name is rendered in the reserved pane");
  expect(output.find("running") != std::string::npos,
         "subagent status is rendered in the reserved pane");
  expect(output.find("tool started") != std::string::npos,
         "subagent activity is rendered in the reserved pane");
  expect(output.find("─") != std::string::npos,
         "horizontal separator is rendered above the subagent pane");
  expect(output.find("root-transcript-content") != std::string::npos,
         "root transcript remains rendered separately");
}

// on_command_output() text (e.g. /memory's format_ascii_table() output) is
// already fully formatted and must render byte-for-byte. Both region and
// viewport re-render command-output blocks through render_visible_markdown()
// for ANSI styling, and a bare CommonMark paragraph collapses every internal
// '\n' to a single space (softbreak) -- silently destroying a box-drawn
// table's line structure. Regression test for the fence-wrapping fix in
// RegionRenderer::on_command_output().
void test_command_output_table_survives_region_rendering() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates command-output capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    renderer->on_command_output("+------+-----------+\n"
                                "| session | allocated |\n"
                                "+------+-----------+\n"
                                "| root | 1.2 MB    |\n"
                                "+------+-----------+\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(80));
  }
  ::close(fds[1]);
  std::string output;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);

  // The bug collapses every row onto one line, so the border sequence would
  // appear only once. A correctly preserved table paints each border row at
  // its own absolute cursor position -- assert more than one distinct
  // "\033[<row>;1H...+------+" occurrence rather than just substring
  // presence, so a regression back to one squished line still fails.
  std::size_t border_rows = 0;
  std::size_t pos = 0;
  while ((pos = output.find("+------+-----------+", pos)) != std::string::npos) {
    ++border_rows;
    pos += 1;
  }
  expect(border_rows >= 3,
         "table borders appear as separate painted rows, not squished into one");
  expect(output.find("| root | 1.2 MB    |") != std::string::npos,
         "a data row survives intact with its internal spacing");
}

void test_command_output_table_survives_viewport_rendering() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates command-output capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_viewport_renderer(fds[1]);
    renderer->on_command_output("+------+-----------+\n"
                                "| session | allocated |\n"
                                "+------+-----------+\n"
                                "| root | 1.2 MB    |\n"
                                "+------+-----------+\n");
  }
  ::close(fds[1]);
  std::string output;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);

  std::size_t border_rows = 0;
  std::size_t pos = 0;
  while ((pos = output.find("+------+-----------+", pos)) != std::string::npos) {
    ++border_rows;
    pos += 1;
  }
  expect(border_rows >= 3,
         "table borders appear as separate lines, not squished into one");
  expect(output.find("| root | 1.2 MB    |") != std::string::npos,
         "a data row survives intact with its internal spacing");
}

// prepare_for_prompt() -- called by the interactive loop immediately before
// every readline() call -- must re-anchor the composer's reserved rows
// regardless of what ran just before it (a real turn, a between-turn
// command like /usage, on_resize(), ...). Without this, the composer/footer
// left by the prompt that submitted the last input is never wiped, and the
// next readline() prompt draws one row lower instead of reusing it,
// compounding into a drifting stack of stale composers with every
// subsequent command.
void test_prepare_for_prompt_reanchors_composer() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates prepare-for-prompt capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    renderer->on_command_output("distinctive-command-output");
    renderer->prepare_for_prompt();
  }
  ::close(fds[1]);
  std::string output;
  char buffer[512];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);

  // Default (non-tty) terminal is 24 rows x 80 columns.
  constexpr int kHeight = 24;
  const int reserved = static_cast<int>(pi::core::kMaxComposerRows);
  const int prompt_anchor = kHeight - reserved;
  const auto reanchor_sequence =
      "\033[" + std::to_string(prompt_anchor) + ";1H\033[?25h";

  const auto content_pos = output.find("distinctive-command-output");
  expect(content_pos != std::string::npos,
         "command output is visible in the transcript");
  expect(content_pos != std::string::npos &&
             output.find(reanchor_sequence, content_pos) != std::string::npos,
         "prepare_for_prompt() re-anchors the composer's prompt cursor after "
         "painting, not just the constructor's initial placement");
}

// Reproduces the reported "footer disappears once tool calls get displayed"
// bug. A single user-visible exchange fires on_turn_start()/on_turn_end()
// once per model round-trip when the agent calls tools (agent_loop.cpp's
// inner tool-calling loop), not once for the whole exchange -- readline()
// only runs again after the *last* round, not between intermediate ones. An
// on_turn_end() that wipes/re-anchors the composer's reserved rows itself
// (as it used to, via position_prompt_cursor()) blanks them the moment a
// second round starts, since nothing but readline()'s own InputRenderer
// ever redraws the "› " prompt and footer hint there, and readline() isn't
// called again until the whole exchange ends. Only prepare_for_prompt() may
// do that wipe/re-anchor; on_turn_end() alone must leave the reserved rows
// untouched so the composer's last-known-good contents keep showing through
// every intermediate round.
void test_intermediate_turn_end_does_not_blank_composer() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates multi-round capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    // Round 1: a tool call, then this round's turn ends.
    renderer->on_turn_start();
    renderer->on_tool_start("call-1", "bash", R"({"command":"echo hi"})");
    renderer->on_tool_end("call-1", "bash", TestToolResult{"hi"}, false);
    renderer->on_turn_end();
    // Round 2 starts immediately, exactly like agent_loop.cpp's inner loop
    // does when has_more_tool_calls is true -- no readline() call, and thus
    // no prepare_for_prompt() call, happens in between.
    renderer->on_turn_start();
    renderer->on_tool_start("call-2", "bash", R"({"command":"echo bye"})");
  }
  ::close(fds[1]);
  std::string output;
  char buffer[512];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);

  // Default (non-tty) terminal is 24 rows x 80 columns.
  constexpr int kHeight = 24;
  const int reserved = static_cast<int>(pi::core::kMaxComposerRows);
  const int prompt_anchor = kHeight - reserved;
  const auto reanchor_sequence =
      "\033[" + std::to_string(prompt_anchor) + ";1H\033[?25h";

  // The constructor performs exactly one re-anchor up front, before either
  // round runs -- anything beyond that would mean on_turn_end() (round 1)
  // re-anchored on its own, which is the bug this test guards against.
  std::size_t occurrences = 0;
  for (std::size_t pos = output.find(reanchor_sequence); pos != std::string::npos;
       pos = output.find(reanchor_sequence, pos + 1))
    ++occurrences;
  expect(occurrences == 1,
         "on_turn_end() must not wipe/re-anchor the composer when another "
         "round is already starting -- only prepare_for_prompt() may");
}

// force_full_repaint() exists so a transient full-screen command UI (/tree,
// /model) that drew directly into this renderer's alternate screen — see
// docs/region-renderer.md's "Transient full-screen command UIs" section —
// can hand back a clean screen. An ordinary dirty-driven repaint of
// unchanged content diffs against the cache and writes nothing; this must
// unconditionally discard the cache and repaint every row regardless.
void test_force_full_repaint_reissues_every_row() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates force-repaint capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    renderer->on_turn_start();
    renderer->on_text_delta("distinctive-repaint-marker");
    renderer->on_message_end_presentation(pi::core::MessageEndPresentation{
        .stop_reason = pi::core::StopReason::stop});
    renderer->on_turn_end();
    renderer->force_full_repaint();
  }
  ::close(fds[1]);
  std::string output;
  char buffer[512];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);
  const auto first = output.find("distinctive-repaint-marker");
  const auto second =
      first == std::string::npos
          ? std::string::npos
          : output.find("distinctive-repaint-marker", first + 1);
  expect(first != std::string::npos && second != std::string::npos,
         "force_full_repaint() unconditionally repaints content the diff "
         "cache would otherwise consider unchanged");
}

void test_idle_paint_saves_cursor_and_scrolls() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates idle paint capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    std::string command_output;
    for (int line = 0; line < 40; ++line)
      command_output += "command output " + std::to_string(line) + "\n\n";
    renderer->on_command_output(command_output);
    renderer->set_status_line(std::string("custom status"));
    renderer->on_scroll(pi::core::RendererScrollCommand::top);
    renderer->on_scroll(pi::core::RendererScrollCommand::line_up);
    renderer->on_scroll(pi::core::RendererScrollCommand::page_down);
    renderer->set_status_line(std::nullopt);
    renderer->on_error(pi::core::RendererErrorKind::transport,
                       "connection lost");
    renderer->on_scroll(pi::core::RendererScrollCommand::bottom);
  }
  ::close(fds[1]);
  std::string output;
  char buffer[256];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);
  expect(output.find("\0337") != std::string::npos,
         "idle updates save the readline cursor");
  expect(output.find("\0338") != std::string::npos,
         "idle updates restore the readline cursor");
  expect(output.find("custom status") != std::string::npos,
         "idle status update is painted synchronously");
  expect(output.find("scroll ") != std::string::npos,
         "idle scrolling paints a nonzero scroll extent");
  expect(output.find("error: connection lost") != std::string::npos,
         "idle errors are painted in the compositor");
}

void test_custom_status_line_outranks_builtin_status_text() {
  // An addon-provided status line (e.g. costline) must outrank the built-in
  // "tokens: N  done" text that on_turn_end() leaves in status_text --
  // otherwise the addon line is buried for the whole idle period between
  // turns. Errors remain the one exception: they always outrank the addon.
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates status precedence capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    renderer->set_status_line(std::string("custom status"));
    renderer->on_turn_start();
    renderer->on_message_end(pi::core::TokenUsage{.input = 12, .output = 34});
    renderer->on_turn_end();
  }
  ::close(fds[1]);
  std::string output;
  char buffer[256];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);
  // The turn-end frame paints "tokens: ..." (addon not yet consulted at
  // that exact instant), but the idle repaint driven by the next
  // set_status_line() must show the addon line, never the built-in token
  // tally.
  expect(output.find("custom status") != std::string::npos,
         "custom status line survives a completed turn");
  expect(output.find("tokens: ") == std::string::npos,
         "built-in token tally must not bury the custom status line");
}

void test_explicit_turn_request_sections() {
  pi::core::RegionState state;
  pi::core::RegionTurn ordinary;
  ordinary.requests.push_back(pi::core::RegionRequestBlock{
      .metadata = {}, .raw_text = "ordinary prompt"});
  ordinary.blocks.emplace_back(
      pi::core::RegionTextBlock{.raw = "ordinary answer"});
  state.turns.push_back(std::move(ordinary));

  pi::core::RegionTurn mailbox;
  pi::core::InputProvenance metadata{.source =
                                             pi::core::InputProvenance::Source::mailbox,
                                         .sender_task_path = "/root/luna"};
  mailbox.requests.push_back(
      pi::core::RegionRequestBlock{.metadata = metadata,
                                   .raw_text = "mailbox prompt\nwrapped text",
                                   .non_text_attachments = 1});
  mailbox.requests.push_back(pi::core::RegionRequestBlock{
      .metadata = metadata, .raw_text = "second prompt"});
  mailbox.blocks.emplace_back(
      pi::core::RegionTextBlock{.raw = "mailbox answer"});
  mailbox.complete = true;
  state.turns.push_back(std::move(mailbox));

  const auto frame = pi::core::build_region_frame(state, 40, 40);
  std::size_t request_headings = 0;
  std::size_t ordinary_prompt = frame.lines.size();
  std::size_t mailbox_prompt = frame.lines.size();
  std::size_t mailbox_heading = frame.lines.size();
  std::size_t attachment = frame.lines.size();
  std::size_t answer = frame.lines.size();
  for (std::size_t index = 0; index < frame.lines.size(); ++index) {
    const auto &line = frame.lines[index];
    request_headings += line.find("REQUEST") != std::string::npos;
    if (line.find("MAILBOX") != std::string::npos)
      mailbox_heading = index;
    if (line.find("ordinary prompt") != std::string::npos)
      ordinary_prompt = index;
    if (line.find("mailbox prompt") != std::string::npos)
      mailbox_prompt = index;
    if (line.find("[image attachment]") != std::string::npos)
      attachment = index;
    if (line.find("mailbox answer") != std::string::npos)
      answer = index;
  }
  expect(request_headings == 2, "each explicit turn has one request heading");
  expect(frame.lines[0].find("REQUEST") != std::string::npos,
         "ordinary request heading is visible");
  expect(mailbox_prompt < frame.lines.size() &&
             frame.lines[mailbox_prompt].find("MAILBOX") == std::string::npos,
         "mailbox source stays on its heading");
  expect(mailbox_heading < frame.lines.size(),
         "mailbox heading includes source");
  expect(std::any_of(frame.lines.begin(), frame.lines.end(),
                     [](const auto &line) {
                       return line.find("root/luna") != std::string::npos;
                     }),
         "mailbox heading prefers task path");
  expect(ordinary_prompt < mailbox_prompt,
         "completed history precedes new request");
  expect(attachment < answer, "attachment placeholder stays before answer");
  expect(frame.lines[answer].find("mailbox answer") != std::string::npos,
         "completed turn preserves answer text");

  state.scroll_offset_rows = frame.max_scroll_rows;
  const auto top = pi::core::build_region_frame(state, 40, 4);
  expect(top.total_rows > top.lines.size(),
         "explicit turns expose physical scroll rows");
  expect(top.max_scroll_rows > 0, "explicit turns have a scroll boundary");
}

void test_request_audit_behaviors() {
  pi::core::RegionState empty;
  empty.turns.push_back(pi::core::RegionTurn{
      .requests = {pi::core::RegionRequestBlock{}}, .blocks = {}});
  const auto empty_frame = pi::core::build_region_frame(empty, 40, 20);
  expect(std::all_of(empty_frame.lines.begin(), empty_frame.lines.end(),
                     [](const auto &line) {
                       return line.find("REQUEST") == std::string::npos;
                     }),
         "legacy empty ordinary requests do not paint a heading");

  pi::core::RegionTurn mixed;
  mixed.requests.push_back(
      pi::core::RegionRequestBlock{.metadata = {}, .raw_text = "ordinary"});
  mixed.requests.push_back(pi::core::RegionRequestBlock{
      .metadata =
          pi::core::InputProvenance{.source =
                                            pi::core::InputProvenance::Source::mailbox,
                                        .sender_task_path = "/root/task"},
      .raw_text = "mailbox"});
  const auto mixed_frame = pi::core::build_region_frame(
      pi::core::RegionState{.turns = {std::move(mixed)}}, 40, 20);
  expect(mixed_frame.lines[0] == "\033[1;36mREQUEST\033[0m",
         "mixed request provenance uses a neutral heading");
  expect(std::all_of(mixed_frame.lines.begin(), mixed_frame.lines.end(),
                     [](const auto &line) {
                       return line.find("MAILBOX") == std::string::npos &&
                              line.find("/root/task") == std::string::npos;
                     }),
         "mixed request heading does not claim one provenance");

  pi::core::InputProvenance long_sender{
      .source = pi::core::InputProvenance::Source::mailbox,
      .sender_agent_id = "abcdefghijklmnopqrstuvwx12345"};
  pi::core::RegionTurn narrow;
  narrow.requests.push_back(pi::core::RegionRequestBlock{
      .metadata = std::move(long_sender), .raw_text = "prompt"});
  const auto narrow_frame = pi::core::build_region_frame(
      pi::core::RegionState{.turns = {std::move(narrow)}}, 12, 20);
  const auto visible_text = [](std::string_view line) {
    std::string text;
    for (std::size_t index = 0; index < line.size(); ++index) {
      if (line[index] != '\033') {
        text.push_back(line[index]);
        continue;
      }
      ++index;
      if (index < line.size() && line[index] == '[')
        while (index + 1 < line.size() && line[++index] != 'm')
          ;
    }
    return text;
  };
  std::string narrow_text;
  for (const auto &line : narrow_frame.lines)
    narrow_text += visible_text(line);
  expect(narrow_text.find("abcdefghijklmnop...12345") != std::string::npos,
         "long raw agent IDs use a deterministic shortened label");
  const auto visible_length = [](std::string_view line) {
    std::size_t length = 0;
    for (std::size_t index = 0; index < line.size(); ++index) {
      if (line[index] != '\033') {
        ++length;
        continue;
      }
      ++index;
      if (index < line.size() && line[index] == '[')
        while (index + 1 < line.size() && line[++index] != 'm')
          ;
    }
    return length;
  };
  expect(
      std::all_of(narrow_frame.lines.begin(), narrow_frame.lines.end(),
                  [&](const auto &line) { return visible_length(line) <= 12; }),
      "long request headings wrap to the requested width");

  pi::core::RegionState command_state;
  pi::core::RegionTurn before_first;
  before_first.blocks.emplace_back(
      pi::core::RegionTextBlock{"command before first"});
  before_first.complete = true;
  command_state.turns.push_back(std::move(before_first));
  pi::core::RegionTurn first;
  first.requests.push_back(pi::core::RegionRequestBlock{
      .metadata = {}, .raw_text = "first request"});
  first.blocks.emplace_back(pi::core::RegionTextBlock{"first answer"});
  first.complete = true;
  command_state.turns.push_back(std::move(first));
  command_state.turns.emplace_back();
  command_state.turns.back().complete = true;
  command_state.turns.back().blocks.emplace_back(
      pi::core::RegionTextBlock{"command output"});
  expect(command_state.turns.back().complete,
         "standalone idle command turns are complete");
  pi::core::RegionTurn second;
  second.requests.push_back(pi::core::RegionRequestBlock{
      .metadata = {}, .raw_text = "second request"});
  command_state.turns.push_back(std::move(second));
  const auto command_frame =
      pi::core::build_region_frame(command_state, 40, 20);
  std::size_t before_first_command = command_frame.lines.size();
  std::size_t first_answer = command_frame.lines.size();
  std::size_t command = command_frame.lines.size();
  std::size_t second_heading = command_frame.lines.size();
  for (std::size_t index = 0; index < command_frame.lines.size(); ++index) {
    if (command_frame.lines[index].find("first answer") != std::string::npos)
      first_answer = index;
    if (command_frame.lines[index].find("command before first") !=
        std::string::npos)
      before_first_command = index;
    if (command_frame.lines[index].find("command output") != std::string::npos)
      command = index;
    if (index > command &&
        command_frame.lines[index].find("REQUEST") != std::string::npos)
      second_heading = index;
  }
  expect(before_first_command < first_answer,
         "idle command output remains visible before the first turn");
  expect(first_answer < command && command < second_heading,
         "idle command output remains standalone between completed turns");

  pi::core::RegionTurn unsafe;
  unsafe.requests.push_back(pi::core::RegionRequestBlock{
      .metadata =
          pi::core::InputProvenance{
              .source = pi::core::InputProvenance::Source::mailbox,
              .sender_task_path = "\033[31msender\033[0m\033]52;c;bad\007"},
      .raw_text = "visible \033[2Jtext \033[?25l\033[31mred\033[0m"});
  const auto unsafe_frame = pi::core::build_region_frame(
      pi::core::RegionState{.turns = {std::move(unsafe)}}, 10, 20);
  std::string unsafe_text;
  for (const auto &line : unsafe_frame.lines) {
    unsafe_text += visible_text(line);
    expect(line.ends_with("\033[0m"),
           "sanitized request rows close their SGR state");
    expect(visible_length(line) <= 10,
           "sanitized request rows stay within the requested width");
  }
  // Word wrapping legitimately drops the space at whatever word boundary a
  // row happens to break on (see split_region_lines), so compare with
  // interior spaces collapsed rather than requiring an exact byte-for-byte
  // "visible text red" substring across the wrapped rows.
  std::string unsafe_text_no_spaces;
  for (const char c : unsafe_text) {
    if (c != ' ')
      unsafe_text_no_spaces.push_back(c);
  }
  expect(unsafe_text_no_spaces.find("visibletextred") != std::string::npos,
         "sanitized request keeps visible text");
  expect(unsafe_text.find("2J") == std::string::npos &&
             unsafe_text.find("?25l") == std::string::npos &&
             unsafe_text.find("52;c;bad") == std::string::npos,
         "sanitized request removes cursor, clear-screen, and OSC controls");
}

void test_assistant_section_classification() {
  auto count_heading = [](const pi::core::RegionFrame &frame,
                          std::string_view heading) {
    std::size_t count = 0;
    for (const auto &line : frame.lines)
      count += line.find(heading) != std::string::npos ? 1U : 0U;
    return count;
  };

  pi::core::RegionState state;
  pi::core::RegionTurn turn;
  turn.blocks.emplace_back(pi::core::RegionTextBlock{
      .raw = "streaming preamble",
      .kind = pi::core::RegionAssistantTextKind::provisional});
  turn.blocks.emplace_back(pi::core::RegionTextBlock{
      .raw = "checking tools",
      .kind = pi::core::RegionAssistantTextKind::work});
  turn.blocks.emplace_back(pi::core::RegionThinkingBlock{"reasoning"});
  pi::core::RegionToolBlock tool;
  tool.call_id = "tool-call";
  tool.tool_name = "tool";
  tool.args_json = "{}";
  tool.running = false;
  tool.raw_output = "tool output";
  turn.blocks.emplace_back(std::move(tool));
  turn.blocks.emplace_back(pi::core::RegionTextBlock{
      .raw = "more work", .kind = pi::core::RegionAssistantTextKind::work});
  turn.blocks.emplace_back(pi::core::RegionTextBlock{
      .raw = "final answer",
      .kind = pi::core::RegionAssistantTextKind::answer});
  state.turns.push_back(std::move(turn));

  auto frame = pi::core::build_region_frame(state, 80, 30);
  expect(count_heading(frame, "ASSISTANT...") == 1,
         "streaming assistant text has a provisional heading");
  expect(count_heading(frame, "WORK") == 1,
         "contiguous narration, thinking, and tools share one WORK heading");
  expect(count_heading(frame, "ANSWER") == 1,
         "terminal assistant text has one ANSWER heading");
  expect(frame.lines.back().find("final answer") != std::string::npos,
         "final answer remains after ordered work blocks");

  auto &first = std::get<pi::core::RegionTextBlock>(state.turns[0].blocks[0]);
  first.kind = pi::core::RegionAssistantTextKind::answer;
  frame = pi::core::build_region_frame(state, 80, 30);
  expect(count_heading(frame, "ASSISTANT...") == 0 &&
             count_heading(frame, "ANSWER") == 2,
         "reclassification relabels the existing provisional block atomically");

  pi::core::RegionState truncated;
  pi::core::RegionTurn truncated_turn;
  truncated_turn.blocks.emplace_back(pi::core::RegionTextBlock{
      .raw = "partial output",
      .kind = pi::core::RegionAssistantTextKind::answer_truncated});
  truncated.turns.push_back(std::move(truncated_turn));
  frame = pi::core::build_region_frame(truncated, 80, 10);
  expect(count_heading(frame, "ANSWER | TRUNCATED") == 1,
         "length stop reasons use a truncated answer heading");

  pi::core::RegionState failed_state;
  pi::core::RegionTurn failed_turn;
  failed_turn.blocks.emplace_back(pi::core::RegionTextBlock{
      .raw = "error text", .kind = pi::core::RegionAssistantTextKind::work});
  failed_state.turns.push_back(std::move(failed_turn));
  frame = pi::core::build_region_frame(failed_state, 80, 10);
  expect(count_heading(frame, "WORK") == 1 &&
             count_heading(frame, "ANSWER") == 0,
         "error and aborted assistant text remain under WORK");

  pi::core::RegionState empty_answer;
  pi::core::RegionTurn empty_turn;
  empty_turn.blocks.emplace_back(pi::core::RegionTextBlock{
      .kind = pi::core::RegionAssistantTextKind::answer});
  empty_answer.turns.push_back(std::move(empty_turn));
  frame = pi::core::build_region_frame(empty_answer, 80, 10);
  expect(count_heading(frame, "ANSWER") == 1,
         "empty terminal answers still expose an ANSWER section");

  const auto visible_length = [](std::string_view line) {
    std::size_t length = 0;
    for (std::size_t index = 0; index < line.size(); ++index) {
      if (line[index] != '\033') {
        ++length;
        continue;
      }
      ++index;
      if (index < line.size() && line[index] == '[')
        while (index + 1 < line.size() && line[++index] != 'm')
          ;
    }
    return length;
  };
  frame = pi::core::build_region_frame(state, 5, 4);
  for (const auto &line : frame.lines) {
    expect(line.ends_with("\033[0m"),
           "semantic headings and content reset every narrow row");
    expect(visible_length(line) <= 5,
           "semantic headings remain within narrow physical width");
  }
}

void test_callback_classification_lifecycle() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates callback classification capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    renderer->on_turn_start();
    renderer->on_text_delta("streaming preamble");
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    renderer->on_tool_start("call", "tool", "{}");
    renderer->on_message_end_presentation(pi::core::MessageEndPresentation{
        .stop_reason = pi::core::StopReason::tool_use});
    renderer->on_tool_end("call", "tool", TestToolResult{"tool result"}, false);
    renderer->on_text_delta("final answer");
    renderer->on_message_end_presentation(pi::core::MessageEndPresentation{
        .stop_reason = pi::core::StopReason::stop});
    renderer->on_turn_end();

    renderer->on_turn_start();
    renderer->on_text_delta("truncated text");
    renderer->on_message_end_presentation(pi::core::MessageEndPresentation{
        .stop_reason = pi::core::StopReason::length});
    renderer->on_turn_end();

    renderer->on_turn_start();
    renderer->on_text_delta("error text");
    renderer->on_error(pi::core::RendererErrorKind::llm, "failed");
    renderer->on_message_end_presentation(pi::core::MessageEndPresentation{
        .stop_reason = pi::core::StopReason::error});
    renderer->on_turn_end();

    renderer->on_turn_start();
    renderer->on_text_delta("aborted text");
    renderer->on_message_end_presentation(pi::core::MessageEndPresentation{
        .stop_reason = pi::core::StopReason::aborted});
    renderer->on_turn_end();
  }
  ::close(fds[1]);
  std::string output;
  char buffer[512];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);
  expect(output.find("ASSISTANT...") != std::string::npos,
         "callback text is initially provisional");
  expect(output.find("WORK") != std::string::npos,
         "tool-use preamble and failed text are rendered as work");
  expect(output.find("ANSWER") != std::string::npos,
         "stop callback reclassifies the existing text as answer");
  expect(output.find("ANSWER | TRUNCATED") != std::string::npos,
         "length callback preserves a truncated answer label");
  expect(output.find("[38;5;245mWORK") != std::string::npos,
         "WORK uses subordinate renderer-owned styling");
  expect(output.find("[1;97mANSWER") != std::string::npos,
         "ANSWER uses high-contrast renderer-owned styling");
  expect(output.find("ASSISTANT...") < output.find("WORK") &&
             output.find("WORK") < output.find("ANSWER"),
         "callback lifecycle preserves provisional, work, and answer order");
}

void test_error_survives_fast_turn_end() {
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates fast error capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    renderer->set_status_line(std::string("custom status"));
    renderer->on_turn_start();
    renderer->on_message_end(pi::core::TokenUsage{.input = 12, .output = 34});
    renderer->on_error(pi::core::RendererErrorKind::transport,
                       "request failed");
    renderer->on_turn_end();
  }
  ::close(fds[1]);
  std::string output;
  char buffer[256];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);
  expect(output.find("error: request failed") != std::string::npos,
         "fast turn errors remain visible in the final frame");
  expect(output.rfind("error: request failed") > output.rfind("custom status"),
         "error status outranks the custom status line");
  expect(output.find("in:12 out:34") != std::string::npos,
         "usage is visible in the status bar");
}

void test_turn_based_scroll_reaches_older_turns() {
  // Reproduces the reported "scrolling doesn't work with the region
  // renderer" complaint via the real event path (on_turn_start /
  // on_text_delta / on_turn_end) instead of hand-built RegionState, since
  // that's what actual chat sessions exercise and none of the existing
  // scroll coverage goes through it (test_tail_anchor_and_scroll only
  // covers the legacy state.blocks path).
  int fds[2]{};
  const bool pipe_ok = ::pipe(fds) == 0;
  expect(pipe_ok, "pipe creates turn-scroll capture fd");
  if (!pipe_ok)
    return;
  {
    auto renderer = pi::core::make_region_renderer(fds[1]);
    for (int turn = 0; turn < 15; ++turn) {
      renderer->on_turn_start();
      renderer->on_text_delta("turn " + std::to_string(turn) +
                              " marker line one\n\nturn " +
                              std::to_string(turn) + " marker line two\n\n");
      renderer->on_turn_end();
    }
    renderer->on_scroll(pi::core::RendererScrollCommand::bottom);
    renderer->on_scroll(pi::core::RendererScrollCommand::top);
  }
  ::close(fds[1]);
  std::string output;
  char buffer[4096];
  for (;;) {
    const auto count = ::read(fds[0], buffer, sizeof(buffer));
    if (count <= 0)
      break;
    output.append(buffer, static_cast<std::size_t>(count));
  }
  ::close(fds[0]);
  expect(output.find("turn 14") != std::string::npos,
         "latest turn is reachable before scrolling");
  expect(output.find("turn 0 marker") != std::string::npos,
         "scrolling to top repaints the earliest turn's content");
}

} // namespace

int main() {
  test_ordered_transcript_blocks();
  test_tail_anchor_and_scroll();
  test_thinking_is_inserted_at_the_current_turn_boundary();
  test_sgr_reopens_after_wrap();
  test_word_wrap_breaks_on_spaces();
  test_word_wrap_overlong_token_hard_wraps();
  test_word_wrap_sgr_reopens_after_word_boundary_break();
  test_diff_only_changes_rows();
  test_diff_scrolls_tail_without_repainting_every_row();
  test_degenerate_sizing();
  test_tool_regions_preserve_call_order();
  test_tool_updates_are_isolated();
  test_interleaved_text_rounds();
  test_tool_body_cap_and_expansion_cap();
  test_custom_tool_output_is_rendered();
  test_formatter_regions_cap_and_reset();
  test_formatter_sanitization_contract();
  test_tiny_layout_keeps_tool_body_collapsed();
  test_tool_callbacks_route_into_regions();
  test_tool_call_streaming_finalizes_into_single_block();
  test_tool_call_streaming_interleaved_calls_stay_isolated();
  test_region_factory_lifecycle();
  test_composer_rows_reserved_in_region_mode();
  test_subagent_pane_is_reserved_in_region_mode();
  test_command_output_table_survives_region_rendering();
  test_command_output_table_survives_viewport_rendering();
  test_prepare_for_prompt_reanchors_composer();
  test_intermediate_turn_end_does_not_blank_composer();
  test_force_full_repaint_reissues_every_row();
  test_mailbox_reply_receipt_is_persistent_and_safe();
  test_mailbox_reply_alongside_parallel_unrelated_tools();
  test_mailbox_reply_callback_path();
  test_idle_paint_saves_cursor_and_scrolls();
  test_custom_status_line_outranks_builtin_status_text();
  test_turn_based_scroll_reaches_older_turns();
  test_explicit_turn_request_sections();
  test_request_audit_behaviors();
  test_assistant_section_classification();
  test_callback_classification_lifecycle();
  test_error_survives_fast_turn_end();
  if (failed != 0)
    return 1;
  std::cout << "region renderer tests passed\n";
  return 0;
}
