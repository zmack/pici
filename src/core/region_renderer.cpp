#include "core/region_renderer.h"

#include "core/message_types.h"
#include "core/request_presentation.h"
#include "core/stream_renderer.h"
#include "core/terminal.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace pi::core {
namespace {

constexpr int kFrameIntervalMs = 16;
constexpr std::size_t kMaxExpandedToolRegions = 6;
constexpr int kMaxToolBodyLines = 4;

std::string with_sgr_reset(std::string line) {
  if (!line.ends_with("\033[0m"))
    line += "\033[0m";
  return line;
}

bool is_sgr_sequence(std::string_view sequence) {
  return sequence.size() >= 3 && sequence.front() == '\033' &&
         sequence[1] == '[' && sequence.back() == 'm';
}

bool sgr_resets(std::string_view sequence) {
  if (!is_sgr_sequence(sequence))
    return false;
  const auto params = sequence.substr(2, sequence.size() - 3);
  if (params.empty())
    return true;
  std::size_t begin = 0;
  while (begin <= params.size()) {
    const auto end = params.find(';', begin);
    const auto token = params.substr(begin, end == std::string_view::npos
                                                ? params.size() - begin
                                                : end - begin);
    if (token == "0")
      return true;
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return false;
}

bool sgr_is_only_reset(std::string_view sequence) {
  if (!is_sgr_sequence(sequence))
    return false;
  const auto params = sequence.substr(2, sequence.size() - 3);
  if (params.empty())
    return true;
  std::size_t begin = 0;
  while (begin <= params.size()) {
    const auto end = params.find(';', begin);
    const auto token = params.substr(begin, end == std::string_view::npos
                                                ? params.size() - begin
                                                : end - begin);
    if (token != "0")
      return false;
    if (end == std::string_view::npos)
      break;
    begin = end + 1;
  }
  return true;
}

// split_lines() intentionally preserves escape sequences but does not reopen
// SGR state at a physical wrap. The compositor needs that extra property when
// a single row is diffed, so this small wrapper carries SGR prefixes forward.
std::vector<std::string> split_region_lines(std::string_view source,
                                            int width) {
  if (width <= 0)
    return {};

  std::vector<std::string> lines;
  std::string line;
  std::string active_sgr;
  int columns = 0;

  auto start_line = [&] {
    line = active_sgr;
    columns = 0;
  };
  auto finish_line = [&] {
    lines.push_back(with_sgr_reset(std::move(line)));
    line.clear();
    columns = 0;
  };
  start_line();

  for (std::size_t i = 0; i < source.size();) {
    if (source[i] == '\n') {
      finish_line();
      start_line();
      ++i;
      continue;
    }

    if (source[i] == '\033') {
      const auto next = skip_ansi_sequence(source, i);
      if (next > i) {
        const auto sequence = source.substr(i, next - i);
        line.append(sequence);
        if (is_sgr_sequence(sequence)) {
          if (sgr_is_only_reset(sequence)) {
            active_sgr.clear();
          } else if (sgr_resets(sequence)) {
            active_sgr = std::string(sequence);
          } else {
            active_sgr.append(sequence);
          }
        }
        i = next;
        continue;
      }
    }

    const auto next = advance_utf8(source, i);
    const int next_columns = columns + codepoint_width(source, i);
    if (columns > 0 && next_columns > width) {
      finish_line();
      start_line();
    }
    line.append(source.substr(i, next - i));
    columns += codepoint_width(source, i);
    i = next;
  }
  finish_line();
  return lines;
}

std::string tool_summary(const RegionToolBlock &tool) {
  std::string out = "[";
  out += tool.tool_name;
  out += tool.running ? "] running\xE2\x80\xA6" : "] ";
  if (!tool.running) {
    out += tool.is_error ? "error" : "done";
  }
  return out;
}

std::vector<std::string> tool_lines(const RegionToolBlock &tool, int width,
                                    bool expanded, int content_rows) {
  if (width <= 0)
    return {};

  if (!expanded)
    return split_region_lines(tool_summary(tool), width);

  if (!tool.running && !tool.custom_result_output.empty()) {
    auto custom_lines = split_region_lines(tool.custom_result_output, width);
    const auto max_custom_lines =
        static_cast<std::size_t>(kMaxToolBodyLines) + 1U;
    if (custom_lines.size() > max_custom_lines)
      custom_lines.resize(kMaxToolBodyLines + 1);
    return custom_lines;
  }

  std::string header;
  if (!tool.custom_call_output.empty()) {
    auto custom_header = split_region_lines(tool.custom_call_output, width);
    while (!custom_header.empty() && custom_header.front() == "\033[0m")
      custom_header.erase(custom_header.begin());
    if (custom_header.size() > 1)
      custom_header.resize(1);
    if (!custom_header.empty()) {
      header = std::move(custom_header.front());
      if (header.ends_with("\033[0m"))
        header.resize(header.size() - 4);
    }
  }
  if (header.empty()) {
    header = "[" + tool.tool_name + "] ";
    header += "\033[38;5;214m";
    header += tool.args_json;
    header += "\033[0m";
  }

  auto lines = split_region_lines(header, width);
  if (lines.size() >= static_cast<std::size_t>(content_rows))
    return lines;
  if (tool.raw_output.empty())
    return lines;

  std::string body = "\033[38;5;245m";
  body += truncate_tool_result(tool.raw_output);
  body += "\033[0m";
  auto body_lines = split_region_lines(body, width);
  if (body_lines.size() > static_cast<std::size_t>(kMaxToolBodyLines))
    body_lines.resize(kMaxToolBodyLines);
  lines.insert(lines.end(), std::make_move_iterator(body_lines.begin()),
               std::make_move_iterator(body_lines.end()));
  return lines;
}

std::vector<std::string> legacy_region_lines(const RegionState &state,
                                             int width, int content_rows) {
  std::vector<std::string> lines;
  if (width <= 0)
    return lines;

  const auto append_thinking = [&] {
    if (state.thinking.empty())
      return;
    auto thinking = render_visible_markdown("[thinking]\n" + state.thinking);
    auto thinking_lines = split_region_lines(thinking, width);
    lines.insert(lines.end(), std::make_move_iterator(thinking_lines.begin()),
                 std::make_move_iterator(thinking_lines.end()));
  };

  std::size_t tool_count = 0;
  for (const auto &block : state.blocks) {
    if (std::holds_alternative<RegionToolBlock>(block))
      ++tool_count;
  }
  std::size_t seen_tools = 0;
  const auto thinking_index =
      std::min(state.thinking_block_index, state.blocks.size());
  for (std::size_t block_index = 0; block_index < state.blocks.size();
       ++block_index) {
    if (block_index == thinking_index)
      append_thinking();
    const auto &block = state.blocks[block_index];
    if (const auto *text = std::get_if<RegionTextBlock>(&block)) {
      auto rendered = render_visible_markdown(text->raw);
      auto text_lines = split_region_lines(rendered, width);
      lines.insert(lines.end(), std::make_move_iterator(text_lines.begin()),
                   std::make_move_iterator(text_lines.end()));
      continue;
    }

    const auto &tool = std::get<RegionToolBlock>(block);
    const bool expanded =
        content_rows > 1 && seen_tools + kMaxExpandedToolRegions >= tool_count;
    ++seen_tools;
    auto rendered = tool_lines(tool, width, expanded, content_rows);
    lines.insert(lines.end(), std::make_move_iterator(rendered.begin()),
                 std::make_move_iterator(rendered.end()));
  }
  if (thinking_index == state.blocks.size())
    append_thinking();
  if (lines.empty())
    lines.emplace_back("\033[0m");
  return lines;
}

std::string request_source_label(RequestSource source) {
  switch (source) {
  case RequestSource::ordinary:
    return {};
  case RequestSource::mailbox:
    return "MAILBOX";
  case RequestSource::follow_up:
    return "FOLLOW-UP";
  }
  return {};
}

std::string request_sender_label(const RequestPresentation &metadata) {
  if (metadata.sender_task_path)
    return *metadata.sender_task_path;
  if (metadata.sender_session_name)
    return *metadata.sender_session_name;
  if (metadata.sender_agent_id) {
    constexpr std::size_t kMaxSenderIdLength = 24;
    const auto &id = *metadata.sender_agent_id;
    if (id.size() <= kMaxSenderIdLength)
      return id;
    constexpr std::size_t kSuffixLength = 5;
    constexpr std::size_t kPrefixLength =
        kMaxSenderIdLength - 3 - kSuffixLength;
    return id.substr(0, kPrefixLength) + "..." +
           id.substr(id.size() - kSuffixLength);
  }
  if (metadata.source == RequestSource::mailbox)
    return "unknown sender";
  return {};
}

std::string request_heading(const RegionRequestBlock &request) {
  std::string heading = "-- REQUEST";
  const auto source = request_source_label(request.metadata.source);
  if (!source.empty()) {
    heading += " | ";
    heading += source;
    const auto sender = request_sender_label(request.metadata);
    if (!sender.empty()) {
      heading += " | ";
      heading += sender;
    }
  }
  return heading;
}

void append_request_lines(std::vector<std::string> &lines,
                          const RegionTurn &turn, int width) {
  if (turn.requests.empty())
    return;
  const auto has_content = [](const RegionRequestBlock &request) {
    return !request.raw_text.empty() || request.non_text_attachments != 0 ||
           request.metadata.source != RequestSource::ordinary ||
           request.metadata.message_id.has_value() ||
           request.metadata.message_kind.has_value() ||
           request.metadata.sender_agent_id.has_value() ||
           request.metadata.sender_session_id.has_value() ||
           request.metadata.sender_task_path.has_value() ||
           request.metadata.sender_session_name.has_value();
  };
  if (std::none_of(turn.requests.begin(), turn.requests.end(), has_content))
    return;

  std::string heading =
      sanitize_tool_output(request_heading(turn.requests.front()));
  const auto first_source = turn.requests.front().metadata.source;
  const auto first_sender =
      request_sender_label(turn.requests.front().metadata);
  const bool mixed = std::any_of(
      turn.requests.begin() + 1, turn.requests.end(), [&](const auto &request) {
        return request.metadata.source != first_source ||
               request_sender_label(request.metadata) != first_sender;
      });
  if (mixed)
    heading = "-- REQUEST";
  auto heading_lines = split_region_lines(heading, width);
  lines.insert(lines.end(), std::make_move_iterator(heading_lines.begin()),
               std::make_move_iterator(heading_lines.end()));
  for (const auto &request : turn.requests) {
    std::string request_text = sanitize_tool_output(request.raw_text);
    for (std::size_t index = 0; index < request.non_text_attachments; ++index) {
      if (!request_text.empty())
        request_text.push_back('\n');
      request_text += "[image attachment]";
    }
    if (request_text.empty())
      continue;
    auto request_lines = split_region_lines(request_text, width);
    lines.insert(lines.end(), std::make_move_iterator(request_lines.begin()),
                 std::make_move_iterator(request_lines.end()));
  }
}

enum class RegionSection { none, provisional, work, answer, truncated };

RegionSection region_section(RegionAssistantTextKind kind) {
  switch (kind) {
  case RegionAssistantTextKind::provisional:
    return RegionSection::provisional;
  case RegionAssistantTextKind::work:
    return RegionSection::work;
  case RegionAssistantTextKind::answer:
    return RegionSection::answer;
  case RegionAssistantTextKind::answer_truncated:
    return RegionSection::truncated;
  }
  return RegionSection::provisional;
}

std::string_view region_section_heading(RegionSection section) {
  switch (section) {
  case RegionSection::provisional:
    return "\033[38;5;245mASSISTANT...\033[0m";
  case RegionSection::work:
    return "\033[38;5;245mWORK\033[0m";
  case RegionSection::answer:
    return "\033[1;97mANSWER\033[0m";
  case RegionSection::truncated:
    return "\033[1;93mANSWER | TRUNCATED\033[0m";
  case RegionSection::none:
    return {};
  }
  return {};
}

std::vector<std::string> turn_region_lines(const RegionState &state, int width,
                                           int content_rows) {
  std::vector<std::string> lines;
  if (width <= 0)
    return lines;

  std::size_t tool_count = 0;
  for (const auto &turn : state.turns) {
    for (const auto &block : turn.blocks) {
      if (std::holds_alternative<RegionToolBlock>(block))
        ++tool_count;
    }
  }

  std::size_t seen_tools = 0;
  for (const auto &turn : state.turns) {
    append_request_lines(lines, turn, width);
    RegionSection section = RegionSection::none;
    const auto append_section_heading = [&](RegionSection next) {
      if (next == section)
        return;
      section = next;
      auto heading_lines =
          split_region_lines(region_section_heading(next), width);
      lines.insert(lines.end(), std::make_move_iterator(heading_lines.begin()),
                   std::make_move_iterator(heading_lines.end()));
    };
    for (const auto &block : turn.blocks) {
      if (const auto *text = std::get_if<RegionTextBlock>(&block)) {
        append_section_heading(region_section(text->kind));
        auto rendered = render_visible_markdown(text->raw);
        auto text_lines = split_region_lines(rendered, width);
        lines.insert(lines.end(), std::make_move_iterator(text_lines.begin()),
                     std::make_move_iterator(text_lines.end()));
        continue;
      }
      if (const auto *thinking = std::get_if<RegionThinkingBlock>(&block)) {
        append_section_heading(RegionSection::work);
        auto rendered = render_visible_markdown("[thinking]\n" + thinking->raw);
        auto thinking_lines = split_region_lines(rendered, width);
        lines.insert(lines.end(),
                     std::make_move_iterator(thinking_lines.begin()),
                     std::make_move_iterator(thinking_lines.end()));
        continue;
      }

      if (const auto *reply = std::get_if<RegionReplyBlock>(&block)) {
        append_section_heading(RegionSection::none);
        auto heading = std::string("\033[1;96mREPLY -> ") +
                       sanitize_tool_output(reply->recipient_label) +
                       " queued\033[0m";
        auto reply_heading = split_region_lines(heading, width);
        lines.insert(lines.end(),
                     std::make_move_iterator(reply_heading.begin()),
                     std::make_move_iterator(reply_heading.end()));
        auto reply_text =
            split_region_lines(sanitize_tool_output(reply->raw_text), width);
        lines.insert(lines.end(), std::make_move_iterator(reply_text.begin()),
                     std::make_move_iterator(reply_text.end()));
        continue;
      }
      append_section_heading(RegionSection::work);
      const bool expanded = content_rows > 1 &&
                            seen_tools + kMaxExpandedToolRegions >= tool_count;
      ++seen_tools;
      const auto &tool = std::get<RegionToolBlock>(block);
      auto rendered = tool_lines(tool, width, expanded, content_rows);
      lines.insert(lines.end(), std::make_move_iterator(rendered.begin()),
                   std::make_move_iterator(rendered.end()));
    }
  }
  if (lines.empty())
    lines.emplace_back("\033[0m");
  return lines;
}

std::vector<std::string> all_region_lines(const RegionState &state, int width,
                                          int content_rows) {
  if (!state.turns.empty())
    return turn_region_lines(state, width, content_rows);
  return legacy_region_lines(state, width, content_rows);
}
std::string usage_line(const TokenUsage &usage) {
  if (usage.input == 0 && usage.output == 0 && usage.cache_read == 0)
    return {};
  return "in:" + std::to_string(usage.input) +
         " out:" + std::to_string(usage.output);
}

bool write_all(int fd, std::string_view data) {
  std::size_t written = 0;
  while (written < data.size()) {
    const auto chunk = data.substr(written);
    const auto count = ::write(fd, chunk.data(), chunk.size());
    if (count > 0) {
      written += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR)
      continue;
    return false;
  }
  return true;
}

std::string scroll_region_sequence(int height) {
  if (height < 3)
    return {};
  return "\033[1;" + std::to_string(height - 2) + "r";
}

class RegionRenderer final : public Renderer {
public:
  explicit RegionRenderer(int fd)
      : fd_(fd), alt_screen_(fd),
        last_resize_generation_(resize_generation()),
        paint_thread_([this](const std::stop_token &st) { paint_loop(st); }) {
    install_resize_handler();
    const auto scroll_region = scroll_region_sequence(term_height(fd_));
    write_all(fd_, scroll_region);
    // Anchor the cursor on the dedicated prompt row before the first turn
    // starts, matching the position on_turn_end() restores afterward.
    // Without this, the very first prompt is drawn at the top of the alt
    // screen instead of the bottom.
    position_prompt_cursor();
  }

  RegionRenderer(const RegionRenderer &) = delete;
  RegionRenderer &operator=(const RegionRenderer &) = delete;
  RegionRenderer(RegionRenderer &&) = delete;
  RegionRenderer &operator=(RegionRenderer &&) = delete;

  ~RegionRenderer() noexcept override {
    paint_thread_.request_stop();
    cv_.notify_all();
    if (paint_thread_.joinable())
      paint_thread_.join();
    alt_screen_.leave();
  }

  void on_turn_start() override {
    std::scoped_lock lock(mutex_);
    state_.turns.emplace_back();
    state_.active_turn_index = state_.turns.size() - 1;
    state_.has_active_turn = true;
    state_.tool_index.clear();
    state_.tool_addresses.clear();
    state_.thinking.clear();
    state_.thinking_block_index = state_.blocks.size();
    state_.in_thinking = false;
    state_.scroll_offset_rows = 0;
    state_.status_text.clear();
    state_.has_error = false;
    state_.last_usage = {};
    state_.start_new_text_block = true;
    state_.hide_cursor_on_frame = true;
    state_.revision = ++revision_;
    state_.dirty = true;
    turn_active_ = true;
    cv_.notify_one();
  }

  void on_request(const RendererRequest &request) override {
    std::scoped_lock lock(mutex_);
    if (!state_.has_active_turn ||
        state_.active_turn_index >= state_.turns.size())
      return;
    if (request.text.empty() && request.non_text_attachments == 0 &&
        request.presentation.source == RequestSource::ordinary &&
        !request.presentation.message_id &&
        !request.presentation.message_kind &&
        !request.presentation.sender_agent_id &&
        !request.presentation.sender_session_id &&
        !request.presentation.sender_task_path &&
        !request.presentation.sender_session_name)
      return;
    RegionRequestBlock block;
    block.metadata = request.presentation;
    block.raw_text = request.text;
    block.non_text_attachments = request.non_text_attachments;
    state_.turns[state_.active_turn_index].requests.push_back(std::move(block));
    state_.revision = ++revision_;
    mark_dirty_locked();
  }

  void on_text_delta(std::string_view delta) override {
    std::scoped_lock lock(mutex_);
    if (state_.has_active_turn) {
      auto &turn = state_.turns[state_.active_turn_index];
      if (state_.start_new_text_block || turn.blocks.empty() ||
          !std::holds_alternative<RegionTextBlock>(turn.blocks.back()))
        turn.blocks.emplace_back(RegionTextBlock{});
      state_.start_new_text_block = false;
      std::get<RegionTextBlock>(turn.blocks.back())
          .raw.append(delta.data(), delta.size());
    } else {
      if (state_.start_new_text_block || state_.blocks.empty() ||
          !std::holds_alternative<RegionTextBlock>(state_.blocks.back()))
        state_.blocks.emplace_back(RegionTextBlock{});
      state_.start_new_text_block = false;
      std::get<RegionTextBlock>(state_.blocks.back())
          .raw.append(delta.data(), delta.size());
    }
    state_.revision = ++revision_;
    mark_dirty_locked();
  }

  void on_thinking_start() override {
    std::scoped_lock lock(mutex_);
    if (state_.has_active_turn) {
      auto &turn = state_.turns[state_.active_turn_index];
      if (!turn.blocks.empty()) {
        if (auto *text = std::get_if<RegionTextBlock>(&turn.blocks.back()))
          if (text->kind == RegionAssistantTextKind::provisional)
            text->kind = RegionAssistantTextKind::work;
      }
      turn.blocks.emplace_back(RegionThinkingBlock{});
    }
    state_.thinking.clear();
    state_.in_thinking = true;
    state_.status_text = "[thinking\xE2\x80\xA6]";
    state_.revision = ++revision_;
    mark_dirty_locked();
  }

  void on_thinking_delta(std::string_view delta) override {
    std::scoped_lock lock(mutex_);
    if (state_.has_active_turn) {
      auto &turn = state_.turns[state_.active_turn_index];
      if (!turn.blocks.empty()) {
        if (auto *thinking =
                std::get_if<RegionThinkingBlock>(&turn.blocks.back()))
          thinking->raw.append(delta.data(), delta.size());
      }
    }
    state_.thinking.append(delta.data(), delta.size());
    state_.in_thinking = true;
    state_.status_text = "[thinking\xE2\x80\xA6]";
    state_.revision = ++revision_;
    mark_dirty_locked();
  }

  void on_thinking_end() override {
    std::scoped_lock lock(mutex_);
    state_.in_thinking = false;
    if (!state_.has_error)
      state_.status_text.clear();
    state_.revision = ++revision_;
    mark_dirty_locked();
  }

  void on_tool_start(std::string_view call_id, std::string_view tool_name,
                     std::string_view args_json) override {
    std::scoped_lock lock(mutex_);
    RegionToolBlock tool;
    tool.call_id.assign(call_id);
    tool.tool_name.assign(tool_name);
    tool.args_json.assign(args_json);
    if (state_.has_active_turn) {
      auto &turn = state_.turns[state_.active_turn_index];
      const auto block_index = turn.blocks.size();
      if (!turn.blocks.empty()) {
        if (auto *text = std::get_if<RegionTextBlock>(&turn.blocks.back()))
          if (text->kind == RegionAssistantTextKind::provisional)
            text->kind = RegionAssistantTextKind::work;
      }

      turn.blocks.emplace_back(std::move(tool));
      state_.tool_addresses[std::string(call_id)] =
          RegionToolAddress{state_.active_turn_index, block_index};
      state_.tool_index[std::string(call_id)] = block_index;
    } else {
      state_.tool_index[std::string(call_id)] = state_.blocks.size();
      state_.blocks.emplace_back(std::move(tool));
    }
    state_.revision = ++revision_;
    mark_dirty_locked();
  }

  void on_tool_update(std::string_view call_id, std::string_view,
                      std::string_view partial_result) override {
    std::scoped_lock lock(mutex_);
    const auto it = state_.tool_addresses.find(std::string(call_id));
    if (it != state_.tool_addresses.end() &&
        it->second.turn_index < state_.turns.size()) {
      auto &turn = state_.turns[it->second.turn_index];
      if (it->second.block_index < turn.blocks.size()) {
        auto *tool =
            std::get_if<RegionToolBlock>(&turn.blocks[it->second.block_index]);
        if (tool != nullptr)
          tool->raw_output.assign(partial_result);
      }
    } else {
      const auto legacy = state_.tool_index.find(std::string(call_id));
      if (legacy == state_.tool_index.end() ||
          legacy->second >= state_.blocks.size())
        return;
      auto *tool = std::get_if<RegionToolBlock>(&state_.blocks[legacy->second]);
      if (tool == nullptr)
        return;
      tool->raw_output.assign(partial_result);
    }
    state_.revision = ++revision_;
    mark_dirty_locked();
  }

  void on_tool_end(std::string_view call_id, std::string_view,
                   const ToolResult &result, bool is_error) override {
    std::scoped_lock lock(mutex_);
    const auto it = state_.tool_addresses.find(std::string(call_id));
    if (it != state_.tool_addresses.end() &&
        it->second.turn_index < state_.turns.size()) {
      auto &turn = state_.turns[it->second.turn_index];
      if (it->second.block_index < turn.blocks.size()) {
        auto *tool =
            std::get_if<RegionToolBlock>(&turn.blocks[it->second.block_index]);
        if (tool != nullptr) {
          tool->raw_output = result.content();
          tool->running = false;
          tool->is_error = is_error;
        }
      }
    } else {
      const auto legacy = state_.tool_index.find(std::string(call_id));
      if (legacy == state_.tool_index.end() ||
          legacy->second >= state_.blocks.size())
        return;
      auto *tool = std::get_if<RegionToolBlock>(&state_.blocks[legacy->second]);
      if (tool == nullptr)
        return;
      tool->raw_output = result.content();
      tool->running = false;
      tool->is_error = is_error;
    }
    state_.revision = ++revision_;
    mark_dirty_locked();
  }

  void
  on_mailbox_reply_queued(std::string_view call_id,
                          const MailboxReplyQueuedNotice &notice) override {
    std::scoped_lock lock(mutex_);
    if (!state_.has_active_turn ||
        state_.active_turn_index >= state_.turns.size())
      return;
    auto &turn = state_.turns[state_.active_turn_index];
    const auto recipient = notice.recipient_agent_id.value_or(
        notice.recipient_session_id.empty() ? "unknown recipient"
                                            : notice.recipient_session_id);
    turn.blocks.emplace_back(
        RegionReplyBlock{.request_message_id = notice.request_message_id,
                         .call_id = std::string(call_id),
                         .recipient_label = recipient,
                         .raw_text = notice.reply_text});
    state_.revision = ++revision_;
    mark_dirty_locked();
  }
  void on_tool_output_text(std::string_view call_id,
                           std::string_view text) override {
    std::scoped_lock lock(mutex_);
    const auto it = state_.tool_addresses.find(std::string(call_id));
    if (it != state_.tool_addresses.end() &&
        it->second.turn_index < state_.turns.size()) {
      auto &turn = state_.turns[it->second.turn_index];
      if (it->second.block_index < turn.blocks.size()) {
        auto *tool =
            std::get_if<RegionToolBlock>(&turn.blocks[it->second.block_index]);
        if (tool != nullptr) {
          if (tool->running)
            tool->custom_call_output.assign(text);
          else
            tool->custom_result_output.assign(text);
        }
      }
    } else {
      const auto legacy = state_.tool_index.find(std::string(call_id));
      if (legacy == state_.tool_index.end() ||
          legacy->second >= state_.blocks.size())
        return;
      auto *tool = std::get_if<RegionToolBlock>(&state_.blocks[legacy->second]);
      if (tool == nullptr)
        return;
      if (tool->running)
        tool->custom_call_output.assign(text);
      else
        tool->custom_result_output.assign(text);
    }
    state_.revision = ++revision_;
    mark_dirty_locked();
  }

  void on_message_end_presentation(const MessageEndPresentation &end) override {
    std::scoped_lock lock(mutex_);
    if (state_.has_active_turn &&
        state_.active_turn_index < state_.turns.size()) {
      auto &turn = state_.turns[state_.active_turn_index];
      RegionAssistantTextKind kind = RegionAssistantTextKind::work;
      if (end.stop_reason == StopReason::stop)
        kind = RegionAssistantTextKind::answer;
      else if (end.stop_reason == StopReason::length)
        kind = RegionAssistantTextKind::answer_truncated;
      if (!turn.blocks.empty()) {
        if (auto *text = std::get_if<RegionTextBlock>(&turn.blocks.back()))
          text->kind = kind;
        else if (end.stop_reason == StopReason::stop ||
                 end.stop_reason == StopReason::length)
          turn.blocks.emplace_back(RegionTextBlock{.kind = kind});
      } else if (end.stop_reason == StopReason::stop ||
                 end.stop_reason == StopReason::length) {
        turn.blocks.emplace_back(RegionTextBlock{.kind = kind});
      }
    }
    state_.last_usage = end.usage;
    state_.revision = ++revision_;
    mark_dirty_locked();
  }

  void on_message_end(const TokenUsage &usage) override {
    on_message_end_presentation(MessageEndPresentation{.usage = usage});
  }

  void on_command_output(std::string_view text) override {
    bool paint_now = false;
    {
      std::scoped_lock lock(mutex_);
      if (state_.has_active_turn &&
          state_.active_turn_index < state_.turns.size()) {
        auto &turn = state_.turns[state_.active_turn_index];
        if (turn.blocks.empty() ||
            !std::holds_alternative<RegionTextBlock>(turn.blocks.back()))
          turn.blocks.emplace_back(RegionTextBlock{});
        auto &block = std::get<RegionTextBlock>(turn.blocks.back()).raw;
        if (!block.empty() && !block.ends_with('\n'))
          block.push_back('\n');
        block.append(text.data(), text.size());
      } else {
        state_.turns.emplace_back();
        auto &turn = state_.turns.back();
        turn.complete = true;
        if (turn.blocks.empty() ||
            !std::holds_alternative<RegionTextBlock>(turn.blocks.back()))
          turn.blocks.emplace_back(RegionTextBlock{});
        auto &block = std::get<RegionTextBlock>(turn.blocks.back()).raw;
        if (!block.empty() && !block.ends_with('\n'))
          block.push_back('\n');
        block.append(text.data(), text.size());
      }
      state_.revision = ++revision_;
      mark_dirty_locked();
      paint_now = !turn_active_;
    }
    if (paint_now)
      paint_idle_synchronously();
  }

  void on_error(RendererErrorKind, std::string_view message) override {
    bool paint_now = false;
    {
      std::scoped_lock lock(mutex_);
      state_.status_text = "error: ";
      if (state_.has_active_turn &&
          state_.active_turn_index < state_.turns.size()) {
        auto &turn = state_.turns[state_.active_turn_index];
        if (!turn.blocks.empty()) {
          if (auto *text = std::get_if<RegionTextBlock>(&turn.blocks.back()))
            if (text->kind == RegionAssistantTextKind::provisional)
              text->kind = RegionAssistantTextKind::work;
        }
      }

      state_.status_text.append(message.data(), message.size());
      state_.has_error = true;
      state_.revision = ++revision_;
      mark_dirty_locked();
      paint_now = !turn_active_;
    }
    if (paint_now)
      paint_idle_synchronously();
  }

  void on_scroll(RendererScrollCommand command) override {
    bool paint_now = false;
    {
      std::scoped_lock lock(mutex_);
      const int page_rows = std::max(1, term_height(fd_) - 3);
      const auto add_scroll = [&](int amount) {
        if (state_.scroll_offset_rows >
            std::numeric_limits<int>::max() - amount) {
          state_.scroll_offset_rows = std::numeric_limits<int>::max();
        } else {
          state_.scroll_offset_rows += amount;
        }
      };
      switch (command) {
      case RendererScrollCommand::line_up:
        add_scroll(1);
        break;
      case RendererScrollCommand::line_down:
        state_.scroll_offset_rows = std::max(0, state_.scroll_offset_rows - 1);
        break;
      case RendererScrollCommand::page_up:
        add_scroll(page_rows);
        break;
      case RendererScrollCommand::page_down:
        state_.scroll_offset_rows =
            std::max(0, state_.scroll_offset_rows - page_rows);
        break;
      case RendererScrollCommand::top:
        state_.scroll_offset_rows = std::numeric_limits<int>::max();
        break;
      case RendererScrollCommand::bottom:
        state_.scroll_offset_rows = 0;
        break;
      }
      state_.revision = ++revision_;
      mark_dirty_locked();
      paint_now = !turn_active_;
    }
    if (paint_now)
      paint_idle_synchronously();
  }

  bool owns_status_line() const override { return true; }

  void set_status_line(const std::optional<std::string> &text) override {
    bool paint_now = false;
    {
      std::scoped_lock lock(mutex_);
      custom_status_line_ = text;
      state_.custom_status_line = text;
      state_.revision = ++revision_;
      mark_dirty_locked();
      paint_now = !turn_active_;
    }
    if (paint_now)
      paint_idle_synchronously();
  }

  void on_turn_end() override {
    State snapshot;
    {
      std::scoped_lock lock(mutex_);
      turn_active_ = false;
      if (state_.has_active_turn &&
          state_.active_turn_index < state_.turns.size()) {
        state_.turns[state_.active_turn_index].complete = true;
        state_.has_active_turn = false;
      }
      if (!state_.has_error) {
        state_.status_text =
            "tokens: " + std::to_string(state_.last_usage.output) + "  done";
      }
      state_.revision = ++revision_;
      state_.dirty = false;
      snapshot = state_;
    }
    try {
      render_frame(snapshot);
    } catch (...) { // NOLINT(bugprone-empty-catch)
      std::scoped_lock output_lock(paint_mutex_);
      last_frame_lines_.clear();
      // Keep terminal teardown available after malformed markdown.
    }
    position_prompt_cursor();
  }

  bool owns_tool_output() const override { return true; }

  // Between turns, the interactive loop calls this on the terminal resize
  // notification. Repaint the content/status panes at the new dimensions and
  // re-anchor the cursor on the (possibly moved) prompt row. Must only be
  // called while no turn is active — see Renderer::on_resize().
  void on_resize() override {
    bool paint_now = false;
    {
      std::scoped_lock lock(mutex_);
      last_resize_generation_ = resize_generation();
      state_.revision = ++revision_;
      mark_dirty_locked();
      paint_now = !turn_active_;
    }
    if (paint_now) {
      paint_idle_synchronously();
      position_prompt_cursor();
    }
  }

private:
  struct State : RegionState {
    bool dirty{true};
    bool start_new_text_block{true};
    bool hide_cursor_on_frame{true};
    std::uint64_t revision{0};
    std::string status_text;
    bool has_error{false};
    std::optional<std::string> custom_status_line;
    TokenUsage last_usage;
  };

  // EventStream serializes renderer callbacks on its consumer thread, even
  // when tool workers enqueue updates concurrently. The mutex only protects
  // callback-thread writes from the paint-thread snapshot and never guards
  // callback-to-callback races.
  void mark_dirty_locked() {
    state_.dirty = true;
    cv_.notify_one();
  }

  // Called with mutex_ held, from paint_loop only. Returns true if a resize
  // was newly observed (and marks state dirty).
  bool check_resize_locked() {
    const auto generation = resize_generation();
    if (generation == last_resize_generation_)
      return false;
    last_resize_generation_ = generation;
    state_.dirty = true;
    return true;
  }

  void paint_idle_synchronously() {
    State snapshot;
    {
      std::scoped_lock lock(mutex_);
      if (turn_active_)
        return;
      state_.dirty = false;
      snapshot = state_;
    }

    write_all(fd_, "\0337");
    try {
      render_frame(snapshot);
    } catch (...) { // NOLINT(bugprone-empty-catch)
      std::scoped_lock output_lock(paint_mutex_);
      last_frame_lines_.clear();
    }
    write_all(fd_, "\0338");
  }

  void paint_loop(const std::stop_token &stop) {
    auto next_frame = std::chrono::steady_clock::now() +
                      std::chrono::milliseconds(kFrameIntervalMs);
    while (!stop.stop_requested()) {
      State snapshot;
      {
        std::unique_lock lock(mutex_);
        // Wait for real work. While a turn is active but otherwise quiet
        // (e.g. a tool call or network request in flight with no streaming
        // output yet), wake periodically anyway so a terminal resize is
        // still noticed with nothing else to piggyback on. With no turn
        // active at all, wait with no deadline — an idle resize is instead
        // handled synchronously by on_resize(), since this background
        // thread must never write to fd_ concurrently with the foreground
        // readline prompt.
        while (!stop.stop_requested() && !(turn_active_ && state_.dirty)) {
          if (turn_active_ && check_resize_locked())
            break;
          if (turn_active_)
            cv_.wait_for(lock, std::chrono::milliseconds(100));
          else
            cv_.wait(lock,
                     [&] { return stop.stop_requested() || turn_active_; });
        }
        if (stop.stop_requested())
          return;

        // Updates arriving before the deadline are coalesced. Leaving this
        // wait when a turn ends returns to the loop above, while a still-
        // active dirty turn remains gated by next_frame.
        cv_.wait_until(lock, next_frame, [&] {
          if (turn_active_)
            check_resize_locked();
          return stop.stop_requested() || !turn_active_ || !state_.dirty;
        });
        if (stop.stop_requested())
          return;
        if (!turn_active_ || !state_.dirty)
          continue;
        state_.dirty = false;
        snapshot = state_;
      }

      try {
        render_frame(snapshot);
      } catch (...) { // A paint failure must not terminate the jthread.
        std::scoped_lock lock(paint_mutex_);
        last_frame_lines_.clear();
      }
      next_frame = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(kFrameIntervalMs);
    }
  }

  void render_frame(const State &snapshot) {
    std::scoped_lock output_lock(paint_mutex_);
    const int width = term_width(fd_);
    const int height = term_height(fd_);
    const int content_rows = std::max(0, height - 2);
    if (content_rows < 1)
      return;

    auto frame = build_region_frame(snapshot, width, content_rows);
    std::vector<std::string> rows = std::move(frame.lines);
    rows.resize(static_cast<std::size_t>(content_rows));

    int scroll_offset = 0;
    {
      std::scoped_lock lock(mutex_);
      if (snapshot.revision != state_.revision)
        return;
      state_.scroll_offset_rows =
          std::clamp(state_.scroll_offset_rows, 0, frame.max_scroll_rows);
      scroll_offset = state_.scroll_offset_rows;
    }

    const bool layout_changed = width != last_width_ || height != last_height_;
    if (layout_changed) {
      last_frame_lines_.clear();
      last_width_ = width;
      last_height_ = height;
    }

    std::string output;
    if (layout_changed)
      output += scroll_region_sequence(height);
    if (snapshot.hide_cursor_on_frame)
      output += "\033[?25l";
    output += diff_region_rows(last_frame_lines_, rows);
    output += status_sequence(snapshot, width, height, scroll_offset,
                              frame.max_scroll_rows);
    if (!output.empty() && !write_all(fd_, output)) {
      last_frame_lines_.clear();
      return;
    }
    last_frame_lines_ = std::move(rows);
    if (snapshot.hide_cursor_on_frame) {
      std::scoped_lock lock(mutex_);
      if (state_.revision == snapshot.revision)
        state_.hide_cursor_on_frame = false;
    }
  }

  // Readline's prompt starts with a newline. Leave the cursor on the status
  // row so that newline advances to the dedicated prompt row at the very
  // bottom of the terminal.
  void position_prompt_cursor() const {
    const int prompt_anchor = std::max(1, term_height(fd_) - 1);
    const auto cursor_sequence =
        "\033[" + std::to_string(prompt_anchor) + ";1H\033[?25h";
    write_all(fd_, cursor_sequence);
  }

  static std::string status_sequence(const State &snapshot, int width,
                                     int height, int scroll, int max_scroll) {
    if (height < 2 || width < 1)
      return {};

    std::string left;
    if (!snapshot.status_text.empty()) {
      left = snapshot.status_text;
    } else if (snapshot.custom_status_line) {
      left = *snapshot.custom_status_line;
    } else {
      bool first = true;
      const auto append_tools = [&first, &left](const auto &blocks) {
        for (const auto &block : blocks) {
          const auto *tool = std::get_if<RegionToolBlock>(&block);
          if (tool == nullptr || !tool->running)
            continue;
          if (first)
            left = "[";
          else
            left += ", ";
          left += tool->tool_name;
          first = false;
        }
      };
      if (!snapshot.turns.empty()) {
        for (const auto &turn : snapshot.turns)
          append_tools(turn.blocks);
      } else {
        append_tools(snapshot.blocks);
      }
      if (!first)
        left += "]";
    }
    if (scroll > 0) {
      if (!left.empty())
        left += "  ";
      left +=
          "scroll " + std::to_string(scroll) + "/" + std::to_string(max_scroll);
    }
    left = truncate_ansi_line(left, width);

    auto usage = usage_line(snapshot.last_usage);
    const int gap = width - display_columns(left) - display_columns(usage);
    if (usage.empty() || gap < 1)
      usage.clear();

    std::string bar =
        "\033[" + std::to_string(height - 1) + ";1H\033[2K\033[2m";
    bar += left;
    if (!usage.empty()) {
      bar.append(static_cast<std::size_t>(gap), ' ');
      bar += usage;
    }
    bar += "\033[0m";
    return bar;
  }

  int fd_;
  std::mutex mutex_;
  State state_;
  std::condition_variable cv_;
  bool turn_active_{false};
  std::uint64_t revision_{0};
  std::optional<std::string> custom_status_line_;
  AltScreenSession alt_screen_;
  std::mutex paint_mutex_;
  std::vector<std::string> last_frame_lines_;
  int last_width_{0};
  int last_height_{0};
  int last_resize_generation_{0};
  std::jthread paint_thread_;
};

} // namespace

RegionFrame build_region_frame(const RegionState &state, int width,
                               int content_rows) {
  if (content_rows < 1 || width < 1)
    return {};

  auto all_lines = all_region_lines(state, width, content_rows);
  const int total_rows = static_cast<int>(all_lines.size());
  const int max_scroll = std::max(0, total_rows - content_rows);
  const int scroll = std::clamp(state.scroll_offset_rows, 0, max_scroll);
  const int first = std::max(0, total_rows - content_rows - scroll);
  const int last = std::min(total_rows, first + content_rows);

  RegionFrame frame;
  frame.max_scroll_rows = max_scroll;
  frame.total_rows = total_rows;
  frame.lines.assign(all_lines.begin() + first, all_lines.begin() + last);
  return frame;
}

std::string diff_region_rows(const std::vector<std::string> &old_rows,
                             const std::vector<std::string> &new_rows) {
  std::string output;
  std::vector<std::string> shifted_rows = old_rows;
  bool shifted = false;
  if (old_rows.size() == new_rows.size() && old_rows.size() > 1) {
    for (std::size_t shift = 1; shift < old_rows.size(); ++shift) {
      if (std::equal(old_rows.begin() + static_cast<std::ptrdiff_t>(shift),
                     old_rows.end(), new_rows.begin())) {
        output = "\033[1;1H\033[" + std::to_string(shift) + "S";
        shifted_rows.erase(shifted_rows.begin(),
                           shifted_rows.begin() +
                               static_cast<std::ptrdiff_t>(shift));
        shifted_rows.resize(new_rows.size());
        shifted = true;
        break;
      }
    }
    for (std::size_t shift = 1; !shifted && shift < old_rows.size(); ++shift) {
      if (std::equal(old_rows.begin(),
                     old_rows.end() - static_cast<std::ptrdiff_t>(shift),
                     new_rows.begin() + static_cast<std::ptrdiff_t>(shift))) {
        output = "\033[1;1H\033[" + std::to_string(shift) + "T";
        shifted_rows.insert(shifted_rows.begin(), shift, std::string{});
        shifted_rows.resize(new_rows.size());
        break;
      }
    }
  }
  for (std::size_t i = 0; i < new_rows.size(); ++i) {
    if (i < shifted_rows.size() && shifted_rows[i] == new_rows[i])
      continue;
    output += "\033[" + std::to_string(i + 1) + ";1H\033[2K";
    output += new_rows[i];
  }
  return output;
}

std::unique_ptr<Renderer> make_region_renderer(int fd) {
  return std::make_unique<RegionRenderer>(fd);
}

} // namespace pi::core
