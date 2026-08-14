#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pi::core {

// Append-ordered transcript and tool blocks make the compositor's ordering
// contract explicit for its pure frame builder.
struct RegionTextBlock {
  std::string raw;
};

struct RegionToolBlock {
  std::string call_id;
  std::string tool_name;
  std::string args_json;
  std::string raw_output;
  std::string custom_call_output;
  std::string custom_result_output;
  bool running{true};
  bool is_error{false};
};

using RegionBlock = std::variant<RegionTextBlock, RegionToolBlock>;

struct RegionState {
  std::vector<RegionBlock> blocks;
  std::unordered_map<std::string, std::size_t> tool_index;
  std::string thinking;
  std::size_t thinking_block_index{0};
  bool in_thinking{false};
  int scroll_offset_rows{0};
};

struct RegionFrame {
  std::vector<std::string> lines;
  int max_scroll_rows{0};
  int total_rows{0};
};

// Build the visible transcript rows without touching a terminal. Each row is
// self-contained with respect to SGR state so a row can be repainted alone.
RegionFrame build_region_frame(const RegionState &state, int width,
                               int content_rows);

// Produce the terminal writes needed to change old_rows into new_rows. The
// returned string is empty when no row changed.
std::string diff_region_rows(const std::vector<std::string> &old_rows,
                             const std::vector<std::string> &new_rows);

} // namespace pi::core
