#include "core/message_types.h"
#include "core/tool_validation.h"
#include "nlohmann/json_fwd.hpp"

#include <concepts>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace pi::core {

std::string_view stop_reason_to_string(StopReason reason) {
  switch (reason) {
  case StopReason::stop:
    return "stop";
  case StopReason::length:
    return "length";
  case StopReason::tool_use:
    return "toolUse";
  case StopReason::error:
    return "error";
  case StopReason::aborted:
    return "aborted";
  }
  return "unknown";
}

StopReason stop_reason_from_string(std::string_view s) {
  if (s == "stop")
    return StopReason::stop;
  if (s == "length")
    return StopReason::length;
  if (s == "toolUse")
    return StopReason::tool_use;
  if (s == "error")
    return StopReason::error;
  if (s == "aborted")
    return StopReason::aborted;
  return StopReason::error;
}

std::string_view thinking_level_to_string(ThinkingLevel level) {
  switch (level) {
  case ThinkingLevel::off:
    return "off";
  case ThinkingLevel::minimal:
    return "minimal";
  case ThinkingLevel::low:
    return "low";
  case ThinkingLevel::medium:
    return "medium";
  case ThinkingLevel::high:
    return "high";
  case ThinkingLevel::xhigh:
    return "xhigh";
  }
  return "unknown";
}

ThinkingLevel thinking_level_from_string(std::string_view s) {
  if (s == "off")
    return ThinkingLevel::off;
  if (s == "minimal")
    return ThinkingLevel::minimal;
  if (s == "low")
    return ThinkingLevel::low;
  if (s == "medium")
    return ThinkingLevel::medium;
  if (s == "high")
    return ThinkingLevel::high;
  if (s == "xhigh")
    return ThinkingLevel::xhigh;
  return ThinkingLevel::off;
}

std::ostream &operator<<(std::ostream &os, StopReason reason) {
  os << stop_reason_to_string(reason);
  return os;
}

std::ostream &operator<<(std::ostream &os, ThinkingLevel level) {
  os << thinking_level_to_string(level);
  return os;
}

std::ostream &operator<<(std::ostream &os, ToolExecutionMode mode) {
  switch (mode) {
  case ToolExecutionMode::sequential:
    os << "sequential";
    break;
  case ToolExecutionMode::parallel:
    os << "parallel";
    break;
  }
  return os;
}

std::optional<std::string>
JsonSchemaToolSchema::validate_arguments(ToolArguments &arguments) const {
  // validate() mutates arguments in-place for coercion; const_cast is safe
  // here because JsonSchemaToolSchema is specifically designed for coercion.
  return ToolValidator::validate("(schema)", serialize(), arguments);
}

namespace json {

using nlohmann::json;

static json content_block_to_json(const ContentBlock &block) {
  return std::visit(
      []<typename T>(const T &value) -> json {
        if constexpr (std::same_as<T, TextContent>) {
          json j = json::object();
          j["type"] = "text";
          j["text"] = value.text;
          if (value.text_signature) {
            j["textSignature"] = *value.text_signature;
          }
          return j;
        } else if constexpr (std::same_as<T, ThinkingContent>) {
          json j = json::object();
          j["type"] = "thinking";
          j["thinking"] = value.thinking;
          if (value.thinking_signature) {
            j["thinkingSignature"] = *value.thinking_signature;
          }
          j["redacted"] = value.redacted;
          return j;
        } else if constexpr (std::same_as<T, ImageContent>) {
          json j = json::object();
          j["type"] = "image";
          j["data"] = value.data;
          j["mimeType"] = value.mime_type;
          return j;
        } else if constexpr (std::same_as<T, ToolCall>) {
          json j = json::object();
          j["type"] = "toolCall";
          j["id"] = value.id;
          j["name"] = value.name;
          j["arguments"] =
              value.arguments.is_object() ? value.arguments : json::object();
          return j;
        }
        return json{};
      },
      block);
}

static std::optional<ContentBlock> json_to_content_block(const json &j) {
  if (!j.is_object())
    return std::nullopt;
  auto type_it = j.find("type");
  if (type_it == j.end() || !type_it->is_string())
    return std::nullopt;
  std::string type = type_it->get<std::string>();

  if (type == "text") {
    TextContent tc;
    tc.text = j.value("text", "");
    if (j.contains("textSignature") && j["textSignature"].is_string()) {
      tc.text_signature = j["textSignature"].get<std::string>();
    }
    return tc;
  }
  if (type == "thinking") {
    ThinkingContent tc;
    tc.thinking = j.value("thinking", "");
    if (j.contains("thinkingSignature") && j["thinkingSignature"].is_string()) {
      tc.thinking_signature = j["thinkingSignature"].get<std::string>();
    }
    tc.redacted = j.value("redacted", false);
    return tc;
  }
  if (type == "image") {
    ImageContent ic;
    ic.data = j.value("data", "");
    ic.mime_type = j.value("mimeType", "");
    return ic;
  }
  if (type == "toolCall") {
    ToolCall tc;
    tc.id = j.value("id", "");
    tc.name = j.value("name", "");
    if (j.contains("arguments") && j["arguments"].is_object()) {
      tc.arguments = j["arguments"];
    } else {
      tc.arguments = json::object();
    }
    return tc;
  }
  return std::nullopt;
}

// Build a JSON object for a Message. Key order is explicitly controlled to
// match the output the existing tests expect (insertion order in nlohmann).
static json message_to_json_obj(const Message &msg) {
  return std::visit(
      []<typename T>(const T &m) -> json {
        if constexpr (std::same_as<T, UserMessage>) {
          json j = json::object();
          j["role"] = "user";
          j["timestamp"] = m.timestamp;
          json arr = json::array();
          for (const auto &cb : m.content) {
            arr.push_back(content_block_to_json(cb));
          }
          j["content"] = std::move(arr);
          return j;
        } else if constexpr (std::same_as<T, AssistantMessage>) {
          json j = json::object();
          j["role"] = "assistant";
          j["api"] = m.api;
          j["provider"] = m.provider;
          j["model"] = m.model;
          j["stopReason"] = stop_reason_to_string(m.stop_reason);
          j["timestamp"] = m.timestamp;

          json arr = json::array();
          for (const auto &cb : m.content) {
            arr.push_back(content_block_to_json(cb));
          }
          j["content"] = std::move(arr);

          {
            json usage = json::object();
            usage["input"] = m.usage.input;
            usage["output"] = m.usage.output;
            usage["cacheRead"] = m.usage.cache_read;
            usage["cacheWrite"] = m.usage.cache_write;
            usage["totalTokens"] = m.usage.total_tokens;
            json cost = json::object();
            cost["input"] = m.usage.cost.input;
            cost["output"] = m.usage.cost.output;
            cost["cacheRead"] = m.usage.cost.cache_read;
            cost["cacheWrite"] = m.usage.cost.cache_write;
            cost["total"] = m.usage.cost.total;
            usage["cost"] = std::move(cost);
            j["usage"] = std::move(usage);
          }
          if (!m.response_model.empty()) {
            j["responseModel"] = m.response_model;
          }
          if (m.response_id.has_value()) {
            j["responseId"] = *m.response_id;
          }
          if (m.error_message.has_value()) {
            j["errorMessage"] = *m.error_message;
          }
          return j;
        } else if constexpr (std::same_as<T, ToolResultMessage>) {
          json j = json::object();
          j["role"] = "toolResult";
          j["toolCallId"] = m.tool_call_id;
          j["toolName"] = m.tool_name;
          j["isError"] = m.is_error;
          j["timestamp"] = m.timestamp;
          json arr = json::array();
          for (const auto &cb : m.content) {
            arr.push_back(content_block_to_json(cb));
          }
          j["content"] = std::move(arr);
          if (m.details.has_value()) {
            j["details"] = *m.details;
          }
          return j;
        }
        return json{};
      },
      msg);
}

std::string to_json(const TokenUsage &usage) {
  json j = json::object();
  j["input"] = usage.input;
  j["output"] = usage.output;
  j["cacheRead"] = usage.cache_read;
  j["cacheWrite"] = usage.cache_write;
  j["totalTokens"] = usage.total_tokens;
  return j.dump(2);
}

std::string to_json(const Message &msg) {
  return message_to_json_obj(msg).dump(2);
}

std::string to_jsonl_line(const Message &msg) {
  return message_to_json_obj(msg).dump();
}

std::string to_json(const Model &model) {
  json j = json::object();
  j["id"] = model.id;
  j["name"] = model.name;
  j["api"] = model.api;
  j["provider"] = model.provider;
  j["baseUrl"] = model.base_url;
  j["reasoning"] = model.reasoning;
  json arr = json::array();
  for (const auto &cap : model.input_capabilities) {
    arr.push_back(cap);
  }
  j["input"] = std::move(arr);
  j["contextWindow"] = model.context_window;
  j["maxTokens"] = model.max_tokens;
  return j.dump(2);
}

static Message from_json_message(const json &j) {
  std::string role = j.value("role", "");
  if (role == "user") {
    UserMessage msg;
    msg.timestamp = j.value("timestamp", std::int64_t{0});
    if (j.contains("content") && j["content"].is_array()) {
      for (const auto &cb : j["content"]) {
        auto block = json_to_content_block(cb);
        if (block)
          msg.content.push_back(*block);
      }
    }
    return msg;
  }
  if (role == "assistant") {
    AssistantMessage msg;
    msg.api = j.value("api", "");
    msg.provider = j.value("provider", "");
    msg.model = j.value("model", "");
    msg.stop_reason = stop_reason_from_string(j.value("stopReason", ""));
    msg.timestamp = j.value("timestamp", std::int64_t{0});
    if (j.contains("responseModel") && j["responseModel"].is_string()) {
      msg.response_model = j["responseModel"].get<std::string>();
    }
    if (j.contains("responseId") && j["responseId"].is_string()) {
      msg.response_id = j["responseId"].get<std::string>();
    }
    if (j.contains("errorMessage") && j["errorMessage"].is_string()) {
      msg.error_message = j["errorMessage"].get<std::string>();
    }
    if (j.contains("content") && j["content"].is_array()) {
      for (const auto &cb : j["content"]) {
        auto block = json_to_content_block(cb);
        if (block)
          msg.content.push_back(*block);
      }
    }
    if (j.contains("usage") && j["usage"].is_object()) {
      const auto &u = j["usage"];
      msg.usage.input = u.value("input", std::uint64_t{0});
      msg.usage.output = u.value("output", std::uint64_t{0});
      msg.usage.cache_read = u.value("cacheRead", std::uint64_t{0});
      msg.usage.cache_write = u.value("cacheWrite", std::uint64_t{0});
      msg.usage.total_tokens = u.value("totalTokens", std::uint64_t{0});
      if (u.contains("cost") && u["cost"].is_object()) {
        const auto &c = u["cost"];
        msg.usage.cost.input = c.value("input", 0.0);
        msg.usage.cost.output = c.value("output", 0.0);
        msg.usage.cost.cache_read = c.value("cacheRead", 0.0);
        msg.usage.cost.cache_write = c.value("cacheWrite", 0.0);
        msg.usage.cost.total = c.value("total", 0.0);
      }
    }
    return msg;
  }
  if (role == "toolResult") {
    ToolResultMessage msg;
    msg.tool_call_id = j.value("toolCallId", "");
    msg.tool_name = j.value("toolName", "");
    msg.is_error = j.value("isError", false);
    msg.timestamp = j.value("timestamp", std::int64_t{0});
    if (j.contains("content") && j["content"].is_array()) {
      for (const auto &cb : j["content"]) {
        auto block = json_to_content_block(cb);
        if (block)
          msg.content.push_back(*block);
      }
    }
    if (j.contains("details") && j["details"].is_string()) {
      msg.details = j["details"].get<std::string>();
    }
    return msg;
  }
  throw std::runtime_error("Unknown message role: " + role);
}

std::optional<Message> from_json(const std::string &s) {
  try {
    auto j = nlohmann::json::parse(s);
    return from_json_message(j);
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<Message> from_json(std::string_view s) {
  return from_json(std::string(s));
}

} // namespace json

void compute_cost(TokenUsage &usage, const Model::Cost &price) {
  usage.cost.input =
      static_cast<double>(usage.input) * price.input_per_mtok / 1'000'000.0;
  usage.cost.output =
      static_cast<double>(usage.output) * price.output_per_mtok / 1'000'000.0;
  usage.cost.cache_read = static_cast<double>(usage.cache_read) *
                          price.cache_read_per_mtok / 1'000'000.0;
  usage.cost.cache_write = static_cast<double>(usage.cache_write) *
                           price.cache_write_per_mtok / 1'000'000.0;
  usage.cost.total = usage.cost.input + usage.cost.output +
                     usage.cost.cache_read + usage.cost.cache_write;
}

} // namespace pi::core
