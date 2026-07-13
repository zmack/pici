#pragma once

#include "core/agent_state.h"
#include "core/llm_client.h"
#include "core/message_types.h"

#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace pi::core {

// Small, directly testable parser for the Anthropic-style Messages SSE
// protocol. The parser preserves encrypted reasoning blocks without exposing
// their payload as visible text.
class MuseMessagesSseParser {
public:
  MuseMessagesSseParser(std::shared_ptr<AssistantMessage> result,
                        AssistantEventCallback on_event = {},
                        std::shared_ptr<StreamDiagnostics> diagnostics = {});

  void feed_line(std::string_view line);
  void finish();

  const std::optional<std::string> &error() const { return error_; }

private:
  enum class BlockKind {
    ignored,
    text,
    thinking,
    redacted_thinking,
    tool_call
  };

  struct BlockState {
    BlockKind kind{BlockKind::ignored};
    std::optional<std::size_t> content_index;
    std::string partial_json;
    bool finished{false};
  };

  std::shared_ptr<AssistantMessage> result_;
  AssistantEventCallback on_event_;
  std::shared_ptr<StreamDiagnostics> diagnostics_;
  std::map<std::size_t, BlockState> blocks_;
  std::optional<std::size_t> active_block_index_;
  std::string event_name_;
  std::optional<std::string> error_;
  bool done_emitted_{false};

  void process_data(std::string_view data);
  void start_block(const nlohmann::json &event);
  void process_delta(const nlohmann::json &event);
  void stop_block(const nlohmann::json &event);
  void finish_block(std::size_t protocol_index);
  void finish_text_block(std::size_t protocol_index);
  void finish_thinking_block(std::size_t protocol_index);
  void finish_tool_call_block(std::size_t protocol_index);
  void emit_done();
};

class MuseMessagesClient : public LLMClient {
public:
  MuseMessagesClient() = default;
  explicit MuseMessagesClient(std::string base_url, std::string model_id = "");

  std::shared_ptr<AssistantMessage> stream(const Model &model,
                                           const AgentContext &context,
                                           const StreamOptions &options,
                                           AssistantEventCallback on_event,
                                           std::stop_token stop_tok) override;

  std::string_view provider_name() const override { return "meta"; }
  std::string_view api_id() const override { return "muse-messages"; }

  static nlohmann::json build_request_json(const Model &model,
                                           const AgentContext &context,
                                           const StreamOptions &options);

  static StopReason map_stop_reason(std::string_view stop_reason);

private:
  std::string base_url_;
  std::string model_id_;
};

void register_muse_messages_client();

} // namespace pi::core
