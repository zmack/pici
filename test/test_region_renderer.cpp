#include "core/region_renderer.h"
#include "core/stream_renderer.h"

#include <chrono>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
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
  state.blocks = {{"first"}, {"second"}};
  const auto frame = pi::core::build_region_frame(state, 80, 10);
  expect(frame.lines.size() >= 2, "ordered blocks produce rows");
  expect(frame.lines[0].starts_with("first"), "first block stays first");
  expect(frame.lines[1].starts_with("second"), "second block stays second");
  expect(frame.lines[0].ends_with("\033[0m"), "rows close SGR state");
}

void test_tail_anchor_and_scroll() {
  pi::core::RegionState state;
  state.blocks = {{"one"}, {"two"}, {"three"}, {"four"}};
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

void test_sgr_reopens_after_wrap() {
  pi::core::RegionState state;
  state.blocks = {{"\033[31mabcdef\033[0m"}};
  const auto frame = pi::core::build_region_frame(state, 3, 4);
  expect(frame.lines.size() == 2, "colored text wraps into two physical rows");
  expect(frame.lines[0].starts_with("\033[31mabc"), "first row includes color");
  expect(frame.lines[1].starts_with("\033[31mdef"),
         "wrapped row reopens color");
  expect(frame.lines[1].ends_with("\033[0m"), "wrapped row closes color");

  state.blocks = {{"\033[31mabc\033[mdef"}};
  const auto reset_frame = pi::core::build_region_frame(state, 3, 4);
  expect(reset_frame.lines.size() == 2, "empty SGR reset still wraps normally");
  expect(reset_frame.lines[1].starts_with("def"),
         "empty SGR reset does not leak color to continuation");

  state.blocks = {{"\033[0;31mabcdef"}};
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

void test_degenerate_sizing() {
  pi::core::RegionState state;
  state.blocks = {{"text"}};
  expect(pi::core::build_region_frame(state, 80, 0).lines.empty(),
         "zero content rows produce no output");
  expect(pi::core::build_region_frame(state, 0, 2).lines.empty(),
         "zero width produces no output");
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
  expect(output.find("\033[22;1H\033[?25h") != std::string::npos,
         "turn end leaves cursor at the content/status anchor");
  std::size_t clear_count = 0;
  for (std::size_t pos = output.find("\033[H\033[J"); pos != std::string::npos;
       pos = output.find("\033[H\033[J", pos + 1)) {
    ++clear_count;
  }
  expect(clear_count == 2,
         "turn-start clear is preserved for a short second turn");
}

} // namespace

int main() {
  test_ordered_transcript_blocks();
  test_tail_anchor_and_scroll();
  test_sgr_reopens_after_wrap();
  test_diff_only_changes_rows();
  test_degenerate_sizing();
  test_region_factory_lifecycle();
  if (failed != 0)
    return 1;
  std::cout << "region renderer tests passed\n";
  return 0;
}
