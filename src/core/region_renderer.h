#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace pi::core {

// One append-ordered transcript block. Tool blocks will be added in a later
// milestone; keeping this as a distinct type makes the ordering contract
// explicit for the compositor and its pure frame builder.
struct RegionTextBlock {
  std::string raw;
};

struct RegionState {
  std::vector<RegionTextBlock> blocks;
  std::string thinking;
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
