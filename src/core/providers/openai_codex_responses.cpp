#include "core/providers/openai_codex_responses.h"

#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/providers/transform_messages.h"
#include "http/http_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace pi::core {

namespace {

using Json = nlohmann::json;

std::int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string user_agent() {
#if defined(__APPLE__)
  constexpr std::string_view platform = "macOS";
#elif defined(__linux__)
  constexpr std::string_view platform = "Linux";
#else
  constexpr std::string_view platform = "unknown";
#endif
#if defined(__aarch64__) || defined(__arm64__)
  constexpr std::string_view architecture = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
  constexpr std::string_view architecture = "x86_64";
#else
  constexpr std::string_view architecture = "unknown";
#endif
  return "pici/" PI_CPP_VERSION " (" + std::string(platform) + "; " +
         std::string(architecture) + ")";
}

std::string safe_text(const Json &value, std::string fallback = {}) {
  std::string text;
  if (value.is_string())
    text = value.get<std::string>();
  else if (value.is_object())
    text = value.value("message", value.value("code", fallback));
  else
    return fallback;
  if (text.empty())
    return fallback;
  if (text.size() > 1024)
    text.resize(1024);
  return text;
}

std::string normalize_id(std::string_view id, std::size_t limit = 64) {
  std::string result;
  result.reserve(std::min(id.size(), limit));
  for (const unsigned char c : id) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_' || c == '-')
      result.push_back(static_cast<char>(c));
    if (result.size() == limit)
      break;
  }
  return result;
}

std::pair<std::string, std::string> split_tool_id(std::string_view id) {
  const auto separator = id.find('|');
  if (separator == std::string_view::npos)
    return {normalize_id(id), {}};
  return {normalize_id(id.substr(0, separator)),
          normalize_id(id.substr(separator + 1))};
}

std::string signature_for_item(const Json &item) { return item.dump(); }

std::optional<Json> parse_signature(std::string_view signature) {
  auto parsed = Json::parse(signature, nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object() ||
      (parsed.value("v", 0) != 1 && !parsed.contains("type")))
    return std::nullopt;
  if (parsed.value("v", 0) == 1 && parsed.contains("item") &&
      parsed["item"].is_object())
    return parsed["item"];
  return parsed;
}

std::string text_signature_for_item(const Json &item) {
  Json signature = {{"v", 1}, {"id", item.value("id", "")}};
  if (item.contains("phase") && item["phase"].is_string())
    signature["phase"] = item["phase"];
  return signature.dump();
}

void add_text_content(Json &content, std::string_view type,
                      std::string_view text, std::string_view id = {}) {
  content.push_back(
      {{"type", type}, {"text", text}, {"annotations", Json::array()}});
  (void)id;
}

Json tool_schema(const ToolDefinition &tool) {
  auto parsed = Json::parse(tool.schema().serialize(), nullptr, false);
  if (parsed.is_discarded() || !parsed.is_object())
    throw std::runtime_error("invalid schema for tool " +
                             std::string(tool.name()));
  return parsed;
}

Json convert_input(const Model &model, const AgentContext &context) {
  auto transformed =
      transform_messages(context.messages, model, [](const std::string &id) {
        auto [call_id, item_id] = split_tool_id(id);
        return item_id.empty() ? call_id : call_id + "|" + item_id;
      });
  Json input = Json::array();
  for (const auto &message : transformed) {
    if (const auto *user = std::get_if<UserMessage>(&message)) {
      Json content = Json::array();
      for (const auto &block : user->content) {
        if (const auto *text = std::get_if<TextContent>(&block)) {
          content.push_back({{"type", "input_text"}, {"text", text->text}});
        } else if (const auto *image = std::get_if<ImageContent>(&block)) {
          content.push_back({{"type", "input_image"},
                             {"image_url", "data:" + image->mime_type +
                                               ";base64," + image->data},
                             {"detail", "auto"}});
        }
      }
      if (!content.empty())
        input.push_back({{"type", "message"},
                         {"role", "user"},
                         {"content", std::move(content)}});
      continue;
    }
    if (const auto *assistant = std::get_if<AssistantMessage>(&message)) {
      for (const auto &block : assistant->content) {
        if (const auto *text = std::get_if<TextContent>(&block)) {
          Json item = {{"type", "message"},
                       {"role", "assistant"},
                       {"status", "completed"},
                       {"content", Json::array()}};
          if (auto signature =
                  parse_signature(text->text_signature.value_or(""))) {
            item["id"] = (*signature).value("id", "");
            if ((*signature).contains("phase") &&
                (*signature)["phase"].is_string())
              item["phase"] = (*signature)["phase"];
          }
          add_text_content(item["content"], "output_text", text->text);
          input.push_back(std::move(item));
        } else if (const auto *thinking =
                       std::get_if<ThinkingContent>(&block)) {
          if (thinking->thinking_signature) {
            if (auto item = parse_signature(*thinking->thinking_signature))
              input.push_back(std::move(*item));
          }
        } else if (const auto *call = std::get_if<ToolCall>(&block)) {
          auto [call_id, item_id] = split_tool_id(call->id);
          if (call_id.empty())
            call_id = "call_" + normalize_id(call->name);
          Json item = {{"type", "function_call"},
                       {"call_id", call_id},
                       {"name", call->name},
                       {"arguments", call->arguments.dump()}};
          if (!item_id.empty())
            item["id"] = item_id.starts_with("fc_") ? item_id : "fc_" + item_id;
          input.push_back(std::move(item));
        }
      }
      continue;
    }
    if (const auto *compaction =
            std::get_if<ContextCompactionMessage>(&message)) {
      // transform_messages already dropped this item unless it was produced
      // by this exact api/provider/model, so it is always safe to forward
      // here.
      Json item = {{"type", "compaction"},
                   {"encrypted_content", compaction->encrypted_content}};
      if (compaction->item_id && !compaction->item_id->empty())
        item["id"] = *compaction->item_id;
      input.push_back(std::move(item));
      continue;
    }
    if (const auto *tool = std::get_if<ToolResultMessage>(&message)) {
      std::string output;
      for (const auto &block : tool->content) {
        if (const auto *text = std::get_if<TextContent>(&block)) {
          if (!output.empty())
            output.push_back('\n');
          output += text->text;
        } else if (const auto *image = std::get_if<ImageContent>(&block)) {
          if (!output.empty())
            output.push_back('\n');
          output += "[image data omitted]";
          (void)image;
        }
      }
      if (output.empty())
        output = "(no tool output)";
      auto [call_id, unused_item_id] = split_tool_id(tool->tool_call_id);
      (void)unused_item_id;
      input.push_back({{"type", "function_call_output"},
                       {"call_id", call_id},
                       {"output", output}});
    }
  }
  return input;
}

// Codex scales the compact request's timeout relative to the normal idle
// timeout (COMPACT_REQUEST_TIMEOUT_IDLE_MULTIPLIER = 4 in the reference
// client.rs) rather than inventing a new constant. Pici's HttpClient
// timeout is already a total-duration timeout (CURLOPT_TIMEOUT_MS), not an
// idle one, but the same multiplier is applied for the same reason Codex
// gives: a unary request has no incremental progress signal to reset an
// idle clock against, so it needs more total headroom than a streaming
// request configured for the same nominal timeout.
constexpr std::uint32_t kCompactTimeoutIdleMultiplier = 4;
// Mirrors the hardcoded fallback in HttpClient's post_* helpers
// (src/http/http_client.cpp) so an unset timeout scales consistently
// instead of silently falling back to the unscaled default.
constexpr std::uint32_t kDefaultRequestTimeoutMs = 600000;

std::uint32_t
compact_request_timeout_ms(std::optional<std::uint32_t> configured) {
  const auto base =
      static_cast<std::uint64_t>(configured.value_or(kDefaultRequestTimeoutMs));
  const auto scaled =
      base * static_cast<std::uint64_t>(kCompactTimeoutIdleMultiplier);
  constexpr auto max_u32 =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());
  return static_cast<std::uint32_t>(std::min(scaled, max_u32));
}

// Shared by build_request_json and build_compact_request_json so the two
// request builders cannot drift on how a model's thinking level maps to its
// provider-specific reasoning effort string.
std::string resolve_reasoning_effort(const Model &model, ThinkingLevel level) {
  auto effort = std::string(thinking_level_to_string(level));
  if (auto it = model.thinking_level_map.find(effort);
      it != model.thinking_level_map.end() && it->second.has_value()) {
    effort = *it->second;
  }
  return effort;
}

} // namespace

std::string OpenAICodexResponsesClient::endpoint_url(std::string base_url) {
  while (!base_url.empty() && base_url.back() == '/')
    base_url.pop_back();
  if (base_url.ends_with("/codex/responses"))
    return base_url;
  if (base_url.ends_with("/codex"))
    return base_url + "/responses";
  if (base_url.empty())
    base_url = "https://chatgpt.com/backend-api";
  return base_url + "/codex/responses";
}

std::string
OpenAICodexResponsesClient::compact_endpoint_url(std::string base_url) {
  while (!base_url.empty() && base_url.back() == '/')
    base_url.pop_back();
  // Guard against double-appending if a caller already passed the compact
  // URL itself (checked before endpoint_url normalization, which does not
  // recognize a "/compact" suffix and would otherwise append a second
  // "/codex/responses").
  if (base_url.ends_with("/responses/compact"))
    return base_url;
  return endpoint_url(std::move(base_url)) + "/compact";
}

std::uint32_t OpenAICodexResponsesClient::compact_request_timeout_ms(
    std::optional<std::uint32_t> configured) {
  return ::pi::core::compact_request_timeout_ms(configured);
}

nlohmann::json
OpenAICodexResponsesClient::build_request_json(const Model &model,
                                               const AgentContext &context,
                                               const StreamOptions &options) {
  Json request = {
      {"model", model.id},
      {"store", false},
      {"stream", true},
      {"instructions", context.system_prompt.empty()
                           ? "You are a helpful assistant."
                           : context.system_prompt},
      {"input", convert_input(model, context)},
      {"text", {{"verbosity", "low"}}},
      {"include", {"reasoning.encrypted_content"}},
      {"tool_choice", "auto"},
      {"parallel_tool_calls", true},
  };
  if (options.session_id) {
    request["prompt_cache_key"] = *options.session_id;
  }
  if (options.temperature)
    request["temperature"] = *options.temperature;
  if (model.reasoning && options.reasoning != ThinkingLevel::off) {
    auto effort = resolve_reasoning_effort(model, options.reasoning);
    request["reasoning"] = {{"effort", effort}, {"summary", "auto"}};
  }
  if (!context.tools.empty()) {
    request["tools"] = Json::array();
    for (const auto &tool : context.tools) {
      request["tools"].push_back({{"type", "function"},
                                  {"name", tool->name()},
                                  {"description", tool->description()},
                                  {"parameters", tool_schema(*tool)},
                                  {"strict", false}});
    }
  }
  if (options.on_payload) {
    auto next = options.on_payload(request, model);
    if (next) {
      if (!next->is_object())
        throw std::runtime_error(
            "openai-codex payload hook must return an object");
      request = *next;
    }
  }
  return request;
}

// Mirrors the reference client's ApiCompactionInput: model, input,
// instructions, tools, parallel_tool_calls, reasoning, prompt_cache_key,
// text. Unlike build_request_json, this omits "stream"/"store"/"include"/
// "tool_choice" — the compact endpoint is unary JSON, not SSE, and does not
// document those fields.
nlohmann::json OpenAICodexResponsesClient::build_compact_request_json(
    const Model &model, const AgentContext &context,
    const CompactionOptions &options) {
  Json request = {
      {"model", model.id},
      {"instructions", context.system_prompt.empty()
                           ? "You are a helpful assistant."
                           : context.system_prompt},
      {"input", convert_input(model, context)},
      {"text", {{"verbosity", "low"}}},
      {"parallel_tool_calls", true},
  };
  if (options.session_id) {
    request["prompt_cache_key"] = *options.session_id;
  }
  if (model.reasoning && options.reasoning != ThinkingLevel::off) {
    auto effort = resolve_reasoning_effort(model, options.reasoning);
    request["reasoning"] = {{"effort", effort}, {"summary", "auto"}};
  }
  if (!context.tools.empty()) {
    request["tools"] = Json::array();
    for (const auto &tool : context.tools) {
      request["tools"].push_back({{"type", "function"},
                                  {"name", tool->name()},
                                  {"description", tool->description()},
                                  {"parameters", tool_schema(*tool)},
                                  {"strict", false}});
    }
  }
  if (options.on_payload) {
    auto next = options.on_payload(request, model);
    if (next) {
      if (!next->is_object())
        throw std::runtime_error(
            "openai-codex compact payload hook must return an object");
      request = *next;
    }
  }
  return request;
}

OpenAICodexResponsesParser::OpenAICodexResponsesParser(
    const Model &model, AssistantEventCallback on_event)
    : model_(model), on_event_(std::move(on_event)),
      result_(std::make_shared<AssistantMessage>()) {
  result_->api = model.api;
  result_->provider = model.provider;
  result_->model = model.id;
  result_->stop_reason = StopReason::stop;
  result_->timestamp = now_ms();
  if (on_event_)
    on_event_(AssistantMessageStartEvent{*result_});
}

void OpenAICodexResponsesParser::fail(std::string message) {
  if (failed_)
    return;
  failed_ = true;
  error_ = std::move(message);
  result_->stop_reason = StopReason::error;
  result_->error_message = error_;
  emit_error();
}

void OpenAICodexResponsesParser::emit_error() {
  if (emitted_error_ || !on_event_)
    return;
  emitted_error_ = true;
  on_event_(AssistantMessageErrorEvent{.reason = result_->stop_reason,
                                       .error = *result_});
}

void OpenAICodexResponsesParser::parse_usage(const Json &usage) {
  if (!usage.is_object())
    return;
  result_->usage.input = usage.value("input_tokens", std::uint64_t{0});
  result_->usage.output = usage.value("output_tokens", std::uint64_t{0});
  result_->usage.total_tokens =
      usage.value("total_tokens", result_->usage.input + result_->usage.output);
  if (auto details = usage.find("input_tokens_details");
      details != usage.end() && details->is_object()) {
    result_->usage.cache_read =
        details->value("cached_tokens", std::uint64_t{0});
    result_->usage.cache_write =
        details->value("cache_write_tokens", std::uint64_t{0});
  }
  const auto cached = result_->usage.cache_read + result_->usage.cache_write;
  result_->usage.input = result_->usage.input >= cached
                             ? result_->usage.input - cached
                             : std::uint64_t{0};
}

void OpenAICodexResponsesParser::reconcile_text(Slot &slot, std::string text) {
  auto &content = result_->content[slot.content_index];
  auto *current = std::get_if<TextContent>(&content);
  if (current == nullptr)
    return;
  if (text.starts_with(current->text)) {
    const auto suffix = text.substr(current->text.size());
    if (!suffix.empty() && on_event_)
      on_event_(
          AssistantMessageTextDeltaEvent{.content_index = slot.content_index,
                                         .delta = suffix,
                                         .partial = *result_});
  }
  current->text = std::move(text);
}

void OpenAICodexResponsesParser::close_slot(Slot &slot, const Json *item) {
  if (!slot.open)
    return;
  if (item != nullptr && item->is_object()) {
    if (slot.type == "message") {
      std::string text;
      if (auto content = item->find("content");
          content != item->end() && content->is_array()) {
        for (const auto &part : *content)
          if (part.value("type", "") == "output_text")
            text += part.value("text", "");
      }
      reconcile_text(slot, std::move(text));
      if (auto *current =
              std::get_if<TextContent>(&result_->content[slot.content_index]))
        current->text_signature = text_signature_for_item(*item);
    } else if (slot.type == "reasoning") {
      if (auto summary = item->find("summary");
          summary != item->end() && summary->is_array()) {
        std::string text;
        for (const auto &part : *summary) {
          if (!text.empty())
            text += "\n\n";
          text += part.value("text", "");
        }
        if (!text.empty())
          std::get<ThinkingContent>(result_->content[slot.content_index])
              .thinking = std::move(text);
      }
      if (auto *current = std::get_if<ThinkingContent>(
              &result_->content[slot.content_index]))
        current->thinking_signature = signature_for_item(*item);
    } else if (slot.type == "function_call") {
      const auto args = item->value("arguments", slot.partial_arguments);
      auto parsed = Json::parse(args, nullptr, false);
      auto &call = std::get<ToolCall>(result_->content[slot.content_index]);
      call.arguments = parsed.is_discarded() || !parsed.is_object()
                           ? Json::object()
                           : parsed;
      call.partial_json.clear();
    }
  }
  if (slot.type == "message") {
    if (auto *current =
            std::get_if<TextContent>(&result_->content[slot.content_index]);
        current != nullptr && on_event_)
      on_event_(
          AssistantMessageTextEndEvent{.content_index = slot.content_index,
                                       .content = current->text,
                                       .partial = *result_});
  } else if (slot.type == "reasoning") {
    if (auto *current =
            std::get_if<ThinkingContent>(&result_->content[slot.content_index]);
        current != nullptr && on_event_)
      on_event_(
          AssistantMessageThinkingEndEvent{.content_index = slot.content_index,
                                           .content = current->thinking,
                                           .partial = *result_});
  } else if (slot.type == "function_call") {
    if (on_event_)
      on_event_(AssistantMessageToolCallEndEvent{
          .content_index = slot.content_index,
          .tool_call = std::get<ToolCall>(result_->content[slot.content_index]),
          .partial = *result_});
  }
  slot.open = false;
}

// NOLINTNEXTLINE(misc-no-recursion): a terminal item can synthesize its
// missing added event before using the same slot handling path.
void OpenAICodexResponsesParser::process_event(std::string_view event_name,
                                               const Json &event) {
  if (!event.is_object() || failed_)
    return;
  const auto type =
      event_name.empty() ? event.value("type", "") : std::string(event_name);
  if (event.contains("error")) {
    fail(safe_text(event.value("error", Json::object()),
                   "OpenAI response error"));
    return;
  }
  if (type.empty())
    return;
  const Json *response = &event;
  if (auto it = event.find("response"); it != event.end() && it->is_object())
    response = &*it;
  if (type == "response.created") {
    result_->response_id = response->value("id", "");
    return;
  }
  if (type == "response.output_item.added") {
    const auto index = event.value("output_index", 0);
    const auto item = event.value("item", Json::object());
    const auto item_type = item.value("type", "");
    Slot slot{.content_index = result_->content.size(),
              .type = item_type,
              .item_id = item.value("id", ""),
              .call_id = item.value("call_id", ""),
              .partial_arguments = item.value("arguments", ""),
              .open = true};
    if (item_type == "message") {
      result_->content.emplace_back(TextContent{});
      if (on_event_)
        on_event_(AssistantMessageTextStartEvent{
            .content_index = slot.content_index, .partial = *result_});
    } else if (item_type == "reasoning") {
      result_->content.emplace_back(ThinkingContent{});
      if (on_event_)
        on_event_(AssistantMessageThinkingStartEvent{
            .content_index = slot.content_index, .partial = *result_});
    } else if (item_type == "function_call") {
      std::string item_id;
      if (!slot.item_id.empty()) {
        item_id = slot.item_id.starts_with("fc_") ? slot.item_id
                                                  : "fc_" + slot.item_id;
      }
      ToolCall call{.id = slot.call_id.empty() ? item_id : slot.call_id,
                    .name = item.value("name", "")};
      if (!item_id.empty() && !slot.call_id.empty())
        call.id += "|" + item_id;
      result_->content.emplace_back(std::move(call));
      std::get<ToolCall>(result_->content.back()).partial_json =
          slot.partial_arguments;
      if (on_event_)
        on_event_(AssistantMessageToolCallStartEvent{
            .content_index = slot.content_index, .partial = *result_});
    } else {
      return;
    }
    slots_[index] = std::move(slot);
    if (!slots_[index].item_id.empty())
      item_slots_[slots_[index].item_id] = index;
    return;
  }
  auto slot_for = [&](const Json &source) -> Slot * {
    const auto item_id = source.value("item_id", "");
    if (!item_id.empty()) {
      if (auto it = item_slots_.find(item_id); it != item_slots_.end())
        return &slots_[it->second];
    }
    return &slots_[source.value("output_index", 0)];
  };
  if (type == "response.output_text.delta" ||
      type == "response.refusal.delta" ||
      type == "response.reasoning_summary_text.delta" ||
      type == "response.reasoning_text.delta") {
    auto *slot = slot_for(event);
    const auto delta = event.value("delta", "");
    if (slot->type == "message") {
      auto &text = std::get<TextContent>(result_->content[slot->content_index]);
      text.text += delta;
      if (on_event_ && !delta.empty())
        on_event_(
            AssistantMessageTextDeltaEvent{.content_index = slot->content_index,
                                           .delta = delta,
                                           .partial = *result_});
    } else if (slot->type == "reasoning") {
      auto &thinking =
          std::get<ThinkingContent>(result_->content[slot->content_index]);
      thinking.thinking += delta;
      if (on_event_ && !delta.empty())
        on_event_(AssistantMessageThinkingDeltaEvent{.content_index =
                                                         slot->content_index,
                                                     .delta = delta,
                                                     .partial = *result_});
    }
    return;
  }
  if (type == "response.reasoning_summary_part.done") {
    auto *slot = slot_for(event);
    if (slot->type == "reasoning") {
      auto &thinking =
          std::get<ThinkingContent>(result_->content[slot->content_index]);
      thinking.thinking += "\n\n";
      if (on_event_)
        on_event_(AssistantMessageThinkingDeltaEvent{.content_index =
                                                         slot->content_index,
                                                     .delta = "\n\n",
                                                     .partial = *result_});
    }
    return;
  }
  if (type == "response.function_call_arguments.delta") {
    auto *slot = slot_for(event);
    const auto delta = event.value("delta", "");
    slot->partial_arguments += delta;
    auto &call = std::get<ToolCall>(result_->content[slot->content_index]);
    call.partial_json = slot->partial_arguments;
    if (on_event_ && !delta.empty())
      on_event_(AssistantMessageToolCallDeltaEvent{.content_index =
                                                       slot->content_index,
                                                   .delta = delta,
                                                   .partial = *result_});
    return;
  }
  if (type == "response.function_call_arguments.done") {
    auto *slot = slot_for(event);
    slot->partial_arguments = event.value("arguments", slot->partial_arguments);
    return;
  }
  if (type == "response.output_item.done") {
    auto index = event.value("output_index", 0);
    auto it = slots_.find(index);
    if (it == slots_.end()) {
      const auto item_id = event.value("item_id", "");
      if (auto item_it = item_slots_.find(item_id);
          item_it != item_slots_.end()) {
        index = item_it->second;
        it = slots_.find(index);
      }
    }
    if (it == slots_.end()) {
      Json added = event;
      added["type"] = "response.output_item.added";
      added["output_index"] = index;
      process_event("response.output_item.added", added);
      it = slots_.find(index);
    }
    if (it != slots_.end()) {
      const auto item = event.value("item", Json::object());
      close_slot(it->second, &item);
    }
    return;
  }
  if (type == "response.completed" || type == "response.done" ||
      type == "response.incomplete" || type == "response.failed") {
    terminal_seen_ = true;
    result_->response_id =
        response->value("id", result_->response_id.value_or(""));
    parse_usage(response->value("usage", Json::object()));
    for (auto &[index, slot] : slots_)
      close_slot(slot);
    if (type == "response.incomplete") {
      const auto reason = response->value("incomplete_details", Json::object())
                              .value("reason", "");
      result_->stop_reason = reason == "max_output_tokens" ? StopReason::length
                                                           : StopReason::error;
      if (result_->stop_reason == StopReason::error)
        result_->error_message =
            reason.empty() ? "OpenAI response incomplete" : reason;
    } else if (type == "response.failed") {
      result_->stop_reason = StopReason::error;
      result_->error_message = safe_text(
          response->value("error", Json::object()), "OpenAI response failed");
    } else {
      bool has_tool = false;
      for (const auto &content : result_->content)
        has_tool = has_tool || std::holds_alternative<ToolCall>(content);
      result_->stop_reason = has_tool ? StopReason::tool_use : StopReason::stop;
    }
    if (result_->stop_reason == StopReason::error)
      emit_error();
    else if (on_event_)
      on_event_(AssistantMessageDoneEvent{.reason = result_->stop_reason,
                                          .message = *result_});
    return;
  }
  if (type == "error") {
    auto message = safe_text(event.value("error", Json::object()), "");
    if (message.empty())
      message =
          event.value("message", event.value("code", "OpenAI response error"));
    fail(message);
  }
}

void OpenAICodexResponsesParser::feed_line(std::string_view line) {
  if (failed_ || terminal_seen_)
    return;
  if (!line.empty() && line.back() == '\r')
    line.remove_suffix(1);
  if (line.empty()) {
    pending_event_name_.clear();
    return;
  }
  if (line.starts_with(':'))
    return;
  if (line.starts_with("event:")) {
    line.remove_prefix(6);
    while (!line.empty() && line.front() == ' ')
      line.remove_prefix(1);
    pending_event_name_ = std::string(line);
    return;
  }
  if (!line.starts_with("data:"))
    return;
  line.remove_prefix(5);
  while (!line.empty() && line.front() == ' ')
    line.remove_prefix(1);
  if (line == "[DONE]")
    return;
  auto parsed = Json::parse(line, nullptr, false);
  if (parsed.is_discarded()) {
    fail("OpenAI response contained invalid SSE JSON");
    return;
  }
  process_event(pending_event_name_, parsed);
  pending_event_name_.clear();
}

void OpenAICodexResponsesParser::finish() {
  if (failed_)
    return;
  if (!terminal_seen_)
    fail("OpenAI response ended before a terminal event");
}

std::shared_ptr<AssistantMessage> OpenAICodexResponsesClient::stream(
    const Model &model, const AgentContext &context,
    const StreamOptions &options, AssistantEventCallback on_event,
    std::stop_token stop_tok) {
  auto result = std::make_shared<AssistantMessage>();
  result->api = model.api;
  result->provider = model.provider;
  result->model = model.id;
  result->stop_reason = StopReason::error;
  result->timestamp = now_ms();
  if (options.transport == Transport::websocket ||
      options.transport == Transport::websocket_cached) {
    result->error_message =
        "openai-codex supports SSE only; WebSocket transport is not supported";
    if (on_event)
      on_event(AssistantMessageErrorEvent{.reason = StopReason::error,
                                          .error = *result});
    return result;
  }

  Json request;
  try {
    request = build_request_json(model, context, options);
  } catch (const std::exception &error) {
    result->error_message = error.what();
    if (on_event)
      on_event(AssistantMessageErrorEvent{.reason = StopReason::error,
                                          .error = *result});
    return result;
  }
  std::map<std::string, std::string> headers = model.headers;
  merge_headers_case_insensitive(headers, options.headers);
  std::map<std::string, std::string> required_headers{
      {"Accept", "text/event-stream"},
      {"OpenAI-Beta", "responses=experimental"},
      {"originator", "pi"},
      {"User-Agent", user_agent()}};
  if (options.session_id) {
    required_headers["session-id"] = *options.session_id;
    required_headers["x-client-request-id"] = *options.session_id;
  }
  merge_headers_case_insensitive(headers, required_headers);
  auto auth = options.auth;
  if (!auth && options.api_key)
    auth = RequestAuth{.kind = AuthKind::api_key,
                       .bearer_token = options.api_key,
                       .source = "legacy-api-key"};
  OpenAICodexResponsesParser parser(model, std::move(on_event));
  const auto ok = HttpClient::post_streaming_authenticated(
      endpoint_url(model.base_url), request.dump(),
      [&parser](const std::string &line) { parser.feed_line(line); }, headers,
      auth, options.timeout_ms, stop_tok, options.diagnostics,
      [&options, &model](int status,
                         const std::map<std::string, std::string> &headers) {
        if (options.on_response)
          options.on_response(status, headers, model);
      });
  parser.finish();
  result = parser.result();
  if (stop_tok.stop_requested()) {
    result->stop_reason = StopReason::aborted;
    result->error_message = "Request was aborted";
  } else if (!ok && !result->error_message) {
    result->stop_reason = StopReason::error;
    result->error_message = "OpenAI Codex request failed";
  }
  return result;
}

CompactionResult parse_compact_response(const Model &model,
                                        const nlohmann::json &body) {
  if (!body.is_object() || !body.contains("output") ||
      !body["output"].is_array())
    throw std::runtime_error(
        "OpenAI Codex compact response missing \"output\" array");

  CompactionResult result;
  for (const auto &item : body["output"]) {
    if (!item.is_object())
      continue;
    const auto type = item.value("type", std::string{});
    if (type == "compaction") {
      // The compaction item is the load-bearing part of the response; a
      // malformed one fails the whole compaction rather than silently
      // installing a transcript missing its own summary.
      if (!item.contains("encrypted_content") ||
          !item["encrypted_content"].is_string())
        throw std::runtime_error("OpenAI Codex compact response \"compaction\" "
                                 "item missing encrypted_content");
      ContextCompactionMessage msg;
      msg.api = model.api;
      msg.provider = model.provider;
      msg.model = model.id;
      msg.encrypted_content = item["encrypted_content"].get<std::string>();
      if (item.contains("id") && item["id"].is_string())
        msg.item_id = item["id"].get<std::string>();
      msg.timestamp = now_ms();
      result.messages.emplace_back(std::move(msg));
      continue;
    }
    if (type != "message")
      // function_call/function_call_output/reasoning/compaction_trigger and
      // any other item type: the documented V1 contract drops these
      // unconditionally regardless of retention policy (see
      // should_keep_compacted_history_item in the reference implementation),
      // and the production endpoint does not emit them in compact output.
      // Ignoring them here rather than round-tripping through a type pici's
      // Message model cannot represent flatly keeps this parser exact for
      // what V1 actually returns.
      continue;
    const auto role = item.value("role", std::string{});
    if (role != "user" && role != "assistant")
      // "developer" wrappers and anything else: always dropped by policy,
      // and pici has no Message variant to represent a bare developer role.
      continue;
    auto content_it = item.find("content");
    if (content_it == item.end() || !content_it->is_array())
      continue; // Malformed individual item; skip rather than abort.
    std::vector<ContentBlock> blocks;
    for (const auto &part : *content_it) {
      if (!part.is_object())
        continue;
      const auto part_type = part.value("type", std::string{});
      if (part_type == "input_text" || part_type == "output_text" ||
          part_type == "text") {
        auto text = part.value("text", std::string{});
        if (!text.empty())
          blocks.emplace_back(TextContent{.text = std::move(text)});
      }
    }
    if (blocks.empty())
      continue;
    if (role == "user") {
      UserMessage msg;
      msg.content = std::move(blocks);
      msg.timestamp = now_ms();
      result.messages.emplace_back(std::move(msg));
    } else {
      AssistantMessage msg;
      msg.api = model.api;
      msg.provider = model.provider;
      msg.model = model.id;
      msg.stop_reason = StopReason::stop;
      msg.content = std::move(blocks);
      msg.timestamp = now_ms();
      result.messages.emplace_back(std::move(msg));
    }
  }
  return result;
}

CompactionResult OpenAICodexResponsesClient::compact(
    const Model &model, const AgentContext &context,
    const CompactionOptions &options, std::stop_token stop_tok) {
  CompactionResult result;

  Json request;
  try {
    request = build_compact_request_json(model, context, options);
  } catch (const std::exception &error) {
    result.error_message = error.what();
    return result;
  }

  std::map<std::string, std::string> headers = model.headers;
  merge_headers_case_insensitive(headers, options.headers);
  std::map<std::string, std::string> required_headers{
      {"Accept", "application/json"},
      {"Content-Type", "application/json"},
      {"OpenAI-Beta", "responses=experimental"},
      {"originator", "pi"},
      {"User-Agent", user_agent()}};
  if (options.session_id) {
    required_headers["session-id"] = *options.session_id;
    required_headers["x-client-request-id"] = *options.session_id;
  }
  merge_headers_case_insensitive(headers, required_headers);
  auto auth = options.auth;
  if (!auth && options.api_key)
    auth = RequestAuth{.kind = AuthKind::api_key,
                       .bearer_token = options.api_key,
                       .source = "legacy-api-key"};

  const auto timeout_ms = compact_request_timeout_ms(options.timeout_ms);
  // Diagnostics record only the outgoing payload's byte length, never its
  // content — mirrors how the streaming path's StreamDiagnostics calls only
  // ever pass byte counts. The "compact" stage keeps these entries
  // distinguishable from ordinary streaming "transport"/"parser" events in
  // the same trace file.
  if (options.diagnostics)
    options.diagnostics->record_compact_event("request_sent",
                                              request.dump().size());
  auto response = HttpClient::post_authenticated(
      compact_endpoint_url(model.base_url), request.dump(), headers, auth,
      timeout_ms, stop_tok);

  if (stop_tok.stop_requested()) {
    result.cancelled = true;
    result.error_message = "Request was aborted";
    if (options.diagnostics)
      options.diagnostics->record_compact_event("cancelled");
    return result;
  }
  if (!response) {
    result.error_message = "OpenAI Codex compact request failed (no response)";
    if (options.diagnostics)
      options.diagnostics->record_compact_event("no_response");
    return result;
  }
  if (options.on_response)
    options.on_response(response->status_code, response->headers, model);
  if (options.diagnostics)
    options.diagnostics->record_compact_event(
        "response_status_" + std::to_string(response->status_code),
        response->body.size());
  if (response->status_code < 200 || response->status_code >= 300) {
    std::string message = "OpenAI Codex compact request failed with status " +
                          std::to_string(response->status_code);
    auto err = Json::parse(response->body, nullptr, false);
    if (!err.is_discarded() && err.contains("error")) {
      auto text = safe_text(err.value("error", Json::object()), "");
      if (!text.empty())
        message += ": " + text;
    }
    result.error_message = message;
    result.http_status = response->status_code;
    if (options.diagnostics)
      options.diagnostics->record_compact_event("http_error");
    return result;
  }

  auto body = Json::parse(response->body, nullptr, false);
  if (body.is_discarded()) {
    result.error_message = "OpenAI Codex compact response was not valid JSON";
    if (options.diagnostics)
      options.diagnostics->record_compact_event("invalid_json");
    return result;
  }
  try {
    result = parse_compact_response(model, body);
  } catch (const std::exception &error) {
    result = CompactionResult{};
    result.error_message = error.what();
    if (options.diagnostics)
      options.diagnostics->record_compact_event("parse_failed");
    return result;
  }
  if (body.contains("id") && body["id"].is_string())
    result.response_id = body["id"].get<std::string>();
  if (body.contains("usage") && body["usage"].is_object()) {
    const auto &usage = body["usage"];
    result.usage.input = usage.value("input_tokens", std::uint64_t{0});
    result.usage.output = usage.value("output_tokens", std::uint64_t{0});
    result.usage.total_tokens =
        usage.value("total_tokens", result.usage.input + result.usage.output);
  }
  if (options.diagnostics)
    options.diagnostics->record_compact_event("parsed_ok",
                                              result.messages.size());
  return result;
}

void register_openai_codex_responses_client() {
  LLMClientRegistry::instance().register_client("openai-codex-responses", [] {
    return std::make_shared<OpenAICodexResponsesClient>();
  });
}

} // namespace pi::core
