#include "core/providers/transform_messages.h"
#include "core/message_types.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pi::core {

namespace {

constexpr std::string_view USER_IMAGE_PLACEHOLDER =
    "(image omitted: model does not support images)";
constexpr std::string_view TOOL_IMAGE_PLACEHOLDER =
    "(tool image omitted: model does not support images)";

bool model_supports_images(const Model &model) {
  return std::ranges::find(model.input_capabilities, std::string("image")) !=
         model.input_capabilities.end();
}

std::vector<ContentBlock>
replace_images(const std::vector<ContentBlock> &content,
               std::string_view placeholder) {
  std::vector<ContentBlock> result;
  bool prev_was_placeholder = false;
  for (const auto &block : content) {
    if (std::holds_alternative<ImageContent>(block)) {
      if (!prev_was_placeholder) {
        result.emplace_back(TextContent{.text = std::string(placeholder)});
      }
      prev_was_placeholder = true;
      continue;
    }
    if (const auto *tc = std::get_if<TextContent>(&block)) {
      prev_was_placeholder = (tc->text == placeholder);
    } else {
      prev_was_placeholder = false;
    }
    result.push_back(block);
  }
  return result;
}

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

} // namespace

std::vector<Message>
transform_messages(const std::vector<Message> &messages, const Model &model,
                   const std::function<std::string(const std::string &id)>
                       &normalize_tool_call_id) {

  const bool has_images = model_supports_images(model);

  auto downgrade = [&](const std::vector<ContentBlock> &content,
                       std::string_view ph) -> std::vector<ContentBlock> {
    if (has_images)
      return content;
    return replace_images(content, ph);
  };

  std::map<std::string, std::string> tool_call_id_map;

  std::vector<Message> first_pass;
  first_pass.reserve(messages.size());

  for (const auto &msg : messages) {
    if (std::holds_alternative<UserMessage>(msg)) {
      const auto &um = std::get<UserMessage>(msg);
      if (has_images) {
        first_pass.emplace_back(um);
      } else {
        UserMessage copy = um;
        copy.content = downgrade(um.content, USER_IMAGE_PLACEHOLDER);
        first_pass.emplace_back(std::move(copy));
      }
    } else if (std::holds_alternative<ToolResultMessage>(msg)) {
      const auto &trm = std::get<ToolResultMessage>(msg);
      ToolResultMessage copy = trm;
      if (!has_images) {
        copy.content = downgrade(trm.content, TOOL_IMAGE_PLACEHOLDER);
      }
      auto it = tool_call_id_map.find(copy.tool_call_id);
      if (it != tool_call_id_map.end() && it->second != copy.tool_call_id) {
        copy.tool_call_id = it->second;
      }
      first_pass.emplace_back(std::move(copy));
    } else if (std::holds_alternative<AssistantMessage>(msg)) {
      const auto &am = std::get<AssistantMessage>(msg);
      const bool same_model = am.provider == model.provider &&
                              am.api == model.api && am.model == model.id;

      std::vector<ContentBlock> transformed_content;
      for (const auto &block : am.content) {
        if (const auto *tc = std::get_if<ThinkingContent>(&block)) {
          if (tc->redacted) {
            if (same_model)
              transformed_content.emplace_back(*tc);
            continue;
          }
          if (same_model && tc->thinking_signature.has_value()) {
            transformed_content.emplace_back(*tc);
            continue;
          }
          if (tc->thinking.empty() ||
              tc->thinking.find_first_not_of(" \t\n\r") == std::string::npos) {
            continue;
          }
          if (same_model) {
            transformed_content.emplace_back(*tc);
          } else {
            transformed_content.emplace_back(TextContent{.text = tc->thinking});
          }
        } else if (const auto *text = std::get_if<TextContent>(&block)) {
          if (same_model) {
            transformed_content.emplace_back(*text);
          } else {
            transformed_content.emplace_back(TextContent{.text = text->text});
          }
        } else if (const auto *tool_call = std::get_if<ToolCall>(&block)) {
          ToolCall tc_copy = *tool_call;
          if (!same_model && normalize_tool_call_id) {
            std::string normalized = normalize_tool_call_id(tc_copy.id);
            if (normalized != tc_copy.id) {
              tool_call_id_map[tc_copy.id] = normalized;
              tc_copy.id = std::move(normalized);
            }
          }
          transformed_content.emplace_back(std::move(tc_copy));
        } else {
          transformed_content.push_back(block);
        }
      }

      AssistantMessage copy = am;
      copy.content = std::move(transformed_content);
      first_pass.emplace_back(std::move(copy));
    } else {
      first_pass.push_back(msg);
    }
  }

  std::vector<Message> result;
  result.reserve(first_pass.size());

  std::vector<ToolCall> pending_tool_calls;
  std::set<std::string> existing_tool_result_ids;

  auto insert_synthetic = [&]() {
    if (pending_tool_calls.empty())
      return;
    for (const auto &tc : pending_tool_calls) {
      if (!existing_tool_result_ids.contains(tc.id)) {
        ToolResultMessage synthetic;
        synthetic.tool_call_id = tc.id;
        synthetic.tool_name = tc.name;
        synthetic.content = {TextContent{.text = "No result provided"}};
        synthetic.is_error = true;
        synthetic.timestamp = now_ms();
        result.emplace_back(std::move(synthetic));
      }
    }
    pending_tool_calls.clear();
    existing_tool_result_ids.clear();
  };

  for (const auto &msg : first_pass) {
    if (std::holds_alternative<AssistantMessage>(msg)) {
      insert_synthetic();
      const auto &am = std::get<AssistantMessage>(msg);
      if (am.stop_reason == StopReason::error ||
          am.stop_reason == StopReason::aborted) {
        continue;
      }
      pending_tool_calls.clear();
      existing_tool_result_ids.clear();
      for (const auto &block : am.content) {
        if (const auto *tc = std::get_if<ToolCall>(&block)) {
          pending_tool_calls.push_back(*tc);
        }
      }
      result.push_back(msg);
    } else if (std::holds_alternative<ToolResultMessage>(msg)) {
      const auto &trm = std::get<ToolResultMessage>(msg);
      existing_tool_result_ids.insert(trm.tool_call_id);
      result.push_back(msg);
    } else if (std::holds_alternative<UserMessage>(msg)) {
      insert_synthetic();
      result.push_back(msg);
    } else {
      result.push_back(msg);
    }
  }

  insert_synthetic();

  return result;
}

} // namespace pi::core
