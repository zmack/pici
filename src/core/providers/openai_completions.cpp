#include "core/providers/openai_completions.h"
#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/providers/transform_messages.h"
#include "http/http_client.h"
#include "nlohmann/json_fwd.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <variant>

namespace pi::core {

namespace {

std::int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(system_clock::now().time_since_epoch())
      .count();
}

bool str_contains(const std::string &haystack, std::string_view needle) {
  return haystack.contains(needle);
}

bool is_local_endpoint(const std::string &base_url) {
  return str_contains(base_url, "127.0.0.1") ||
         str_contains(base_url, "localhost") ||
         str_contains(base_url, "0.0.0.0");
}

void parse_chunk_usage(const nlohmann::json &usage_j, TokenUsage &out) {
  out.input = usage_j.value("prompt_tokens", static_cast<std::uint64_t>(0));
  out.output =
      usage_j.value("completion_tokens", static_cast<std::uint64_t>(0));
  auto details =
      usage_j.value("prompt_tokens_details", nlohmann::json::object());
  out.cache_read =
      details.value("cached_tokens", static_cast<std::uint64_t>(0));
  out.cache_write =
      details.value("cache_write_tokens", static_cast<std::uint64_t>(0));
  auto reported = usage_j.value("prompt_cache_hit_tokens", out.cache_read);
  if (out.cache_write > 0) {
    out.cache_read =
        std::max(static_cast<std::uint64_t>(0), reported - out.cache_write);
  } else {
    out.cache_read = reported;
  }
  out.input = std::max(static_cast<std::uint64_t>(0),
                       out.input - out.cache_read - out.cache_write);
  out.total_tokens = out.input + out.output + out.cache_read + out.cache_write;
}

nlohmann::json convert_messages(const Model &model, const AgentContext &context,
                                const OpenAICompletionsCompat &compat) {

  auto normalize_id = [](const std::string &id) -> std::string {
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
  };

  auto transformed = transform_messages(context.messages, model, normalize_id);

  nlohmann::json params = nlohmann::json::array();

  if (!context.system_prompt.empty()) {
    std::string role = (model.reasoning && compat.supports_developer_role)
                           ? "developer"
                           : "system";
    params.push_back({{"role", role}, {"content", context.system_prompt}});
  }

  for (const auto &msg : transformed) {
    if (std::holds_alternative<UserMessage>(msg)) {
      const auto &um = std::get<UserMessage>(msg);
      if (um.content.empty())
        continue;

      bool has_non_text = false;
      for (const auto &b : um.content) {
        if (!std::holds_alternative<TextContent>(b)) {
          has_non_text = true;
          break;
        }
      }

      if (!has_non_text && um.content.size() == 1) {
        const auto &tc = std::get<TextContent>(um.content[0]);
        params.push_back({{"role", "user"}, {"content", tc.text}});
      } else {
        nlohmann::json content_arr = nlohmann::json::array();
        for (const auto &b : um.content) {
          if (const auto *tc = std::get_if<TextContent>(&b)) {
            content_arr.push_back({{"type", "text"}, {"text", tc->text}});
          } else if (const auto *img = std::get_if<ImageContent>(&b)) {
            std::string url = "data:" + img->mime_type + ";base64," + img->data;
            content_arr.push_back(
                {{"type", "image_url"}, {"image_url", {{"url", url}}}});
          }
        }
        if (content_arr.empty())
          continue;
        params.push_back({{"role", "user"}, {"content", content_arr}});
      }
    } else if (std::holds_alternative<AssistantMessage>(msg)) {
      const auto &am = std::get<AssistantMessage>(msg);

      std::string content_text;
      for (const auto &b : am.content) {
        if (const auto *tc = std::get_if<TextContent>(&b)) {
          if (!tc->text.empty() &&
              tc->text.find_first_not_of(" \t\n\r") != std::string::npos) {
            content_text += tc->text;
          }
        } else if (const auto *th = std::get_if<ThinkingContent>(&b);
                   th != nullptr && compat.requires_thinking_as_text &&
                   !th->thinking.empty() &&
                   th->thinking.find_first_not_of(" \t\n\r") !=
                       std::string::npos) {
          if (!content_text.empty()) {
            content_text.insert(0, "\n\n");
          }
          content_text.insert(0, th->thinking);
        }
      }

      nlohmann::json tool_calls = nlohmann::json::array();
      for (const auto &b : am.content) {
        if (const auto *tc = std::get_if<ToolCall>(&b)) {
          tool_calls.push_back(
              {{"id", tc->id},
               {"type", "function"},
               {"function",
                {{"name", tc->name}, {"arguments", tc->arguments.dump()}}}});
        }
      }

      bool has_content = !content_text.empty();
      bool has_tools = !tool_calls.empty();
      if (!has_content && !has_tools)
        continue;

      nlohmann::json assistant_msg = {{"role", "assistant"}};
      if (has_content) {
        assistant_msg["content"] = content_text;
      } else {
        assistant_msg["content"] = nullptr;
      }
      if (has_tools) {
        assistant_msg["tool_calls"] = tool_calls;
      }
      params.push_back(std::move(assistant_msg));
    } else if (std::holds_alternative<ToolResultMessage>(msg)) {
      const auto &trm = std::get<ToolResultMessage>(msg);
      std::string text;
      for (const auto &b : trm.content) {
        if (const auto *tc = std::get_if<TextContent>(&b)) {
          if (!text.empty())
            text += '\n';
          text += tc->text;
        }
      }
      nlohmann::json tool_msg = {{"role", "tool"},
                                 {"tool_call_id", trm.tool_call_id},
                                 {"content", text}};
      if (compat.requires_tool_result_name && !trm.tool_name.empty()) {
        tool_msg["name"] = trm.tool_name;
      }
      params.push_back(std::move(tool_msg));
    }
  }

  return params;
}

enum class BlockType { none, text, thinking, tool_call };

struct PartialToolCall {
  std::string id;
  std::string name;
  std::string partial_args;
  int index{0};
  std::optional<std::size_t> content_index;
};

struct StreamingState {
  std::shared_ptr<AssistantMessage> result;
  AssistantEventCallback on_event;
  std::shared_ptr<StreamDiagnostics> diagnostics;
  BlockType current_block{BlockType::none};
  std::size_t current_content_index{0};
  std::map<int, PartialToolCall> partial_tool_calls;
};

void finish_current_block(StreamingState &state) {
  if (state.current_block == BlockType::none)
    return;
  auto &result = *state.result;
  std::size_t idx = state.current_content_index;

  if (state.current_block == BlockType::text) {
    const auto *tc = std::get_if<TextContent>(&result.content[idx]);
    std::string content = (tc != nullptr) ? tc->text : "";
    if (state.on_event) {
      state.on_event(AssistantMessageTextEndEvent{.content_index = idx,
                                                  .content = std::move(content),
                                                  .partial = result});
    }
  } else if (state.current_block == BlockType::thinking) {
    const auto *th = std::get_if<ThinkingContent>(&result.content[idx]);
    std::string content = (th != nullptr) ? th->thinking : "";
    if (state.on_event) {
      state.on_event(
          AssistantMessageThinkingEndEvent{.content_index = idx,
                                           .content = std::move(content),
                                           .partial = result});
    }
  }

  state.current_block = BlockType::none;
}

void process_sse_line(const std::string &line, StreamingState &state) {
  if (line.empty() || line == "data: [DONE]")
    return;
  if (!line.starts_with("data: "))
    return;

  auto json_str = line.substr(6);
  auto chunk = nlohmann::json::parse(json_str, nullptr, false);
  if (chunk.is_discarded())
    return;
  if (state.diagnostics)
    state.diagnostics->record_parser_event("openai_chunk", json_str.size());

  auto &result = *state.result;

  if (!result.response_id.has_value()) {
    auto id_it = chunk.find("id");
    if (id_it != chunk.end() && id_it->is_string()) {
      result.response_id = id_it->get<std::string>();
    }
  }

  if (auto usage_it = chunk.find("usage");
      usage_it != chunk.end() && !usage_it->is_null()) {
    parse_chunk_usage(*usage_it, result.usage);
  }

  auto choices_it = chunk.find("choices");
  if (choices_it == chunk.end() || !choices_it->is_array() ||
      choices_it->empty())
    return;

  const auto &choice = (*choices_it)[0];

  if (auto fr_it = choice.find("finish_reason");
      fr_it != choice.end() && !fr_it->is_null()) {
    result.stop_reason = OpenAICompatibleClient::map_finish_reason(
        fr_it.value().get<std::string>());
  }

  auto delta_it = choice.find("delta");
  if (delta_it == choice.end())
    return;
  const auto &delta = delta_it.value();

  auto content_it = delta.find("content");
  if (content_it != delta.end() && !content_it->is_null()) {
    auto text = content_it.value().get<std::string>();
    if (!text.empty()) {
      if (state.current_block != BlockType::text) {
        finish_current_block(state);
        state.current_content_index = result.content.size();
        result.content.emplace_back(TextContent{.text = ""});
        state.current_block = BlockType::text;
        if (state.on_event) {
          state.on_event(AssistantMessageTextStartEvent{
              .content_index = state.current_content_index, .partial = result});
        }
      }
      auto &tc =
          std::get<TextContent>(result.content[state.current_content_index]);
      tc.text += text;
      if (state.on_event) {
        state.on_event(AssistantMessageTextDeltaEvent{
            .content_index = state.current_content_index,
            .delta = text,
            .partial = result});
      }
    }
  }

  auto reasoning_it = delta.find("reasoning_content");
  if (reasoning_it == delta.end())
    reasoning_it = delta.find("reasoning");
  if (reasoning_it != delta.end() && !reasoning_it->is_null()) {
    auto text = reasoning_it.value().get<std::string>();
    if (!text.empty()) {
      if (state.current_block != BlockType::thinking) {
        finish_current_block(state);
        state.current_content_index = result.content.size();
        result.content.emplace_back(ThinkingContent{.thinking = ""});
        state.current_block = BlockType::thinking;
        if (state.on_event) {
          state.on_event(AssistantMessageThinkingStartEvent{
              .content_index = state.current_content_index, .partial = result});
        }
      }
      auto &th = std::get<ThinkingContent>(
          result.content[state.current_content_index]);
      th.thinking += text;
      if (state.on_event) {
        state.on_event(AssistantMessageThinkingDeltaEvent{
            .content_index = state.current_content_index,
            .delta = text,
            .partial = result});
      }
    }
  }

  auto tool_calls_it = delta.find("tool_calls");
  if (tool_calls_it != delta.end() && tool_calls_it->is_array()) {
    for (const auto &tc_delta : *tool_calls_it) {
      int index = tc_delta.value("index", 0);
      auto &ptc = state.partial_tool_calls[index];
      ptc.index = index;

      if (auto id_it = tc_delta.find("id");
          id_it != tc_delta.end() && id_it->is_string() && ptc.id.empty()) {
        ptc.id = id_it.value().get<std::string>();
      }

      if (auto fn_it = tc_delta.find("function"); fn_it != tc_delta.end()) {
        if (auto name_it = fn_it->find("name"); name_it != fn_it->end() &&
                                                name_it->is_string() &&
                                                ptc.name.empty()) {
          ptc.name = name_it.value().get<std::string>();
        }
      }

      if (!ptc.content_index.has_value()) {
        // Each tool call gets its own content block, independent of any
        // other tool call's deltas, so interleaved/parallel tool calls
        // don't clobber each other's arguments.
        if (state.current_block == BlockType::text ||
            state.current_block == BlockType::thinking) {
          finish_current_block(state);
        }
        ptc.content_index = result.content.size();
        ToolCall new_tc;
        new_tc.id = ptc.id;
        new_tc.name = ptc.name;
        result.content.emplace_back(std::move(new_tc));
        state.current_block = BlockType::tool_call;
        if (state.on_event) {
          state.on_event(AssistantMessageToolCallStartEvent{
              .content_index = *ptc.content_index, .partial = result});
        }
      } else {
        // Name/id may arrive after the block was created from an
        // arguments-only delta.
        auto &existing_tc =
            std::get<ToolCall>(result.content[*ptc.content_index]);
        if (existing_tc.id.empty() && !ptc.id.empty())
          existing_tc.id = ptc.id;
        if (existing_tc.name.empty() && !ptc.name.empty())
          existing_tc.name = ptc.name;
      }

      if (auto fn_it = tc_delta.find("function"); fn_it != tc_delta.end()) {
        if (auto args_it = fn_it->find("arguments");
            args_it != fn_it->end() && args_it->is_string()) {
          std::string delta_str = args_it.value().get<std::string>();
          if (!delta_str.empty()) {
            ptc.partial_args += delta_str;
            auto &cur_tc =
                std::get<ToolCall>(result.content[*ptc.content_index]);
            cur_tc.partial_json = ptc.partial_args;

            if (state.on_event) {
              state.on_event(AssistantMessageToolCallDeltaEvent{
                  .content_index = *ptc.content_index,
                  .delta = delta_str,
                  .partial = result});
            }
          }
        }
      }
    }
  }
}

} // namespace

OpenAICompatibleClient::OpenAICompatibleClient(std::string base_url,
                                               std::string model_id)
    : base_url_(std::move(base_url)), model_id_(std::move(model_id)) {}

OpenAICompletionsCompat
OpenAICompatibleClient::detect_compat(const Model &model) {
  const auto &provider = model.provider;
  const auto &base_url = model.base_url;

  bool is_zai = provider == "zai" || str_contains(base_url, "api.z.ai");
  bool is_moonshot = provider == "moonshotai" || provider == "moonshotai-cn" ||
                     str_contains(base_url, "api.moonshot.");
  bool is_cloudflare_workers = provider == "cloudflare-workers-ai" ||
                               str_contains(base_url, "api.cloudflare.com");
  bool is_cloudflare_gateway =
      provider == "cloudflare-ai-gateway" ||
      str_contains(base_url, "gateway.ai.cloudflare.com");
  bool is_llamacpp = provider == "llamacpp" || provider == "llama.cpp" ||
                     str_contains(base_url, "llamacpp");
  bool is_local = provider == "local" || is_local_endpoint(base_url);

  bool is_fireworks =
      provider == "fireworks" || str_contains(base_url, "fireworks.ai");

  bool is_non_standard =
      provider == "cerebras" || str_contains(base_url, "cerebras.ai") ||
      provider == "xai" || str_contains(base_url, "api.x.ai") ||
      str_contains(base_url, "chutes.ai") ||
      str_contains(base_url, "deepseek.com") || is_zai || is_moonshot ||
      provider == "opencode" || str_contains(base_url, "opencode.ai") ||
      is_cloudflare_workers || is_cloudflare_gateway || is_llamacpp ||
      is_local || is_fireworks;

  bool use_max_tokens = str_contains(base_url, "chutes.ai") || is_moonshot ||
                        is_cloudflare_gateway || is_llamacpp || is_local ||
                        is_fireworks;

  bool is_grok = provider == "xai" || str_contains(base_url, "api.x.ai");
  bool is_deepseek =
      provider == "deepseek" || str_contains(base_url, "deepseek.com");
  bool is_openrouter =
      provider == "openrouter" || str_contains(base_url, "openrouter.ai");

  std::string cache_control_format;
  if (is_openrouter && model.id.starts_with("anthropic/")) {
    cache_control_format = "anthropic";
  }

  std::string thinking_format;
  if (is_deepseek) {
    thinking_format = "deepseek";
  } else if (is_zai) {
    thinking_format = "zai";
  } else if (is_openrouter) {
    thinking_format = "openrouter";
  } else {
    thinking_format = "openai";
  }

  OpenAICompletionsCompat compat;
  compat.supports_store = !is_non_standard;
  compat.supports_developer_role = !is_non_standard;
  compat.supports_reasoning_effort = !is_grok && !is_zai && !is_moonshot &&
                                     !is_cloudflare_gateway && !is_llamacpp &&
                                     !is_local;
  compat.supports_usage_in_streaming = !is_llamacpp && !is_local;
  compat.max_tokens_field =
      use_max_tokens ? "max_tokens" : "max_completion_tokens";
  compat.requires_tool_result_name = false;
  compat.requires_assistant_after_tool_result = false;
  compat.requires_thinking_as_text = false;
  compat.thinking_format = std::move(thinking_format);
  compat.supports_strict_mode =
      !is_moonshot && !is_cloudflare_gateway && !is_llamacpp && !is_local;
  compat.cache_control_format = std::move(cache_control_format);
  compat.disables_thinking_by_default = is_llamacpp || is_local;
  return compat;
}

StopReason OpenAICompatibleClient::map_finish_reason(std::string_view reason) {
  if (reason == "stop" || reason == "end")
    return StopReason::stop;
  if (reason == "length")
    return StopReason::length;
  if (reason == "tool_calls" || reason == "function_call")
    return StopReason::tool_use;
  return StopReason::error;
}

nlohmann::json
OpenAICompatibleClient::build_request_json(const Model &model,
                                           const AgentContext &context,
                                           const StreamOptions &options) {

  auto compat = detect_compat(model);
  auto messages = convert_messages(model, context, compat);

  nlohmann::json tools_arr = nlohmann::json::array();
  if (!context.tools.empty()) {
    for (const auto &t : context.tools) {
      auto schema_str = t->schema().serialize();
      nlohmann::json schema = nlohmann::json::parse(schema_str, nullptr, false);
      if (schema.is_discarded())
        schema = nlohmann::json::object();
      nlohmann::json function = {{"name", std::string(t->name())},
                                 {"description", std::string(t->description())},
                                 {"parameters", schema}};
      if (compat.supports_strict_mode) {
        function["strict"] = false;
      }
      tools_arr.push_back({{"type", "function"}, {"function", function}});
    }
  }

  nlohmann::json params = {{"model", model.id},
                           {"messages", messages},
                           {"stream", !compat.uses_non_streaming}};
  if (compat.supports_usage_in_streaming) {
    params["stream_options"] = {{"include_usage", true}};
  }
  if (compat.disables_thinking_by_default) {
    params["chat_template_kwargs"] = {{"enable_thinking", false}};
  }
  if (compat.supports_store)
    params["store"] = false;
  // Meta's Chat Completions backend only reliably reuses a cached prefix
  // across requests that share a routing key (verified live: identical
  // 803-token prefixes hit 0% without this key, ~78% with it). One
  // constant key for all pici sessions, per prompt_caching.md's "don't
  // over-partition" guidance — this is not sent on the Messages API,
  // which rejects it with HTTP 400 (also verified live). Matched by
  // base_url rather than model.provider since Meta's registry entries
  // use different provider strings ("meta", "meta-chat") to stay
  // separately selectable.
  if (str_contains(model.base_url, "api.meta.ai")) {
    params["prompt_cache_key"] = "pici";
  }
  if (options.max_tokens) {
    params[compat.max_tokens_field] = *options.max_tokens;
  }
  if (options.temperature)
    params["temperature"] = *options.temperature;
  if (!tools_arr.empty())
    params["tools"] = tools_arr;

  if (model.reasoning && options.reasoning != ThinkingLevel::off &&
      compat.supports_reasoning_effort) {
    params["reasoning_effort"] =
        std::string(thinking_level_to_string(options.reasoning));
  }

  if (options.on_payload) {
    auto next = options.on_payload(params, model);
    if (next)
      params = *next;
  }

  return params;
}

std::shared_ptr<AssistantMessage>
OpenAICompatibleClient::stream(const Model &model, const AgentContext &context,
                               const StreamOptions &options,
                               AssistantEventCallback on_event,
                               std::stop_token stop_tok) {

  auto result = std::make_shared<AssistantMessage>();
  result->api = model.api;
  result->provider = model.provider;
  result->model = model.id;
  result->stop_reason = StopReason::stop;
  result->timestamp = now_ms();

  auto compat = detect_compat(model);
  auto request_json = build_request_json(model, context, options);
  auto request_body = request_json.dump(2);

  if (options.verbose) {
    std::string dbg = "[request] POST ";
    dbg += base_url_.empty() ? model.base_url : base_url_;
    dbg += "/chat/completions\n";
    dbg += request_json.dump(2);
    dbg += '\n';
    ::write(STDERR_FILENO, dbg.data(), dbg.size());
  }

  request_body = request_json.dump();

  std::map<std::string, std::string> headers = options.headers;
  headers["Content-Type"] = "application/json";
  headers["Accept"] =
      compat.uses_non_streaming ? "application/json" : "text/event-stream";

  std::string url = base_url_.empty() ? model.base_url : base_url_;
  if (!url.empty() && url.back() == '/')
    url.pop_back();
  url += "/chat/completions";

  if (on_event)
    on_event(AssistantMessageStartEvent{*result});

  if (compat.uses_non_streaming) {
    auto response =
        HttpClient::post(url, request_body, headers, options.api_key,
                         options.timeout_ms, stop_tok);
    if (!response || response->status_code < 200 ||
        response->status_code >= 300 || stop_tok.stop_requested()) {
      result->stop_reason =
          stop_tok.stop_requested() ? StopReason::aborted : StopReason::error;
      result->error_message = stop_tok.stop_requested() ? "Request was aborted"
                                                        : "LLM request failed";
      if (response) {
        auto err = nlohmann::json::parse(response->body, nullptr, false);
        if (!err.is_discarded() && err.contains("error")) {
          const auto &error = err["error"];
          if (error.is_object()) {
            result->error_message =
                error.value("message", *result->error_message);
          } else if (error.is_string()) {
            result->error_message = error.get<std::string>();
          }
        } else if (!response->body.empty()) {
          result->error_message = response->body;
        }
      }
      if (on_event)
        on_event(AssistantMessageErrorEvent{.reason = result->stop_reason,
                                            .error = *result});
      return result;
    }

    auto body = nlohmann::json::parse(response->body, nullptr, false);
    if (body.is_discarded() || !body.contains("choices") ||
        !body["choices"].is_array() || body["choices"].empty()) {
      result->stop_reason = StopReason::error;
      result->error_message = "LLM response did not contain choices";
      if (on_event)
        on_event(AssistantMessageErrorEvent{.reason = result->stop_reason,
                                            .error = *result});
      return result;
    }

    if (auto id_it = body.find("id"); id_it != body.end() && id_it->is_string())
      result->response_id = id_it->get<std::string>();
    if (auto usage_it = body.find("usage");
        usage_it != body.end() && usage_it->is_object()) {
      parse_chunk_usage(*usage_it, result->usage);
    }

    const auto &choice = body["choices"][0];
    if (auto fr_it = choice.find("finish_reason");
        fr_it != choice.end() && !fr_it->is_null()) {
      result->stop_reason =
          OpenAICompatibleClient::map_finish_reason(fr_it->get<std::string>());
    }

    std::string text;
    if (auto msg_it = choice.find("message"); msg_it != choice.end()) {
      if (auto content_it = msg_it->find("content");
          content_it != msg_it->end() && content_it->is_string()) {
        text = content_it->get<std::string>();
      }
    }
    if (!text.empty()) {
      result->content.emplace_back(TextContent{.text = text});
      if (on_event) {
        on_event(AssistantMessageTextStartEvent{.content_index = 0,
                                                .partial = *result});
        on_event(AssistantMessageTextDeltaEvent{
            .content_index = 0, .delta = text, .partial = *result});
        on_event(AssistantMessageTextEndEvent{
            .content_index = 0, .content = text, .partial = *result});
      }
    }
    if (on_event)
      on_event(AssistantMessageDoneEvent{.reason = result->stop_reason,
                                         .message = *result});
    return result;
  }

  StreamingState state{.result = result,
                       .on_event = on_event,
                       .diagnostics = options.diagnostics};
  std::optional<std::string> response_error;

  bool ok = HttpClient::post_streaming(
      url, request_body,
      [&state, &response_error](const std::string &line) {
        if (!line.starts_with("data: ")) {
          auto err = nlohmann::json::parse(line, nullptr, false);
          if (!err.is_discarded() && err.contains("error")) {
            const auto &error = err["error"];
            if (error.is_object()) {
              response_error =
                  error.value("message", std::string("LLM request failed"));
            } else if (error.is_string()) {
              response_error = error.get<std::string>();
            }
          }
          return;
        }
        try {
          process_sse_line(line, state);
        } catch (const std::exception &e) {
          response_error = e.what();
        }
      },
      headers, options.api_key, options.timeout_ms, stop_tok,
      options.diagnostics);
  if (response_error) {
    ok = false;
  }

  for (auto &[idx, ptc] : state.partial_tool_calls) {
    if (!ptc.content_index.has_value())
      continue;
    auto &tc = std::get<ToolCall>(result->content[*ptc.content_index]);
    auto parsed = nlohmann::json::parse(ptc.partial_args, nullptr, false);
    tc.arguments = parsed.is_discarded() ? nlohmann::json::object() : parsed;
    tc.partial_json.clear();
    if (state.on_event) {
      state.on_event(AssistantMessageToolCallEndEvent{
          .content_index = *ptc.content_index,
          .tool_call = tc,
          .partial = *result});
    }
  }

  finish_current_block(state);

  if (!ok || stop_tok.stop_requested()) {
    result->stop_reason =
        stop_tok.stop_requested() ? StopReason::aborted : StopReason::error;
    if (stop_tok.stop_requested()) {
      result->error_message = "Request was aborted";
    } else {
      result->error_message =
          response_error.value_or("LLM streaming request failed");
    }
    if (on_event)
      on_event(AssistantMessageErrorEvent{.reason = result->stop_reason,
                                          .error = *result});
    return result;
  }

  if (on_event)
    on_event(AssistantMessageDoneEvent{.reason = result->stop_reason,
                                       .message = *result});
  return result;
}

} // namespace pi::core

void pi::core::register_openai_completions_client() {
  pi::core::LLMClientRegistry::instance().register_client(
      "openai-completions",
      [] { return std::make_shared<pi::core::OpenAICompatibleClient>(); });
}

namespace {
// NOLINTNEXTLINE(bugprone-throwing-static-initialization)
const bool registered = [] {
  pi::core::register_openai_completions_client();
  return true;
}();
} // namespace
