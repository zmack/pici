#include "core/llm_client.h"
#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/message_types.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string_view>
#include <utility>

namespace pi::core {


class StubLLMClient : public LLMClient {
public:
  std::shared_ptr<AssistantMessage> stream(const Model &model,
                                           const AgentContext &context,
                                           const StreamOptions &options,
                                           AssistantEventCallback on_event,
                                           std::stop_token stop_tok) override {
    (void)model;
    (void)context;
    (void)options;
    (void)stop_tok;
    auto msg = std::make_shared<AssistantMessage>();
    msg->api = "none";
    msg->provider = "none";
    msg->model = "stub";
    msg->stop_reason = StopReason::error;
    msg->error_message = "No LLM client configured";
    msg->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();
    if (on_event) {
      on_event(AssistantMessageErrorEvent{.reason = StopReason::error,
                                          .error = *msg});
    }
    return msg;
  }

  std::string_view provider_name() const override { return "stub"; }
  std::string_view api_id() const override { return "none"; }
};


void LLMClientRegistry::register_client(std::string api_id, Factory factory) {
  std::scoped_lock lock(mutex_);
  factories_[std::move(api_id)] = std::move(factory);
}

std::shared_ptr<LLMClient> LLMClientRegistry::get_client(const Model &model) {
  std::scoped_lock lock(mutex_);
  auto it = factories_.find(model.api);
  if (it != factories_.end()) {
    return it->second();
  }
  // Default: return stub client
  return std::make_shared<StubLLMClient>();
}

LLMClientRegistry &LLMClientRegistry::instance() {
  static LLMClientRegistry reg;
  return reg;
}


std::shared_ptr<LLMClient> LLMClient::create(const Model &model) {
  return LLMClientRegistry::instance().get_client(model);
}

} // namespace pi::core
