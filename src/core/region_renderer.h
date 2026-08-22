#pragma once

#include "core/request_presentation.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace pi::core {

enum class RegionAssistantTextKind {
  provisional,
  work,
  answer,
  answer_truncated
};

struct RegionRequestBlock {
  RequestPresentation metadata;
  std::string raw_text;
  std::size_t non_text_attachments{0};
};

struct RegionThinkingBlock {
  std::string raw;
};

struct RegionTextBlock {
  std::string raw;
  RegionAssistantTextKind kind{RegionAssistantTextKind::provisional};
  std::uint64_t message_sequence{0};
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

struct RegionReplyBlock {
  std::string request_message_id;
  std::string call_id;
  std::string recipient_label;
  std::string raw_text;
};

using RegionTurnBlock = std::variant<RegionTextBlock, RegionThinkingBlock,
                                     RegionToolBlock, RegionReplyBlock>;

struct RegionTurn {
  std::vector<RegionRequestBlock> requests;
  std::vector<RegionTurnBlock> blocks;
  bool complete{false};
};

using RegionBlock = std::variant<RegionTextBlock, RegionToolBlock>;

struct RegionToolAddress {
  std::size_t turn_index{0};
  std::size_t block_index{0};
};

struct RegionState {
  std::vector<RegionTurn> turns;
  std::size_t active_turn_index{0};
  bool has_active_turn{false};

  // Legacy flat fields remain accepted by the pure builder for callers that
  // render a transcript without turn callbacks.
  std::vector<RegionBlock> blocks;
  std::unordered_map<std::string, std::size_t> tool_index;
  std::unordered_map<std::string, RegionToolAddress> tool_addresses;
  // Correlates a still-streaming tool call's content_index (stable before
  // its call_id is known) to the block on_tool_call_streaming() created for
  // it, so on_tool_start() can finalize that same block in place once the
  // whole call has parsed instead of appending a duplicate. Reset every
  // on_turn_start() alongside tool_index/tool_addresses.
  std::unordered_map<std::size_t, RegionToolAddress> drafting_tool_addresses;
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

RegionFrame build_region_frame(const RegionState &state, int width,
                               int content_rows);

std::string diff_region_rows(const std::vector<std::string> &old_rows,
                             const std::vector<std::string> &new_rows);

} // namespace pi::core
