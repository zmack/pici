#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/message_types.h"

namespace pi::core {

// Callback type for streaming events
using StreamCallback = std::function<void(const AgentEvent&)>;

// ─── LLMClient: Abstraction over LLM API calls ─────────────────────────────

// Each provider (OpenAI, Anthropic, etc.) implements this interface.
// The actual implementation is in src/http/http_client.h for HTTP providers.

class LLMClient {
public:
    virtual ~LLMClient() = default;

    // Stream an assistant response. Emits events via the callback.
    // Returns the final assistant message.
    virtual std::shared_ptr<AssistantMessage> stream(
        const Model& model,
        const AgentContext& context,
        ThinkingLevel thinking_level,
        StreamCallback emit,
        const std::optional<std::string>& api_key,
        std::stop_token stop_tok = std::stop_token{}) = 0;

    // Get the provider name (e.g., "openai", "anthropic")
    virtual std::string_view provider_name() const = 0;

    // Get the API identifier (e.g., "openai-completions")
    virtual std::string_view api_id() const = 0;

    // Check if this client can handle the given model
    static std::shared_ptr<LLMClient> create(const Model& model);
};

// ─── LLMClient registry ────────────────────────────────────────────────────

class LLMClientRegistry {
public:
    // Register a client factory for an API type
    using Factory = std::function<std::shared_ptr<LLMClient>()>;
    void register_client(std::string api_id, Factory factory);

    // Get a client for a model, or create the default
    std::shared_ptr<LLMClient> get_client(const Model& model);

    static LLMClientRegistry& instance();

private:
    LLMClientRegistry() = default;
    std::map<std::string, Factory> factories_;
    mutable std::mutex mutex_;
};

} // namespace pi::core
