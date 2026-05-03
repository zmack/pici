#include "core/llm_client.h"

#include <mutex>

namespace pi::core {

// ─── Stub LLM Client (no-op fallback) ─────────────────────────────────────

class StubLLMClient : public LLMClient {
public:
    std::shared_ptr<AssistantMessage> stream(
        const Model& model,
        const AgentContext& context,
        const StreamOptions& options,
        StreamCallback emit,
        std::stop_token stop_tok) override {
        (void)model;
        (void)context;
        (void)options;
        (void)emit;
        (void)stop_tok;
        // Return a stub error message
        auto msg = std::make_shared<AssistantMessage>();
        msg->api = "none";
        msg->provider = "none";
        msg->model = "stub";
        msg->stop_reason = StopReason::error;
        msg->error_message = "No LLM client configured";
        msg->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now().time_since_epoch())
                             .count();
        return msg;
    }

    std::string_view provider_name() const override { return "stub"; }
    std::string_view api_id() const override { return "none"; }
};

// ─── LLMClientRegistry ────────────────────────────────────────────────────

void LLMClientRegistry::register_client(std::string api_id, Factory factory) {
    std::lock_guard lock(mutex_);
    factories_[std::move(api_id)] = std::move(factory);
}

std::shared_ptr<LLMClient>
LLMClientRegistry::get_client(const Model& model) {
    std::lock_guard lock(mutex_);
    auto it = factories_.find(model.api);
    if (it != factories_.end()) {
        return it->second();
    }
    // Default: return stub client
    return std::make_shared<StubLLMClient>();
}

LLMClientRegistry& LLMClientRegistry::instance() {
    static LLMClientRegistry reg;
    return reg;
}

// ─── LLMClient::create ────────────────────────────────────────────────────

std::shared_ptr<LLMClient> LLMClient::create(const Model& model) {
    return LLMClientRegistry::instance().get_client(model);
}

} // namespace pi::core
