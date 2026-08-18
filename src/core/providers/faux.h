#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stop_token>
#include <vector>

#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"

namespace pi::core {

class FauxClient : public LLMClient {
public:
  struct Script {
    std::vector<AssistantMessageEvent> events;
    std::optional<std::chrono::milliseconds> delay_between;
  };

  explicit FauxClient(std::vector<Script> scripts,
                      std::vector<CompactionResult> compact_results = {});

  std::shared_ptr<AssistantMessage> stream(const Model &model,
                                           const AgentContext &context,
                                           const StreamOptions &options,
                                           AssistantEventCallback on_event,
                                           std::stop_token stop_tok) override;

  // Pops the next queued CompactionResult in order, mirroring stream()'s
  // scripted-response behavior. Returns an unsupported/error result if the
  // queue is exhausted.
  CompactionResult compact(const Model &model, const AgentContext &context,
                           const CompactionOptions &options,
                           std::stop_token stop_tok) override;

  std::string_view provider_name() const override { return "faux"; }
  std::string_view api_id() const override { return "faux"; }

private:
  std::vector<Script> scripts_;
  std::atomic<std::size_t> call_count_{0};
  std::vector<CompactionResult> compact_results_;
  std::atomic<std::size_t> compact_call_count_{0};
};

} // namespace pi::core
