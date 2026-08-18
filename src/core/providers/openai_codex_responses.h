#pragma once

#include "core/agent_state.h"
#include "core/llm_client.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace pi::core {

class OpenAICodexResponsesParser {
public:
  OpenAICodexResponsesParser(const Model &model,
                             AssistantEventCallback on_event);

  void feed_line(std::string_view line);
  void finish();

  const std::shared_ptr<AssistantMessage> &result() const { return result_; }
  bool failed() const { return failed_; }
  const std::string &error() const { return error_; }
  bool terminal_seen() const { return terminal_seen_; }

private:
  struct Slot {
    std::size_t content_index{0};
    std::string type;
    std::string item_id;
    std::string call_id;
    std::string partial_arguments;
    bool open{false};
  };

  void process_event(std::string_view event_name, const nlohmann::json &event);
  void fail(std::string message);
  void emit_error();
  void close_slot(Slot &slot, const nlohmann::json *item = nullptr);
  void reconcile_text(Slot &slot, std::string text);
  void parse_usage(const nlohmann::json &usage);

  Model model_;
  AssistantEventCallback on_event_;
  std::shared_ptr<AssistantMessage> result_;
  std::map<int, Slot> slots_;
  std::map<std::string, int> item_slots_;
  std::string pending_event_name_;
  bool terminal_seen_{false};
  bool failed_{false};
  bool emitted_error_{false};
  std::string error_;
};

// Parses the JSON body of a `/responses/compact` response
// (`{"output": [...]}`) into a CompactionResult. Pure/free so it is
// directly unit-testable without any HTTP involved, mirroring how
// OpenAICodexResponsesParser is tested by feeding it synthetic SSE lines.
// Throws std::runtime_error on a malformed/empty response; the caller is
// expected to catch it and report a failed CompactionResult.
CompactionResult parse_compact_response(const Model &model,
                                        const nlohmann::json &body);

class OpenAICodexResponsesClient final : public LLMClient {
public:
  std::shared_ptr<AssistantMessage> stream(const Model &, const AgentContext &,
                                           const StreamOptions &,
                                           AssistantEventCallback,
                                           std::stop_token) override;

  CompactionResult compact(const Model &, const AgentContext &,
                           const CompactionOptions &, std::stop_token) override;

  std::string_view provider_name() const override { return "openai-codex"; }
  std::string_view api_id() const override { return "openai-codex-responses"; }

  static nlohmann::json build_request_json(const Model &, const AgentContext &,
                                           const StreamOptions &);
  static nlohmann::json build_compact_request_json(const Model &,
                                                   const AgentContext &,
                                                   const CompactionOptions &);
  static std::string endpoint_url(std::string base_url);
  // Appends "/compact" to the Responses endpoint exactly once.
  static std::string compact_endpoint_url(std::string base_url);
  // Scales `configured` (or the same 600s default HttpClient falls back to)
  // by the Codex reference client's COMPACT_REQUEST_TIMEOUT_IDLE_MULTIPLIER
  // (4x), saturating at UINT32_MAX. Exposed so the multiplier is directly
  // unit-testable rather than only observable through a real timed-out HTTP
  // call.
  static std::uint32_t
  compact_request_timeout_ms(std::optional<std::uint32_t> configured);
};

void register_openai_codex_responses_client();

} // namespace pi::core
