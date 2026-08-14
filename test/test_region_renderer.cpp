#include "core/message_types.h"
#include "core/region_renderer.h"
#include "core/stream_renderer.h"

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
  expect(output.find("\033[23;1H\033[?25h") != std::string::npos,
         "turn end leaves cursor on the status row before readline");
  expect(output.find("\033[H\033[J") == std::string::npos,
         "new turns do not clear the alternate screen");
  expect(output.find("ab") != std::string::npos &&
             output.find("short") != std::string::npos,
         "completed transcript remains available across turns");
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

} // namespace

int main() {
  test_ordered_transcript_blocks();
  test_tail_anchor_and_scroll();
  test_thinking_is_inserted_at_the_current_turn_boundary();
  test_sgr_reopens_after_wrap();
  test_diff_only_changes_rows();
  test_diff_scrolls_tail_without_repainting_every_row();
  test_degenerate_sizing();
  test_tool_regions_preserve_call_order();
  test_tool_updates_are_isolated();
  test_interleaved_text_rounds();
  test_tool_body_cap_and_expansion_cap();
  test_custom_tool_output_is_rendered();
  test_tiny_layout_keeps_tool_body_collapsed();
  test_tool_callbacks_route_into_regions();
  test_region_factory_lifecycle();
  test_idle_paint_saves_cursor_and_scrolls();
  test_error_survives_fast_turn_end();
  if (failed != 0)
    return 1;
  std::cout << "region renderer tests passed\n";
  return 0;
}
