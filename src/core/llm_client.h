#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/message_types.h"

namespace pi::core {

// Callback type for streaming events
using StreamCallback = std::function<void(const AgentEvent&)>;
using AssistantEventCallback = std::function<void(const AssistantMessageEvent&)>;

struct StreamOptions {
    std::optional<double> temperature;
    std::optional<std::uint32_t> max_tokens;
    std::optional<std::string> api_key;
    ThinkingLevel reasoning{ThinkingLevel::off};
    std::optional<std::string> cache_retention;
    std::optional<std::string> session_id;
    Transport transport{Transport::auto_transport};
    std::map<std::string, std::string> headers;
    std::optional<std::uint32_t> timeout_ms;
    std::optional<std::uint32_t> max_retries;
    std::optional<std::uint32_t> max_retry_delay_ms;
    nlohmann::json metadata;
    std::function<std::optional<nlohmann::json>(
        const nlohmann::json& payload, const Model&)> on_payload;
    std::function<void(int status,
                       const std::map<std::string, std::string>&,
                       const Model&)> on_response;
};

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
        const StreamOptions& options,
        AssistantEventCallback on_event,
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
