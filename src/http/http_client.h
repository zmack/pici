#pragma once

#ifdef PI_CPP_HAVE_HTTP

#include <concepts>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "core/agent_loop.h"
#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/stream.h"

namespace pi::core {

// ─── HTTP-based LLM client for OpenAI-compatible APIs ─────────────────────

class OpenAICompatibleClient : public LLMClient {
public:
    OpenAICompatibleClient(std::string base_url,
                           std::string model_id,
                           bool streaming = true);

    ~OpenAICompatibleClient() override = default;

    std::shared_ptr<AssistantMessage> stream(
        const Model& model,
        const AgentContext& context,
        ThinkingLevel thinking_level,
        StreamCallback emit,
        const std::optional<std::string>& api_key,
        std::stop_token stop_tok = std::stop_token{}) override;

    std::string_view provider_name() const override { return "openai-compatible"; }
    std::string_view api_id() const override { return "openai-completions"; }

private:
    std::string build_request_json(const AgentContext& context,
                                   ThinkingLevel thinking_level,
                                   const Model& model) const;
    void process_sse_chunk(const std::string& line,
                           AssistantMessage& partial,
                           StreamCallback emit);

    std::string base_url_;
    std::string model_id_;
    bool streaming_{true};
};

// ─── HTTP client utility ──────────────────────────────────────────────────

class HttpClient {
public:
    struct Response {
        int status_code{0};
        std::string body;
        std::map<std::string, std::string> headers;
    };

    static std::optional<Response> post(
        const std::string& url,
        const std::string& body,
        const std::map<std::string, std::string>& extra_headers = {},
        const std::optional<std::string>& api_key = std::nullopt,
        std::stop_token stop_tok = std::stop_token{});

    static std::optional<std::string> post_streaming(
        const std::string& url,
        const std::string& body,
        std::function<void(const std::string& line)> on_line,
        const std::map<std::string, std::string>& extra_headers = {},
        const std::optional<std::string>& api_key = std::nullopt,
        std::stop_token stop_tok = std::stop_token{});
};

} // namespace pi::core

#endif // PI_CPP_HAVE_HTTP
