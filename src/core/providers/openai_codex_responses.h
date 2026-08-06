#pragma once

#include "core/agent_state.h"
#include "core/llm_client.h"

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

class OpenAICodexResponsesClient final : public LLMClient {
public:
  std::shared_ptr<AssistantMessage> stream(const Model &, const AgentContext &,
                                           const StreamOptions &,
                                           AssistantEventCallback,
                                           std::stop_token) override;

  std::string_view provider_name() const override { return "openai-codex"; }
  std::string_view api_id() const override { return "openai-codex-responses"; }

  static nlohmann::json build_request_json(const Model &, const AgentContext &,
                                           const StreamOptions &);
  static std::string endpoint_url(std::string base_url);
};

void register_openai_codex_responses_client();

} // namespace pi::core
