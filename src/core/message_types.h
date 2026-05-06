#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <ostream>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// ─── Token usage and cost ───────────────────────────────────────────────────

namespace pi::core {

struct TokenUsage {
  std::uint64_t input{0};
  std::uint64_t output{0};
  std::uint64_t cache_read{0};
  std::uint64_t cache_write{0};
  std::uint64_t total_tokens{0};

  struct Cost {
    double input{0};
    double output{0};
    double cache_read{0};
    double cache_write{0};
    double total{0};
  } cost{};
};

// ─── Tool arguments ─────────────────────────────────────────────────────────
// Must hold a JSON object (not array/scalar) for tool calls.
using ToolArguments = nlohmann::json;

// ─── Content blocks ─────────────────────────────────────────────────────────

struct TextContent {
  static constexpr std::string_view type = "text";
  std::string text;
  std::optional<std::string> text_signature;
};

struct ThinkingContent {
  static constexpr std::string_view type = "thinking";
  std::string thinking;
  std::optional<std::string> thinking_signature;
  bool redacted{false};
};

struct ImageContent {
  static constexpr std::string_view type = "image";
  std::string data; // base64
  std::string mime_type;
};

struct ToolCall {
  static constexpr std::string_view type = "toolCall";
  std::string id;
  std::string name;
  ToolArguments arguments = nlohmann::json::object();
  // Temporary field used during JSON streaming
  std::string partial_json;
};

using ContentBlock =
    std::variant<TextContent, ThinkingContent, ImageContent, ToolCall>;
using ToolResultContentBlock = std::variant<TextContent, ImageContent>;

// ─── Message types ──────────────────────────────────────────────────────────

enum class StopReason {
  stop,
  length,
  tool_use,
  error,
  aborted,
};

std::string_view stop_reason_to_string(StopReason reason);
StopReason stop_reason_from_string(std::string_view s);

// Forward declarations
struct UserMessage;
struct AssistantMessage;
struct ToolResultMessage;

// ─── UserMessage ────────────────────────────────────────────────────────────

struct UserMessage {
  static constexpr std::string_view role = "user";
  std::vector<ContentBlock> content;
  std::int64_t timestamp{0};
};

// ─── AssistantMessage ───────────────────────────────────────────────────────

struct AssistantMessage {
  static constexpr std::string_view role = "assistant";
  std::vector<ContentBlock> content;
  std::string api;
  std::string provider;
  std::string model;
  std::string response_model;
  std::optional<std::string> response_id;
  TokenUsage usage{};
  StopReason stop_reason{StopReason::stop};
  std::optional<std::string> error_message;
  std::int64_t timestamp{0};
};

// ─── ToolResultMessage ──────────────────────────────────────────────────────

struct ToolResultMessage {
  static constexpr std::string_view role = "toolResult";
  std::string tool_call_id;
  std::string tool_name;
  std::vector<ContentBlock> content;
  std::optional<std::string> details;
  bool is_error{false};
  std::int64_t timestamp{0};
};

// ─── Message (discriminated union) ─────────────────────────────────────────

using Message = std::variant<UserMessage, AssistantMessage, ToolResultMessage>;

// ─── Model ──────────────────────────────────────────────────────────────────

enum class ThinkingLevel {
  off,
  minimal,
  low,
  medium,
  high,
  xhigh,
};

std::string_view thinking_level_to_string(ThinkingLevel level);
ThinkingLevel thinking_level_from_string(std::string_view s);

enum class ToolExecutionMode {
  sequential,
  parallel,
};

enum class Transport {
  sse,
  websocket,
  websocket_cached,
  auto_transport,
};

struct Model {
  std::string id;
  std::string name;
  std::string api;      // e.g. "openai-completions", "anthropic-messages"
  std::string provider; // e.g. "openai", "anthropic"
  std::string base_url;
  bool reasoning{false};
  std::vector<std::string> input_capabilities; // "text", "image"
  struct Cost {
    double input{0};
    double output{0};
    double cache_read{0};
    double cache_write{0};
  } cost{};
  std::uint64_t context_window{0};
  std::uint64_t max_tokens{0};
  std::map<std::string, std::string> headers;
  // Maps thinking levels to provider-specific values
  std::map<std::string, std::optional<std::string>> thinking_level_map;

  bool operator==(const Model &) const = delete;
};

// ─── Tool ───────────────────────────────────────────────────────────────────

class ToolSchema {
public:
  virtual ~ToolSchema() = default;
  virtual std::string serialize() const = 0;
  // Returns a JSON-like map representation for the tool definition
  virtual std::map<std::string, std::string> to_definition() const = 0;
  // Returns nullopt on success, error string on failure.
  // Non-const to allow coercion mutations via validate_arguments.
  virtual std::optional<std::string>
  validate_arguments(ToolArguments &arguments) const {
    (void)arguments;
    return std::nullopt;
  }
};

class ToolResult {
public:
  virtual ~ToolResult() = default;
  virtual bool is_error() const = 0;
  virtual std::string content() const = 0;
  virtual std::vector<ToolResultContentBlock> content_blocks() const {
    auto text = content();
    if (text.empty()) {
      return {};
    }
    return {TextContent{.text = std::move(text)}};
  }
  virtual std::optional<std::string> details() const = 0;
  virtual bool terminate() const { return false; }
};

using ToolUpdateCallback = std::function<void(std::shared_ptr<ToolResult>)>;

class ToolDefinition {
public:
  virtual ~ToolDefinition() = default;
  virtual std::string_view name() const = 0;
  virtual std::string_view description() const = 0;
  // Returns "builtin" for C++ tools, or the source file path for Lua tools.
  virtual std::string_view source_path() const { return "builtin"; }
  virtual ToolSchema &schema() const = 0;
  virtual ToolArguments
  prepare_arguments(const ToolArguments &arguments) const {
    return arguments;
  }
  // Execute the tool
  virtual std::shared_ptr<ToolResult>
  execute(std::string_view call_id, std::string_view args_json,
          std::stop_token stop_tok = std::stop_token{},
          ToolUpdateCallback on_update = {}) const = 0;
  // Per-tool execution mode override
  virtual ToolExecutionMode execution_mode() const {
    return ToolExecutionMode::parallel;
  }
};

// ─── JsonSchemaToolSchema ───────────────────────────────────────────────────
// Base class for tools that validate via ToolValidator (json-schema-validator).
// Subclasses only need to implement serialize() and to_definition().
// validate_arguments applies TypeBox-style coercion before structural
// validation.
class JsonSchemaToolSchema : public ToolSchema {
public:
  std::optional<std::string>
  validate_arguments(ToolArguments &arguments) const override;
};

// ─── JSON helpers ───────────────────────────────────────────────────────────

namespace json {

std::string to_json(const TokenUsage &usage);
std::string to_json(const Message &msg);
std::string to_json(const Model &model);

// Parse a Message from JSON
std::optional<Message> from_json(const std::string &s);
std::optional<Message> from_json(std::string_view s);

} // namespace json

} // namespace pi::core

namespace pi::core {

std::ostream &operator<<(std::ostream &os, StopReason reason);
std::ostream &operator<<(std::ostream &os, ThinkingLevel level);
std::ostream &operator<<(std::ostream &os, ToolExecutionMode mode);

} // namespace pi::core
