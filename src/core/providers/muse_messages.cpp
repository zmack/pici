#include "core/providers/muse_messages.h"
#include "core/auth_types.h"

#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/terminal.h"
#include "core/providers/transform_messages.h"
#include "http/http_client.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

#include <nlohmann/json.hpp>

namespace pi::core {
namespace {

using Json = nlohmann::json; // NOLINT(misc-include-cleaner)

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

std::string normalize_tool_call_id(const std::string &id) {
  if (id.contains('|')) {
    auto pos = id.find('|');
    std::string call_id = id.substr(0, pos);
    if (call_id.size() > 40)
      call_id.resize(40);
    return call_id;
  }
  if (id.size() > 40)
    return id.substr(0, 40);
  return id;
}

Json image_block(const ImageContent &image) {
  return {{"type", "image"},
          {"source",
           {{"type", "base64"},
            {"media_type", image.mime_type},
            {"data", image.data}}}};
}

Json tool_result_block(const ToolResultMessage &tool_result) {
  Json content = Json::array();
  for (const auto &block : tool_result.content) {
    if (const auto *text = std::get_if<TextContent>(&block)) {
      content.push_back({{"type", "text"}, {"text", text->text}});
    } else if (const auto *image = std::get_if<ImageContent>(&block)) {
      content.push_back(image_block(*image));
    }
  }

  Json serialized_content = content;
  if (content.empty()) {
    serialized_content = "";
  } else if (content.size() == 1 && content[0]["type"] == "text") {
    serialized_content = content[0]["text"];
  }

  return {{"type", "tool_result"},
          {"tool_use_id", tool_result.tool_call_id},
          {"content", std::move(serialized_content)},
          {"is_error", tool_result.is_error}};
}

Json convert_tools(const AgentContext &context) {
  Json tools = Json::array();
  for (const auto &tool : context.tools) {
    Json schema = Json::parse(tool->schema().serialize(), nullptr, false);
    if (schema.is_discarded() || !schema.is_object())
      schema = Json::object();
    tools.push_back({{"type", "custom"},
                     {"name", std::string(tool->name())},
                     {"description", std::string(tool->description())},
                     {"input_schema", std::move(schema)},
                     {"strict", false}});
  }
  return tools;
}

Json convert_messages(const Model &model, const AgentContext &context) {
  auto transformed =
      transform_messages(context.messages, model, normalize_tool_call_id);
  Json messages = Json::array();
  std::vector<Json> pending_tool_results;

  auto flush_tool_results = [&] {
    if (pending_tool_results.empty())
      return;
    messages.push_back(
        {{"role", "user"}, {"content", std::move(pending_tool_results)}});
    pending_tool_results.clear();
  };

  for (const auto &message : transformed) {
    if (const auto *tool_result = std::get_if<ToolResultMessage>(&message)) {
      pending_tool_results.push_back(tool_result_block(*tool_result));
      continue;
    }
    flush_tool_results();

    if (const auto *user = std::get_if<UserMessage>(&message)) {
      Json content = Json::array();
      for (const auto &block : user->content) {
        if (const auto *text = std::get_if<TextContent>(&block)) {
          content.push_back({{"type", "text"}, {"text", text->text}});
        } else if (const auto *image = std::get_if<ImageContent>(&block)) {
          content.push_back(image_block(*image));
        }
      }
      if (content.empty())
        continue;
      if (content.size() == 1 && content[0]["type"] == "text") {
        messages.push_back({{"role", "user"}, {"content", content[0]["text"]}});
      } else {
        messages.push_back({{"role", "user"}, {"content", content}});
      }
      continue;
    }

    if (const auto *assistant = std::get_if<AssistantMessage>(&message)) {
      Json content = Json::array();
      for (const auto &block : assistant->content) {
        if (const auto *text = std::get_if<TextContent>(&block)) {
          content.push_back({{"type", "text"}, {"text", text->text}});
        } else if (const auto *thinking =
                       std::get_if<ThinkingContent>(&block)) {
          if (thinking->redacted) {
            if (thinking->thinking_signature &&
                !thinking->thinking_signature->empty()) {
              content.push_back({{"type", "redacted_thinking"},
                                 {"data", *thinking->thinking_signature}});
            }
          } else if (!thinking->thinking.empty()) {
            Json thinking_block = {{"type", "thinking"},
                                   {"thinking", thinking->thinking}};
            if (thinking->thinking_signature)
              thinking_block["signature"] = *thinking->thinking_signature;
            content.push_back(std::move(thinking_block));
          }
        } else if (const auto *tool_call = std::get_if<ToolCall>(&block)) {
          Json input = tool_call->arguments.is_object() ? tool_call->arguments
                                                        : Json::object();
          content.push_back({{"type", "tool_use"},
                             {"id", tool_call->id},
                             {"name", tool_call->name},
                             {"input", std::move(input)}});
        }
      }
      if (content.empty())
        continue;
      if (content.size() == 1 && content[0]["type"] == "text") {
        messages.push_back(
            {{"role", "assistant"}, {"content", content[0]["text"]}});
      } else {
        messages.push_back({{"role", "assistant"}, {"content", content}});
      }
      continue;
    }

    // Unsupported history blocks are omitted rather than serialized into an
    // invalid partial Messages conversation. This also covers
    // ContextCompactionMessage, which is Codex-specific and already dropped
    // by transform_messages for any model that did not produce it.
  }

  flush_tool_results();

  return messages;
}

void update_usage(const Json &usage_json, TokenUsage &usage) {
  if (!usage_json.is_object())
    return;

  if (usage_json.contains("output_tokens")) {
    usage.output = usage_json.value("output_tokens", usage.output);
  }
  if (usage_json.contains("cache_read_input_tokens")) {
    usage.cache_read =
        usage_json.value("cache_read_input_tokens", usage.cache_read);
  }
  if (usage_json.contains("cache_creation_input_tokens")) {
    usage.cache_write =
        usage_json.value("cache_creation_input_tokens", usage.cache_write);
  }
  if (usage_json.contains("input_tokens")) {
    const auto reported_input =
        usage_json.value("input_tokens", static_cast<std::uint64_t>(0));
    usage.input = reported_input >= usage.cache_read + usage.cache_write
                      ? reported_input - usage.cache_read - usage.cache_write
                      : 0;
  }
  usage.total_tokens =
      usage.input + usage.output + usage.cache_read + usage.cache_write;
}

void add_validated_metadata(const Json &metadata, Json &request) {
  if (metadata.is_null())
    return;
  if (!metadata.is_object())
    throw std::invalid_argument("Muse metadata must be a JSON object");
  for (const auto &[key, value] : metadata.items()) {
    if (!value.is_string()) {
      throw std::invalid_argument("Muse metadata values must be strings: " +
                                  key);
    }
  }
  request["metadata"] = metadata;
}

std::string trim_cr(std::string_view line) {
  if (!line.empty() && line.back() == '\r')
    line.remove_suffix(1);
  return std::string(line);
}

std::string data_value(std::string_view line) {
  line.remove_prefix(5);
  if (!line.empty() && line.front() == ' ')
    line.remove_prefix(1);
  return std::string(line);
}

} // namespace

MuseMessagesSseParser::MuseMessagesSseParser(
    std::shared_ptr<AssistantMessage> result, AssistantEventCallback on_event,
    std::shared_ptr<StreamDiagnostics> diagnostics)
    : result_(std::move(result)), on_event_(std::move(on_event)),
      diagnostics_(std::move(diagnostics)) {
  if (!result_)
    throw std::invalid_argument("Muse SSE parser requires a result");
}

void MuseMessagesSseParser::feed_line(std::string_view raw_line) {
  auto line = trim_cr(raw_line);
  if (line.empty())
    return;

  if (line.starts_with("event:")) {
    event_name_ = data_value(line);
    return;
  }
  if (line.starts_with("data:")) {
    process_data(data_value(line));
    event_name_.clear();
    return;
  }

  // HttpClient uses this path for synthetic transport errors.
  if (line.front() == '{')
    process_data(line);
}

void MuseMessagesSseParser::process_data(std::string_view data) {
  if (data.empty() || data == "[DONE]")
    return;

  auto event = Json::parse(data, nullptr, false);
  if (event.is_discarded() || !event.is_object())
    return;

  std::string type = event_name_;
  if (auto type_it = event.find("type");
      type_it != event.end() && type_it->is_string()) {
    type = type_it->get<std::string>();
  }
  if (diagnostics_)
    diagnostics_->record_parser_event(type, data.size());

  if (type == "ping")
    return;

  if (type == "error" || event.contains("error")) {
    if (!error_) {
      const auto &error = event["error"];
      if (error.is_object()) {
        error_ = error.value("message", std::string("Muse request failed"));
      } else if (error.is_string()) {
        error_ = error.get<std::string>();
      } else {
        error_ = "Muse request failed";
      }
      result_->stop_reason = StopReason::error;
      result_->error_message = *error_;
    }
    return;
  }

  if (type == "message_start") {
    const auto &message =
        event.contains("message") && event["message"].is_object()
            ? event["message"]
            : event;
    if (auto id_it = message.find("id");
        id_it != message.end() && id_it->is_string()) {
      result_->response_id = id_it->get<std::string>();
    }
    if (auto model_it = message.find("model");
        model_it != message.end() && model_it->is_string()) {
      result_->response_model = model_it->get<std::string>();
    }
    if (auto usage_it = message.find("usage"); usage_it != message.end()) {
      update_usage(*usage_it, result_->usage);
    }
    return;
  }

  if (type == "content_block_start") {
    start_block(event);
    return;
  }
  if (type == "content_block_delta") {
    process_delta(event);
    return;
  }
  if (type == "content_block_stop") {
    stop_block(event);
    return;
  }
  if (type == "message_delta") {
    if (auto delta_it = event.find("delta");
        delta_it != event.end() && delta_it->is_object()) {
      if (auto reason_it = delta_it->find("stop_reason");
          reason_it != delta_it->end() && reason_it->is_string()) {
        const auto reason = reason_it->get<std::string>();
        result_->stop_reason = MuseMessagesClient::map_stop_reason(reason);
        if (reason == "refusal" && !error_) {
          error_ = "Muse refused the request";
          result_->error_message = *error_;
        }
      }
    }
    if (auto usage_it = event.find("usage"); usage_it != event.end())
      update_usage(*usage_it, result_->usage);
    return;
  }
  if (type == "message_stop") {
    finish();
  }
}

void MuseMessagesSseParser::start_block(const nlohmann::json &event) {
  auto index_it = event.find("index");
  if (index_it == event.end() || !index_it->is_number_unsigned())
    return;
  const auto index = index_it->get<std::size_t>();

  if (active_block_index_)
    finish_block(*active_block_index_);

  BlockState state;
  const auto block_it = event.find("content_block");
  std::string block_type;
  if (block_it != event.end() && block_it->is_object()) {
    const auto type_it = block_it->find("type");
    if (type_it != block_it->end() && type_it->is_string())
      block_type = type_it->get<std::string>();
  }
  if (block_type == "text") {
    state.kind = BlockKind::text;
    state.content_index = result_->content.size();
    TextContent text;
    if (block_it != event.end() && block_it->is_object()) {
      const auto text_it = block_it->find("text");
      if (text_it != block_it->end() && text_it->is_string())
        text.text = text_it->get<std::string>();
    }
    result_->content.emplace_back(std::move(text));
    active_block_index_ = index;
    if (on_event_) {
      on_event_(AssistantMessageTextStartEvent{
          .content_index = *state.content_index, .partial = *result_});
    }
  } else if (block_type == "thinking") {
    state.kind = BlockKind::thinking;
    state.content_index = result_->content.size();
    ThinkingContent thinking;
    if (block_it != event.end() && block_it->is_object()) {
      const auto thinking_it = block_it->find("thinking");
      if (thinking_it != block_it->end() && thinking_it->is_string())
        thinking.thinking = thinking_it->get<std::string>();
      const auto signature_it = block_it->find("signature");
      if (signature_it != block_it->end() && signature_it->is_string())
        thinking.thinking_signature = signature_it->get<std::string>();
    }
    result_->content.emplace_back(std::move(thinking));
    active_block_index_ = index;
    if (on_event_) {
      on_event_(AssistantMessageThinkingStartEvent{
          .content_index = *state.content_index, .partial = *result_});
    }
  } else if (block_type == "redacted_thinking") {
    state.kind = BlockKind::redacted_thinking;
    state.content_index = result_->content.size();
    ThinkingContent thinking;
    thinking.redacted = true;
    const auto data_it = block_it->find("data");
    if (data_it != block_it->end() && data_it->is_string())
      thinking.thinking_signature = data_it->get<std::string>();
    result_->content.emplace_back(std::move(thinking));
  } else if (block_type == "tool_use") {
    state.kind = BlockKind::tool_call;
    state.content_index = result_->content.size();
    ToolCall tool_call;
    if (block_it != event.end() && block_it->is_object()) {
      const auto id_it = block_it->find("id");
      if (id_it != block_it->end() && id_it->is_string())
        tool_call.id = id_it->get<std::string>();
      const auto name_it = block_it->find("name");
      if (name_it != block_it->end() && name_it->is_string())
        tool_call.name = name_it->get<std::string>();
      const auto input_it = block_it->find("input");
      if (input_it != block_it->end() && input_it->is_object())
        tool_call.arguments = *input_it;
    }
    result_->content.emplace_back(std::move(tool_call));
    active_block_index_ = index;
    if (on_event_) {
      on_event_(AssistantMessageToolCallStartEvent{
          .content_index = *state.content_index, .partial = *result_});
    }
  }
  blocks_[index] = state;
}

void MuseMessagesSseParser::process_delta(const nlohmann::json &event) {
  auto index_it = event.find("index");
  auto delta_it = event.find("delta");
  if (index_it == event.end() || !index_it->is_number_unsigned() ||
      delta_it == event.end() || !delta_it->is_object())
    return;

  const auto block_it = blocks_.find(index_it->get<std::size_t>());
  if (block_it == blocks_.end() || !block_it->second.content_index)
    return;

  const auto delta_type_it = delta_it->find("type");
  if (delta_type_it == delta_it->end() || !delta_type_it->is_string())
    return;
  const auto delta_type = delta_type_it->get<std::string>();
  const auto content_index = block_it->second.content_index.value_or(0);
  if (block_it->second.kind == BlockKind::text && delta_type == "text_delta") {
    const auto text_it = delta_it->find("text");
    if (text_it == delta_it->end() || !text_it->is_string())
      return;
    auto &text = std::get<TextContent>(result_->content[content_index]).text;
    const auto delta = text_it->get<std::string>();
    text += delta;
    if (on_event_) {
      on_event_(AssistantMessageTextDeltaEvent{
          .content_index = content_index, .delta = delta, .partial = *result_});
    }
  } else if (block_it->second.kind == BlockKind::thinking &&
             delta_type == "thinking_delta") {
    const auto thinking_it = delta_it->find("thinking");
    if (thinking_it == delta_it->end() || !thinking_it->is_string())
      return;
    auto &thinking =
        std::get<ThinkingContent>(result_->content[content_index]).thinking;
    const auto delta = thinking_it->get<std::string>();
    thinking += delta;
    if (on_event_) {
      on_event_(AssistantMessageThinkingDeltaEvent{
          .content_index = content_index, .delta = delta, .partial = *result_});
    }
  } else if (block_it->second.kind == BlockKind::thinking &&
             delta_type == "signature_delta") {
    const auto signature_it = delta_it->find("signature");
    if (signature_it == delta_it->end() || !signature_it->is_string())
      return;
    auto &signature = std::get<ThinkingContent>(result_->content[content_index])
                          .thinking_signature;
    if (!signature)
      signature = std::string{};
    *signature += signature_it->get<std::string>();
  } else if (block_it->second.kind == BlockKind::redacted_thinking &&
             delta_type == "redacted_thinking_delta") {
    const auto data_it = delta_it->find("data");
    if (data_it == delta_it->end() || !data_it->is_string())
      return;
    auto &signature = std::get<ThinkingContent>(result_->content[content_index])
                          .thinking_signature;
    if (!signature)
      signature = std::string{};
    *signature += data_it->get<std::string>();
  } else if (block_it->second.kind == BlockKind::tool_call &&
             delta_type == "input_json_delta") {
    const auto partial_json_it = delta_it->find("partial_json");
    if (partial_json_it == delta_it->end() || !partial_json_it->is_string())
      return;
    const auto delta = partial_json_it->get<std::string>();
    auto &block = block_it->second;
    block.partial_json += delta;
    auto &tool_call = std::get<ToolCall>(result_->content[content_index]);
    tool_call.partial_json = block.partial_json;
    if (on_event_) {
      on_event_(AssistantMessageToolCallDeltaEvent{
          .content_index = content_index, .delta = delta, .partial = *result_});
    }
  }
}

void MuseMessagesSseParser::stop_block(const nlohmann::json &event) {
  auto index_it = event.find("index");
  if (index_it == event.end() || !index_it->is_number_unsigned())
    return;
  const auto index = index_it->get<std::size_t>();
  auto block_it = blocks_.find(index);
  if (block_it != blocks_.end())
    finish_block(index);
}

void MuseMessagesSseParser::finish_block(std::size_t protocol_index) {
  auto block_it = blocks_.find(protocol_index);
  if (block_it == blocks_.end())
    return;
  if (block_it->second.finished) {
    if (active_block_index_ && *active_block_index_ == protocol_index)
      active_block_index_.reset();
    return;
  }
  block_it->second.finished = true;
  if (block_it->second.kind == BlockKind::text)
    finish_text_block(protocol_index);
  else if (block_it->second.kind == BlockKind::thinking)
    finish_thinking_block(protocol_index);
  else if (block_it->second.kind == BlockKind::tool_call)
    finish_tool_call_block(protocol_index);
  else if (active_block_index_ && *active_block_index_ == protocol_index)
    active_block_index_.reset();
}

void MuseMessagesSseParser::finish_text_block(std::size_t protocol_index) {
  auto block_it = blocks_.find(protocol_index);
  if (block_it == blocks_.end() || block_it->second.kind != BlockKind::text ||
      !block_it->second.content_index)
    return;

  const auto content_index = block_it->second.content_index.value_or(0);
  const auto &text =
      std::get<TextContent>(result_->content[content_index]).text;
  if (on_event_) {
    on_event_(AssistantMessageTextEndEvent{
        .content_index = content_index, .content = text, .partial = *result_});
  }
  if (active_block_index_ && *active_block_index_ == protocol_index)
    active_block_index_.reset();
}

void MuseMessagesSseParser::finish_thinking_block(std::size_t protocol_index) {
  auto block_it = blocks_.find(protocol_index);
  if (block_it == blocks_.end() ||
      block_it->second.kind != BlockKind::thinking ||
      !block_it->second.content_index)
    return;

  const auto content_index = block_it->second.content_index.value_or(0);
  const auto &thinking =
      std::get<ThinkingContent>(result_->content[content_index]).thinking;
  if (on_event_) {
    on_event_(AssistantMessageThinkingEndEvent{.content_index = content_index,
                                               .content = thinking,
                                               .partial = *result_});
  }
  if (active_block_index_ && *active_block_index_ == protocol_index)
    active_block_index_.reset();
}

void MuseMessagesSseParser::finish_tool_call_block(std::size_t protocol_index) {
  auto block_it = blocks_.find(protocol_index);
  if (block_it == blocks_.end() ||
      block_it->second.kind != BlockKind::tool_call ||
      !block_it->second.content_index)
    return;

  const auto content_index = block_it->second.content_index.value_or(0);
  auto &tool_call = std::get<ToolCall>(result_->content[content_index]);
  if (!block_it->second.partial_json.empty()) {
    auto parsed = Json::parse(block_it->second.partial_json, nullptr, false);
    tool_call.arguments = parsed.is_discarded() || !parsed.is_object()
                              ? Json::object()
                              : std::move(parsed);
  }
  tool_call.partial_json.clear();
  if (on_event_) {
    on_event_(AssistantMessageToolCallEndEvent{.content_index = content_index,
                                               .tool_call = tool_call,
                                               .partial = *result_});
  }
  if (active_block_index_ && *active_block_index_ == protocol_index)
    active_block_index_.reset();
}

void MuseMessagesSseParser::emit_done() {
  if (done_emitted_ || error_)
    return;
  done_emitted_ = true;
  if (on_event_) {
    on_event_(AssistantMessageDoneEvent{.reason = result_->stop_reason,
                                        .message = *result_});
  }
}

void MuseMessagesSseParser::finish() {
  if (active_block_index_)
    finish_block(*active_block_index_);
  emit_done();
}

MuseMessagesClient::MuseMessagesClient(std::string base_url,
                                       std::string model_id)
    : base_url_(std::move(base_url)), model_id_(std::move(model_id)) {}

nlohmann::json
MuseMessagesClient::build_request_json(const Model &model,
                                       const AgentContext &context,
                                       const StreamOptions &options) {
  const auto max_tokens =
      options.max_tokens.value_or(static_cast<std::uint32_t>(model.max_tokens));
  if (max_tokens == 0)
    throw std::invalid_argument("Muse Messages requires max_tokens");

  nlohmann::json request = {
      {"model", model.id},
      {"messages", convert_messages(model, context)},
      {"max_tokens", max_tokens},
      {"stream", true},
  };
  // NOTE: Messages API Request fields list disallows unknown top-level
  // fields (400). prompt_caching.md says prompt_cache_key belongs to
  // Chat Completions / Responses only — do NOT send it here. Automatic
  // prefix caching works without any key because we resend full history
  // verbatim (system_prompt first, then messages[0..N-1]).
  // See docs/muse/messages_api.md#request-fields.

  if (options.reasoning != ThinkingLevel::off) {
    request["thinking"] = {{"type", "adaptive"}};
    const auto level = std::string(thinking_level_to_string(options.reasoning));
    if (const auto it = model.thinking_level_map.find(level);
        it != model.thinking_level_map.end()) {
      if (const auto &effort = it->second)
        request["output_config"]["effort"] = *effort;
    }
  }
  if (!context.system_prompt.empty())
    request["system"] = context.system_prompt;
  if (options.temperature)
    request["temperature"] = *options.temperature;
  if (!context.tools.empty())
    request["tools"] = convert_tools(context);
  add_validated_metadata(options.metadata, request);

  if (options.on_payload) {
    auto next = options.on_payload(request, model);
    if (next)
      request = *next;
  }
  return request;
}

StopReason MuseMessagesClient::map_stop_reason(std::string_view reason) {
  if (reason == "end_turn")
    return StopReason::stop;
  if (reason == "tool_use")
    return StopReason::tool_use;
  if (reason == "max_tokens")
    return StopReason::length;
  return StopReason::error;
}

std::shared_ptr<AssistantMessage>
MuseMessagesClient::stream(const Model &model, const AgentContext &context,
                           const StreamOptions &options,
                           AssistantEventCallback on_event,
                           std::stop_token stop_tok) {
  auto result = std::make_shared<AssistantMessage>();
  result->api = model.api;
  result->provider = model.provider;
  result->model = model.id;
  result->stop_reason = StopReason::stop;
  result->timestamp = now_ms();

  nlohmann::json request;
  try {
    request = build_request_json(model, context, options);
  } catch (const std::exception &e) {
    result->stop_reason = StopReason::error;
    result->error_message = e.what();
    if (on_event)
      on_event(AssistantMessageErrorEvent{.reason = result->stop_reason,
                                          .error = *result});
    return result;
  }

  std::string url = base_url_.empty() ? model.base_url : base_url_;
  while (!url.empty() && url.back() == '/')
    url.pop_back();
  if (url.ends_with("/v1"))
    url += "/messages";
  else
    url += "/v1/messages";

  if (options.verbose) {
    std::string debug = "[request] POST " + url + "\n" + request.dump(2) + "\n";
    write_best_effort(STDERR_FILENO, debug.data(), debug.size());
  }

  std::map<std::string, std::string> headers = model.headers;
  merge_headers_case_insensitive(headers, options.headers);
  headers["Content-Type"] = "application/json";
  headers["Accept"] = "text/event-stream";

  if (on_event)
    on_event(AssistantMessageStartEvent{*result});

  MuseMessagesSseParser parser(result, on_event, options.diagnostics);
  std::optional<std::string> callback_error;
  auto auth = options.auth;
  if (!auth && options.api_key) {
    auth = RequestAuth{.kind = AuthKind::api_key,
                       .bearer_token = options.api_key,
                       .source = "legacy-api-key"};
  }
  bool ok = HttpClient::post_streaming_authenticated(
      url, request.dump(),
      [&parser, &callback_error](const std::string &line) {
        try {
          parser.feed_line(line);
        } catch (const std::exception &e) {
          callback_error = e.what();
        }
      },
      headers, auth, options.timeout_ms, stop_tok, options.diagnostics);
  parser.finish();

  if (stop_tok.stop_requested() || !ok || parser.error() || callback_error) {
    result->stop_reason =
        stop_tok.stop_requested() ? StopReason::aborted : StopReason::error;
    result->error_message =
        stop_tok.stop_requested()
            ? "Request was aborted"
            : parser.error().value_or(
                  callback_error.value_or("Muse streaming request failed"));
    if (on_event)
      on_event(AssistantMessageErrorEvent{.reason = result->stop_reason,
                                          .error = *result});
    return result;
  }

  compute_cost(result->usage, model.cost);
  return result;
}

} // namespace pi::core

void pi::core::register_muse_messages_client() {
  pi::core::LLMClientRegistry::instance().register_client("muse-messages", [] {
    return std::make_shared<pi::core::MuseMessagesClient>();
  });
}
