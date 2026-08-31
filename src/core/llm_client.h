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
#include "core/auth_types.h"
#include "core/event_types.h"
#include "core/message_types.h"
#include "core/stream_diagnostics.h"

namespace pi::core {

// Callback type for streaming events
using StreamCallback = std::function<void(const AgentEvent &)>;
using AssistantEventCallback =
    std::function<void(const AssistantMessageEvent &)>;

struct StreamOptions {
  std::optional<double> temperature;
  std::optional<std::uint32_t> max_tokens;
  std::optional<RequestAuth> auth;
  // Kept for source compatibility with providers that have not migrated to
  // RequestAuth yet. New clients should consume auth.
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
  std::function<std::optional<nlohmann::json>(const nlohmann::json &payload,
                                              const Model &)>
      on_payload;
  std::function<void(int status, const std::map<std::string, std::string> &,
                     const Model &)>
      on_response;
  std::shared_ptr<StreamDiagnostics> diagnostics;
  bool verbose{false};
};

// Request concerns for a unary provider-owned compaction call. Mirrors
// StreamOptions' request-shaping fields; a compact request is not a stream,
// so transport/cache_retention/max_tokens/temperature do not apply.
struct CompactionOptions {
  std::optional<RequestAuth> auth;
  // Kept for source compatibility with providers that have not migrated to
  // RequestAuth yet. New clients should consume auth.
  std::optional<std::string> api_key;
  ThinkingLevel reasoning{ThinkingLevel::off};
  std::optional<std::string> session_id;
  std::map<std::string, std::string> headers;
  std::optional<std::uint32_t> timeout_ms;
  nlohmann::json metadata;
  std::function<std::optional<nlohmann::json>(const nlohmann::json &payload,
                                              const Model &)>
      on_payload;
  std::function<void(int status, const std::map<std::string, std::string> &,
                     const Model &)>
      on_response;
  std::shared_ptr<StreamDiagnostics> diagnostics;
  bool verbose{false};
};

// Provider-neutral result of a unary compaction request. The replacement
// transcript is already typed `Message`s (including any opaque
// ContextCompactionMessage the provider returned) — callers should not need
// to touch provider-specific JSON. `messages` is the raw, parsed output in
// provider order; retention filtering (which items to keep) is a separate
// concern (see CompactionManager) so a client implementation is not
// responsible for applying that policy.
struct CompactionResult {
  // False only for the default LLMClient implementation (provider does not
  // expose a remote compaction endpoint at all). A provider that supports
  // compaction but fails a particular request should leave this true and
  // set error_message instead, mirroring how stream() reports failure via
  // AssistantMessage::error_message rather than a distinct "unsupported"
  // signal.
  bool supported{true};
  std::vector<Message> messages;
  std::optional<std::string> response_id;
  TokenUsage usage;
  std::optional<std::string> error_message;
  bool cancelled{false};
  // HTTP status of the provider's response, when one was received at all.
  // Unset means the request never got a response (a transient transport
  // failure); set distinguishes a retryable server error (429/5xx) from a
  // non-retryable client error (4xx other than 429) for the retry policy in
  // core/compaction.h. Not part of the original Phase 2 contract — added in
  // Phase 3 because CompactionResult::error_message alone (a free-form
  // string) is not a reliable signal to retry on.
  std::optional<int> http_status;
};

// Each provider (OpenAI, Anthropic, etc.) implements this interface.
// The actual implementation is in src/http/http_client.h for HTTP providers.

class LLMClient {
public:
  virtual ~LLMClient() = default;

  // Stream an assistant response. Emits events via the callback.
  // Returns the final assistant message.
  virtual std::shared_ptr<AssistantMessage>
  stream(const Model &model, const AgentContext &context,
         const StreamOptions &options, AssistantEventCallback on_event,
         std::stop_token stop_tok = std::stop_token{}) = 0;

  // Send the current transcript to a provider-owned compaction endpoint and
  // return the replacement transcript it proposes. The default
  // implementation reports the operation as unsupported rather than
  // throwing, so callers can route to a local fallback uniformly.
  // stop_tok is by value to match stream()'s override signature convention
  // (every LLMClient override takes std::stop_token by value); this default
  // body just discards it.
  virtual CompactionResult compact(
      const Model &model, const AgentContext &context,
      const CompactionOptions &options,
      std::stop_token stop_tok = // NOLINT(performance-unnecessary-value-param)
      std::stop_token{}) {
    (void)model;
    (void)context;
    (void)options;
    (void)stop_tok;
    CompactionResult result;
    result.supported = false;
    result.error_message =
        "remote compaction is not supported by this provider";
    return result;
  }

  // Get the provider name (e.g., "openai", "anthropic")
  virtual std::string_view provider_name() const = 0;

  // Get the API identifier (e.g., "openai-completions")
  virtual std::string_view api_id() const = 0;

  // Check if this client can handle the given model
  static std::shared_ptr<LLMClient> create(const Model &model);
};

class LLMClientRegistry {
public:
  // Register a client factory for an API type
  using Factory = std::function<std::shared_ptr<LLMClient>()>;
  void register_client(std::string api_id, Factory factory);

  // Get a client for a model, or create the default
  std::shared_ptr<LLMClient> get_client(const Model &model);

  // Read-only validation hook used when building the effective model
  // registry at startup.
  bool has_client(std::string_view api_id) const;

  static LLMClientRegistry &instance();

private:
  LLMClientRegistry() = default;
  std::map<std::string, Factory> factories_;
  mutable std::mutex mutex_;
};

// Process-owned inference adapter collection. This is the injectable seam
// for Provider::inference while LLMClientRegistry remains a compatibility
// singleton during the taxonomy migration.
class InferenceAdapterCollection {
public:
  explicit InferenceAdapterCollection(
      LLMClientRegistry &registry = LLMClientRegistry::instance())
      : registry_(&registry) {}

  void register_adapter(std::string adapter_id,
                        LLMClientRegistry::Factory factory);
  bool has_adapter(std::string_view adapter_id) const;
  std::shared_ptr<LLMClient> get_client(const Model &model) const;

private:
  LLMClientRegistry *registry_;
  mutable std::mutex mutex_;
  std::map<std::string, LLMClientRegistry::Factory> factories_;
};

} // namespace pi::core
