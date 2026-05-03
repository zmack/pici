#pragma once
#include <memory>
#include <stop_token>
#include <string>
#include <nlohmann/json.hpp>
#include "core/agent_state.h"
#include "core/llm_client.h"
#include "core/message_types.h"

namespace pi::core {

struct OpenAICompletionsCompat {
    bool supports_store{true};
    bool supports_developer_role{true};
    bool supports_reasoning_effort{true};
    bool supports_usage_in_streaming{true};
    std::string max_tokens_field{"max_completion_tokens"};
    bool requires_tool_result_name{false};
    bool requires_assistant_after_tool_result{false};
    bool requires_thinking_as_text{false};
    std::string thinking_format{"openai"};
    bool supports_strict_mode{true};
    std::string cache_control_format;
};

class OpenAICompatibleClient : public LLMClient {
public:
    OpenAICompatibleClient() = default;
    explicit OpenAICompatibleClient(std::string base_url, std::string model_id = "");

    std::shared_ptr<AssistantMessage> stream(
        const Model& model,
        const AgentContext& context,
        const StreamOptions& options,
        AssistantEventCallback on_event,
        std::stop_token stop_tok) override;

    std::string_view provider_name() const override { return "openai-compatible"; }
    std::string_view api_id() const override { return "openai-completions"; }

    nlohmann::json build_request_json(
        const Model& model,
        const AgentContext& context,
        const StreamOptions& options) const;

    static StopReason map_finish_reason(std::string_view finish_reason);
    static OpenAICompletionsCompat detect_compat(const Model& model);

private:
    std::string base_url_;
    std::string model_id_;
};

} // namespace pi::core
