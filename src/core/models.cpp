#include "core/models.h"
#include "core/llm_client.h"
#include "core/message_types.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <shared_mutex>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pi::core {

// clang-format off

// OpenAI — https://openai.com/api/pricing/
const std::vector<Model> kModels = { // NOLINT(bugprone-throwing-static-initialization): the read-only model catalog is process-lifetime data.
  { .id="gpt-4o",       .name="GPT-4o",          .api="openai-completions", .provider="openai",
    .base_url="https://api.openai.com/v1", .reasoning=false,
    .cost={.input_per_mtok=2.50, .output_per_mtok=10.00, .cache_read_per_mtok=1.25, .cache_write_per_mtok=0},
    .context_window=128000, .max_tokens=16384 },
  { .id="gpt-4o-mini",  .name="GPT-4o Mini",     .api="openai-completions", .provider="openai",
    .base_url="https://api.openai.com/v1", .reasoning=false,
    .cost={.input_per_mtok=0.15, .output_per_mtok=0.60, .cache_read_per_mtok=0.075, .cache_write_per_mtok=0},
    .context_window=128000, .max_tokens=16384 },
  { .id="gpt-4.1",      .name="GPT-4.1",         .api="openai-completions", .provider="openai",
    .base_url="https://api.openai.com/v1", .reasoning=false,
    .cost={.input_per_mtok=2.00, .output_per_mtok=8.00, .cache_read_per_mtok=0.50, .cache_write_per_mtok=0},
    .context_window=1047576, .max_tokens=32768 },
  { .id="gpt-4.1-mini", .name="GPT-4.1 Mini",    .api="openai-completions", .provider="openai",
    .base_url="https://api.openai.com/v1", .reasoning=false,
    .cost={.input_per_mtok=0.40, .output_per_mtok=1.60, .cache_read_per_mtok=0.10, .cache_write_per_mtok=0},
    .context_window=1047576, .max_tokens=32768 },
  { .id="gpt-4.1-nano", .name="GPT-4.1 Nano",    .api="openai-completions", .provider="openai",
    .base_url="https://api.openai.com/v1", .reasoning=false,
    .cost={.input_per_mtok=0.10, .output_per_mtok=0.40, .cache_read_per_mtok=0.025, .cache_write_per_mtok=0},
    .context_window=1047576, .max_tokens=32768 },
  { .id="o1",           .name="o1",               .api="openai-completions", .provider="openai",
    .base_url="https://api.openai.com/v1", .reasoning=true,
    .cost={.input_per_mtok=15.00, .output_per_mtok=60.00, .cache_read_per_mtok=7.50, .cache_write_per_mtok=0},
    .context_window=200000,  .max_tokens=100000 },
  { .id="o1-mini",      .name="o1 Mini",          .api="openai-completions", .provider="openai",
    .base_url="https://api.openai.com/v1", .reasoning=true,
    .cost={.input_per_mtok=1.10, .output_per_mtok=4.40, .cache_read_per_mtok=0.55, .cache_write_per_mtok=0},
    .context_window=128000,  .max_tokens=65536 },
  { .id="o3",           .name="o3",               .api="openai-completions", .provider="openai",
    .base_url="https://api.openai.com/v1", .reasoning=true,
    .cost={.input_per_mtok=10.00, .output_per_mtok=40.00, .cache_read_per_mtok=2.50, .cache_write_per_mtok=0},
    .context_window=200000,  .max_tokens=100000 },
  { .id="o3-mini",      .name="o3 Mini",          .api="openai-completions", .provider="openai",
    .base_url="https://api.openai.com/v1", .reasoning=true,
    .cost={.input_per_mtok=1.10, .output_per_mtok=4.40, .cache_read_per_mtok=0.55, .cache_write_per_mtok=0},
    .context_window=200000,  .max_tokens=100000 },
  { .id="o4-mini",      .name="o4 Mini",          .api="openai-completions", .provider="openai",
    .base_url="https://api.openai.com/v1", .reasoning=true,
    .cost={.input_per_mtok=1.10, .output_per_mtok=4.40, .cache_read_per_mtok=0.275, .cache_write_per_mtok=0},
    .context_window=200000,  .max_tokens=100000 },

  // OpenAI Codex (ChatGPT OAuth) — limits and pricing mirror the vendored
  // provider catalog. Authentication is deliberately separate from openai.
  { .id="gpt-5.3-codex-spark", .name="GPT-5.3 Codex Spark", .api="openai-codex-responses", .provider="openai-codex",
    .base_url="https://chatgpt.com/backend-api", .reasoning=true,
    .input_capabilities={"text"},
    .cost={.input_per_mtok=1.75, .output_per_mtok=14.00, .cache_read_per_mtok=0.175, .cache_write_per_mtok=0},
    .context_window=128000, .max_tokens=128000 },
  { .id="gpt-5.4", .name="GPT-5.4", .api="openai-codex-responses", .provider="openai-codex",
    .base_url="https://chatgpt.com/backend-api", .reasoning=true,
    .input_capabilities={"text", "image"},
    .cost={.input_per_mtok=2.50, .output_per_mtok=15.00, .cache_read_per_mtok=0.25, .cache_write_per_mtok=0},
    .context_window=272000, .max_tokens=128000 },
  { .id="gpt-5.4-mini", .name="GPT-5.4 mini", .api="openai-codex-responses", .provider="openai-codex",
    .base_url="https://chatgpt.com/backend-api", .reasoning=true,
    .input_capabilities={"text", "image"},
    .cost={.input_per_mtok=0.75, .output_per_mtok=4.50, .cache_read_per_mtok=0.075, .cache_write_per_mtok=0},
    .context_window=272000, .max_tokens=128000 },
  { .id="gpt-5.5", .name="GPT-5.5", .api="openai-codex-responses", .provider="openai-codex",
    .base_url="https://chatgpt.com/backend-api", .reasoning=true,
    .input_capabilities={"text", "image"},
    .cost={.input_per_mtok=5.00, .output_per_mtok=30.00, .cache_read_per_mtok=0.50, .cache_write_per_mtok=0},
    .context_window=272000, .max_tokens=128000 },
  { .id="gpt-5.6-luna", .name="GPT-5.6 Luna", .api="openai-codex-responses", .provider="openai-codex",
    .base_url="https://chatgpt.com/backend-api", .reasoning=true,
    .input_capabilities={"text", "image"},
    .cost={.input_per_mtok=0.20, .output_per_mtok=1.20, .cache_read_per_mtok=0.02, .cache_write_per_mtok=0.25},
    .context_window=272000, .max_tokens=128000 },
  { .id="gpt-5.6-sol", .name="GPT-5.6 Sol", .api="openai-codex-responses", .provider="openai-codex",
    .base_url="https://chatgpt.com/backend-api", .reasoning=true,
    .input_capabilities={"text", "image"},
    .cost={.input_per_mtok=5.00, .output_per_mtok=30.00, .cache_read_per_mtok=0.50, .cache_write_per_mtok=6.25},
    .context_window=272000, .max_tokens=128000 },
  { .id="gpt-5.6-terra", .name="GPT-5.6 Terra", .api="openai-codex-responses", .provider="openai-codex",
    .base_url="https://chatgpt.com/backend-api", .reasoning=true,
    .input_capabilities={"text", "image"},
    .cost={.input_per_mtok=2.00, .output_per_mtok=12.00, .cache_read_per_mtok=0.20, .cache_write_per_mtok=2.50},
    .context_window=272000, .max_tokens=128000 },

  { .id="deepseek-chat",     .name="DeepSeek V3",     .api="openai-completions", .provider="deepseek",
    .base_url="https://api.deepseek.com/v1", .reasoning=false,
    .cost={.input_per_mtok=0.27, .output_per_mtok=1.10, .cache_read_per_mtok=0.07, .cache_write_per_mtok=0},
    .context_window=64000,  .max_tokens=8192 },
  { .id="deepseek-reasoner", .name="DeepSeek R1",     .api="openai-completions", .provider="deepseek",
    .base_url="https://api.deepseek.com/v1", .reasoning=true,
    .cost={.input_per_mtok=0.55, .output_per_mtok=2.19, .cache_read_per_mtok=0.14, .cache_write_per_mtok=0},
    .context_window=64000,  .max_tokens=8192 },

  { .id="llama-3.3-70b-versatile",  .name="Llama 3.3 70B",  .api="openai-completions", .provider="groq",
    .base_url="https://api.groq.com/openai/v1", .reasoning=false,
    .cost={.input_per_mtok=0.59, .output_per_mtok=0.79, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=128000, .max_tokens=32768 },
  { .id="llama-3.1-8b-instant",     .name="Llama 3.1 8B",   .api="openai-completions", .provider="groq",
    .base_url="https://api.groq.com/openai/v1", .reasoning=false,
    .cost={.input_per_mtok=0.05, .output_per_mtok=0.08, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=128000, .max_tokens=8000 },
  { .id="llama3-70b-8192",          .name="Llama 3 70B",    .api="openai-completions", .provider="groq",
    .base_url="https://api.groq.com/openai/v1", .reasoning=false,
    .cost={.input_per_mtok=0.59, .output_per_mtok=0.79, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=8192,   .max_tokens=8192 },
  { .id="moonshotai/kimi-k2-instruct", .name="Kimi K2",     .api="openai-completions", .provider="groq",
    .base_url="https://api.groq.com/openai/v1", .reasoning=false,
    .cost={.input_per_mtok=1.00, .output_per_mtok=3.00, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=131072, .max_tokens=16000 },
  { .id="qwen-qwq-32b",             .name="QwQ 32B",        .api="openai-completions", .provider="groq",
    .base_url="https://api.groq.com/openai/v1", .reasoning=true,
    .cost={.input_per_mtok=0.29, .output_per_mtok=0.39, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=128000, .max_tokens=16000 },

  { .id="grok-3",      .name="Grok 3",       .api="openai-completions", .provider="xai",
    .base_url="https://api.x.ai/v1", .reasoning=false,
    .cost={.input_per_mtok=3.00, .output_per_mtok=15.00, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=131072, .max_tokens=16384 },
  { .id="grok-3-mini", .name="Grok 3 Mini",  .api="openai-completions", .provider="xai",
    .base_url="https://api.x.ai/v1", .reasoning=true,
    .cost={.input_per_mtok=0.30, .output_per_mtok=0.50, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=131072, .max_tokens=16384 },
  { .id="grok-2-1212", .name="Grok 2",       .api="openai-completions", .provider="xai",
    .base_url="https://api.x.ai/v1", .reasoning=false,
    .cost={.input_per_mtok=2.00, .output_per_mtok=10.00, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=131072, .max_tokens=8192 },

  { .id="llama3.1-8b",    .name="Llama 3.1 8B",    .api="openai-completions", .provider="cerebras",
    .base_url="https://api.cerebras.ai/v1", .reasoning=false,
    .cost={.input_per_mtok=0.10, .output_per_mtok=0.10, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=32000,  .max_tokens=8000 },
  { .id="llama-3.3-70b",  .name="Llama 3.3 70B",   .api="openai-completions", .provider="cerebras",
    .base_url="https://api.cerebras.ai/v1", .reasoning=false,
    .cost={.input_per_mtok=0.85, .output_per_mtok=1.20, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=131072, .max_tokens=8192 },
  { .id="qwen-3-235b-a22b-instruct-2507", .name="Qwen 3 235B", .api="openai-completions", .provider="cerebras",
    .base_url="https://api.cerebras.ai/v1", .reasoning=false,
    .cost={.input_per_mtok=0.60, .output_per_mtok=0.60, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=131000, .max_tokens=32000 },

  // OpenRouter — https://openrouter.ai/models
  { .id="anthropic/claude-sonnet-4-5",  .name="Claude Sonnet 4.5 (OR)",  .api="openai-completions", .provider="openrouter",
    .base_url="https://openrouter.ai/api/v1", .reasoning=false,
    .cost={.input_per_mtok=3.00, .output_per_mtok=15.00, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=200000, .max_tokens=64000 },
  { .id="anthropic/claude-opus-4",      .name="Claude Opus 4 (OR)",      .api="openai-completions", .provider="openrouter",
    .base_url="https://openrouter.ai/api/v1", .reasoning=false,
    .cost={.input_per_mtok=15.00, .output_per_mtok=75.00, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=200000, .max_tokens=32000 },
  { .id="google/gemini-2.5-pro",        .name="Gemini 2.5 Pro (OR)",     .api="openai-completions", .provider="openrouter",
    .base_url="https://openrouter.ai/api/v1", .reasoning=true,
    .cost={.input_per_mtok=1.25, .output_per_mtok=10.00, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=1048576,.max_tokens=65536 },
  { .id="google/gemini-2.0-flash",      .name="Gemini 2.0 Flash (OR)",   .api="openai-completions", .provider="openrouter",
    .base_url="https://openrouter.ai/api/v1", .reasoning=false,
    .cost={.input_per_mtok=0.10, .output_per_mtok=0.40, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=1048576,.max_tokens=8192  },
  { .id="meta-llama/llama-4-maverick",  .name="Llama 4 Maverick (OR)",   .api="openai-completions", .provider="openrouter",
    .base_url="https://openrouter.ai/api/v1", .reasoning=false,
    .cost={.input_per_mtok=0.18, .output_per_mtok=0.72, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=524288, .max_tokens=8192  },
  { .id="qwen/qwen3-235b-a22b",         .name="Qwen 3 235B (OR)",        .api="openai-completions", .provider="openrouter",
    .base_url="https://openrouter.ai/api/v1", .reasoning=true,
    .cost={.input_per_mtok=0.14, .output_per_mtok=0.60, .cache_read_per_mtok=0, .cache_write_per_mtok=0},
    .context_window=40960,  .max_tokens=16000 },

  { .id="accounts/fireworks/models/glm-5p2", .name="GLM 5.2", .api="openai-completions", .provider="fireworks",
    .base_url="https://api.fireworks.ai/inference/v1", .reasoning=false,
    .context_window=128000, .max_tokens=4096 },

  { .id="gemini-2.5-pro",         .name="Gemini 2.5 Pro",     .api="openai-completions", .provider="google",
    .base_url="https://generativelanguage.googleapis.com/v1beta/openai", .reasoning=true,
    .cost={.input_per_mtok=1.25, .output_per_mtok=10.00, .cache_read_per_mtok=0.31, .cache_write_per_mtok=0},
    .context_window=1048576, .max_tokens=65536 },
  { .id="gemini-2.5-flash",       .name="Gemini 2.5 Flash",   .api="openai-completions", .provider="google",
    .base_url="https://generativelanguage.googleapis.com/v1beta/openai", .reasoning=true,
    .cost={.input_per_mtok=0.30, .output_per_mtok=2.50, .cache_read_per_mtok=0.075, .cache_write_per_mtok=0},
    .context_window=1048576, .max_tokens=65536 },
  { .id="gemini-2.0-flash",       .name="Gemini 2.0 Flash",   .api="openai-completions", .provider="google",
    .base_url="https://generativelanguage.googleapis.com/v1beta/openai", .reasoning=false,
    .cost={.input_per_mtok=0.10, .output_per_mtok=0.40, .cache_read_per_mtok=0.025, .cache_write_per_mtok=0},
    .context_window=1048576, .max_tokens=8192  },
  { .id="gemini-1.5-pro",         .name="Gemini 1.5 Pro",     .api="openai-completions", .provider="google",
    .base_url="https://generativelanguage.googleapis.com/v1beta/openai", .reasoning=false,
    .cost={.input_per_mtok=1.25, .output_per_mtok=5.00, .cache_read_per_mtok=0.31, .cache_write_per_mtok=0},
    .context_window=2097152, .max_tokens=8192  },
  { .id="gemini-1.5-flash",       .name="Gemini 1.5 Flash",   .api="openai-completions", .provider="google",
    .base_url="https://generativelanguage.googleapis.com/v1beta/openai", .reasoning=false,
    .cost={.input_per_mtok=0.075, .output_per_mtok=0.30, .cache_read_per_mtok=0.01875, .cache_write_per_mtok=0},
    .context_window=1048576, .max_tokens=8192  },

  { .id="muse-spark-1.1", .name="Muse Spark 1.1", .api="muse-messages", .provider="meta",
    .base_url="https://api.meta.ai", .reasoning=true,
    .input_capabilities={"text", "image"},
    .cost={.input_per_mtok=1.25, .output_per_mtok=6.00, .cache_read_per_mtok=0.15, .cache_write_per_mtok=0},
    .context_window=128000, .max_tokens=16384,
    .thinking_level_map={
      {"off", std::nullopt}, {"minimal", "low"}, {"low", "low"},
      {"medium", "medium"}, {"high", "high"}, {"xhigh", "xhigh"},
    } },
  { .id="muse-spark-1.1", .name="Muse Spark 1.1 (Chat Completions)",
    .api="openai-completions", .provider="meta-chat",
    .base_url="https://api.meta.ai/v1", .reasoning=true,
    .input_capabilities={"text", "image"},
    .cost={.input_per_mtok=1.25, .output_per_mtok=6.00, .cache_read_per_mtok=0.15, .cache_write_per_mtok=0},
    .context_window=128000, .max_tokens=16384 },
};
// clang-format on

namespace {

int thinking_rank(ThinkingLevel level) {
  switch (level) {
  case ThinkingLevel::off:
    return 0;
  case ThinkingLevel::minimal:
    return 1;
  case ThinkingLevel::low:
    return 2;
  case ThinkingLevel::medium:
    return 3;
  case ThinkingLevel::high:
    return 4;
  case ThinkingLevel::xhigh:
    return 5;
  }
  return 0;
}

ThinkingLevel thinking_from_rank(int rank) {
  switch (rank) {
  case 1:
    return ThinkingLevel::minimal;
  case 2:
    return ThinkingLevel::low;
  case 3:
    return ThinkingLevel::medium;
  case 4:
    return ThinkingLevel::high;
  case 5:
    return ThinkingLevel::xhigh;
  default:
    return ThinkingLevel::off;
  }
}

} // namespace

ThinkingLevelResolution resolve_thinking_level(const Model &model,
                                               ThinkingLevel requested) {
  auto supports = [&](ThinkingLevel level) {
    if (!model.reasoning)
      return level == ThinkingLevel::off;
    if (!model.thinking_level_map.empty()) {
      const auto key = std::string(thinking_level_to_string(level));
      auto it = model.thinking_level_map.find(key);
      return it != model.thinking_level_map.end() && it->second.has_value();
    }
    return level != ThinkingLevel::xhigh;
  };

  ThinkingLevel resolved = requested;
  if (!supports(resolved)) {
    for (int rank = thinking_rank(requested); rank >= 0; --rank) {
      const auto candidate = thinking_from_rank(rank);
      if (supports(candidate)) {
        resolved = candidate;
        break;
      }
    }
  }
  if (!supports(resolved))
    resolved = ThinkingLevel::off;

  ThinkingLevelResolution result{.level = resolved};
  if (resolved != requested) {
    result.warning =
        "thinking level '" + std::string(thinking_level_to_string(requested)) +
        "' is unsupported by " + model.provider + "/" + model.id + "; using '" +
        std::string(thinking_level_to_string(resolved)) + "'";
  }
  return result;
}

namespace {

std::string lower_ascii(std::string_view value) {
  std::string result(value);
  for (char &c : result)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return result;
}

bool same_header_name(std::string_view left, std::string_view right) {
  return lower_ascii(left) == lower_ascii(right);
}

void merge_headers(std::map<std::string, std::string> &target,
                   const std::map<std::string, std::string> &source) {
  for (const auto &[key, value] : source) {
    for (auto it = target.begin(); it != target.end(); ++it) {
      if (same_header_name(it->first, key)) {
        target.erase(it);
        break;
      }
    }
    target[key] = value;
  }
}

void apply_cost(Model::Cost &target, const ConfiguredCost &source) {
  if (source.input_per_mtok)
    target.input_per_mtok = *source.input_per_mtok;
  if (source.output_per_mtok)
    target.output_per_mtok = *source.output_per_mtok;
  if (source.cache_read_per_mtok)
    target.cache_read_per_mtok = *source.cache_read_per_mtok;
  if (source.cache_write_per_mtok)
    target.cache_write_per_mtok = *source.cache_write_per_mtok;
}

void apply_configured_model(Model &target, const ConfiguredModel &source,
                            const Provider &provider) {
  if (source.name)
    target.name = *source.name;
  if (source.api)
    target.api = *source.api;
  if (source.base_url)
    target.base_url = *source.base_url;
  if (source.reasoning)
    target.reasoning = *source.reasoning;
  if (source.input_capabilities)
    target.input_capabilities = *source.input_capabilities;
  if (source.context_window)
    target.context_window = *source.context_window;
  if (source.max_tokens)
    target.max_tokens = *source.max_tokens;
  merge_headers(target.headers, source.headers);
  apply_cost(target.cost, source.cost);
  if (source.thinking_level_map)
    target.thinking_level_map = *source.thinking_level_map;
  if (target.provider.empty())
    target.provider = provider.id;
}

Model make_custom_model(const ConfiguredModel &source,
                        const Provider &provider) {
  Model model;
  model.id = source.id;
  model.name = source.name.value_or(source.id);
  model.api = source.api.value_or(provider.api);
  model.provider = provider.id;
  model.base_url = source.base_url.value_or(provider.base_url);
  model.reasoning = source.reasoning.value_or(false);
  model.input_capabilities =
      source.input_capabilities.value_or(std::vector<std::string>{"text"});
  model.context_window = source.context_window.value_or(128000);
  model.max_tokens = source.max_tokens.value_or(4096);
  model.headers = provider.headers;
  merge_headers(model.headers, source.headers);
  apply_cost(model.cost, source.cost);
  if (source.thinking_level_map)
    model.thinking_level_map = *source.thinking_level_map;
  return model;
}

} // namespace

ModelCatalogEntry project_model(const Model &model) {
  return {.key = {.provider_id = model.provider, .model_id = model.id},
          .display_name = model.name,
          .api = model.api,
          .reasoning = model.reasoning,
          .input_capabilities = model.input_capabilities,
          .context_window = model.context_window,
          .max_tokens = model.max_tokens,
          .cost = {.input_per_mtok = model.cost.input_per_mtok,
                   .output_per_mtok = model.cost.output_per_mtok,
                   .cache_read_per_mtok = model.cost.cache_read_per_mtok,
                   .cache_write_per_mtok = model.cost.cache_write_per_mtok}};
}

void ModelDiscoveryAdapterCollection::register_adapter(std::string adapter_id,
                                                       Adapter adapter) {
  if (adapter_id.empty())
    throw std::invalid_argument("discovery adapter id must not be empty");
  if (!adapter)
    throw std::invalid_argument("discovery adapter must not be null");
  std::scoped_lock lock(mutex_);
  adapters_[std::move(adapter_id)] = std::move(adapter);
}

bool ModelDiscoveryAdapterCollection::has_adapter(
    std::string_view adapter_id) const {
  std::scoped_lock lock(mutex_);
  return adapters_.contains(std::string(adapter_id));
}

ModelDiscoveryAdapterCollection::Adapter
ModelDiscoveryAdapterCollection::get_adapter(
    std::string_view adapter_id) const {
  std::scoped_lock lock(mutex_);
  const auto it = adapters_.find(std::string(adapter_id));
  return it == adapters_.end() ? nullptr : it->second;
}

std::vector<Provider> ModelCatalog::builtin_providers() {
  std::vector<Provider> providers = {
      {"openai", "openai-completions", "https://api.openai.com/v1"},
      {"openai-codex", "openai-codex-responses",
       "https://chatgpt.com/backend-api", ProviderAuthPolicy::oauth},
      {"deepseek", "openai-completions", "https://api.deepseek.com/v1"},
      {"groq", "openai-completions", "https://api.groq.com/openai/v1"},
      {"xai", "openai-completions", "https://api.x.ai/v1"},
      {"cerebras", "openai-completions", "https://api.cerebras.ai/v1"},
      {"openrouter", "openai-completions", "https://openrouter.ai/api/v1"},
      {"fireworks", "openai-completions",
       "https://api.fireworks.ai/inference/v1"},
      {"google", "openai-completions",
       "https://generativelanguage.googleapis.com/v1beta/openai"},
      {"meta", "muse-messages", "https://api.meta.ai"},
      {"meta-chat", "openai-completions", "https://api.meta.ai/v1"},
  };
  for (auto &provider : providers) {
    provider.inference.adapter_id = provider.api;
    switch (provider.auth) {
    case ProviderAuthPolicy::none:
      provider.authentication = {.kind = AuthenticationBindingKind::none};
      break;
    case ProviderAuthPolicy::oauth:
      provider.authentication = {.kind = AuthenticationBindingKind::adapter,
                                 .adapter_id = provider.id + "-oauth"};
      break;
    case ProviderAuthPolicy::required:
    case ProviderAuthPolicy::optional:
      provider.authentication = {.kind = AuthenticationBindingKind::api_key};
      break;
    }
  }
  return providers;
}

ModelCatalog::ModelCatalog( // NOLINT(readability-function-cognitive-complexity)
    const std::map<std::string, ProviderConfig> &configured,
    std::shared_ptr<ModelDiscoveryAdapterCollection> discovery_adapters,
    std::shared_ptr<InferenceAdapterCollection> inference_adapters)
    : discovery_adapters_(std::move(discovery_adapters)),
      inference_adapters_(std::move(inference_adapters)) {
  if (!discovery_adapters_)
    discovery_adapters_ = std::make_shared<ModelDiscoveryAdapterCollection>();
  if (!inference_adapters_)
    inference_adapters_ = std::make_shared<InferenceAdapterCollection>();
  for (auto definition : builtin_providers())
    providers_.emplace(lower_ascii(definition.id), std::move(definition));

  for (const auto &model : all_models())
    add_or_replace(model);

  for (const auto &[key, config] : configured) {
    const auto canonical_key = lower_ascii(key);
    auto provider_it = providers_.find(canonical_key);
    if (provider_it == providers_.end()) {
      if (!config.api || !config.base_url) {
        throw std::runtime_error("providers." + key +
                                 " requires api and base_url");
      }
      Provider definition;
      definition.id = lower_ascii(config.id.empty() ? key : config.id);
      definition.api = *config.api;
      definition.base_url = *config.base_url;
      definition.auth = config.auth.value_or(ProviderAuthPolicy::required);
      definition.api_key = config.api_key;
      definition.headers = config.headers;
      provider_it =
          providers_.emplace(canonical_key, std::move(definition)).first;
    } else {
      auto &definition = provider_it->second;
      if (config.api)
        definition.api = *config.api;
      if (config.base_url)
        definition.base_url = *config.base_url;
      if (config.auth)
        definition.auth = *config.auth;
      if (config.api_key.literal || config.api_key.env_var)
        definition.api_key = config.api_key;
      merge_headers(definition.headers, config.headers);
    }

    if (provider_it->second.auth == ProviderAuthPolicy::oauth &&
        provider_it->second.id != "openai-codex") {
      throw std::runtime_error("providers." + key +
                               ": auth = oauth is not supported");
    }

    auto &definition = provider_it->second;
    definition.inference.adapter_id =
        config.inference_adapter.value_or(definition.api);
    if (config.discovery_adapter)
      definition.discovery =
          DiscoveryBinding{.adapter_id = *config.discovery_adapter,
                           .options = config.discovery_options};
    if (config.authentication_adapter) {
      definition.authentication = {.kind = AuthenticationBindingKind::adapter,
                                   .adapter_id =
                                       *config.authentication_adapter};
    } else {
      switch (definition.auth) {
      case ProviderAuthPolicy::none:
        definition.authentication = {.kind = AuthenticationBindingKind::none};
        break;
      case ProviderAuthPolicy::oauth:
        definition.authentication = {.kind = AuthenticationBindingKind::adapter,
                                     .adapter_id = definition.id + "-oauth"};
        break;
      case ProviderAuthPolicy::required:
      case ProviderAuthPolicy::optional:
        definition.authentication = {.kind =
                                         AuthenticationBindingKind::api_key};
        break;
      }
    }
    for (auto &model : models_) {
      if (lower_ascii(model.provider) != canonical_key)
        continue;
      model.provider = definition.id;
      if (config.api)
        model.api = *config.api;
      if (config.base_url)
        model.base_url = *config.base_url;
      merge_headers(model.headers, definition.headers);
    }
  }

  for (const auto &[key, config] : configured) {
    const auto *definition = provider(key);
    if (definition == nullptr)
      throw std::runtime_error("unknown configured provider: " + key);

    for (const auto &[model_id, override] : config.model_overrides) {
      // exact() returns const Model* for external read-only callers, but
      // this constructor legitimately owns and mutates models_ itself.
      auto *model =
          const_cast<Model *>( // NOLINT(cppcoreguidelines-pro-type-const-cast)
              exact(definition->id, model_id));
      if (model == nullptr) {
        std::string message = "providers.";
        message += key;
        message += ".model_overrides.";
        message += model_id;
        message += ": unknown built-in model";
        throw std::runtime_error(message);
      }
      configured_keys_[{lower_ascii(definition->id), model_id}] = true;
      apply_configured_model(*model, override, *definition);
    }

    for (const auto &custom : config.models) {
      if (custom.id.empty())
        throw std::runtime_error("providers." + key +
                                 ".models: model id must not be empty");
      configured_keys_[{lower_ascii(definition->id), custom.id}] = true;
      add_or_replace(make_custom_model(custom, *definition));
    }
  }
  base_models_ = models_;
  for (const auto &[id, definition] : providers_) {
    (void)id;
    refresh_status_[definition.id] = {.provider_id = definition.id};
    if (definition.discovery &&
        !discovery_adapters_->has_adapter(definition.discovery->adapter_id)) {
      throw std::runtime_error("provider " + definition.id +
                               " uses unknown discovery adapter '" +
                               definition.discovery->adapter_id + "'");
    }
  }
  view_.generation = 1;
  view_.entries.reserve(models_.size());
  for (const auto &model : models_)
    view_.entries.push_back(project_model(model));
  view_.refresh_status.reserve(providers_.size());
  for (const auto &[id, definition] : providers_)
    view_.refresh_status.push_back({.provider_id = definition.id});
}

void ModelCatalog::add_or_replace(Model model) {
  const auto key = std::make_pair(lower_ascii(model.provider), model.id);
  auto it = indexes_.find(key);
  if (it == indexes_.end()) {
    indexes_[key] = models_.size();
    models_.push_back(std::move(model));
  } else {
    models_[it->second] = std::move(model);
  }
}

const Provider *ModelCatalog::provider(std::string_view id) const {
  auto it = providers_.find(lower_ascii(id));
  return it == providers_.end() ? nullptr : &it->second;
}

const Model *ModelCatalog::exact(std::string_view provider_id,
                                 std::string_view model_id) const {
  auto it = indexes_.find(
      std::make_pair(lower_ascii(provider_id), std::string(model_id)));
  if (it == indexes_.end())
    return nullptr;
  return &models_[it->second];
}

std::optional<ModelCatalogEntry>
ModelCatalog::entry(const ModelKey &key) const {
  const auto *model = exact(key.provider_id, key.model_id);
  return model == nullptr
             ? std::nullopt
             : std::optional<ModelCatalogEntry>(project_model(*model));
}

ModelResolution ModelCatalog::resolve(const ModelKey &key,
                                      std::optional<std::string> base_url,
                                      std::string source) const {
  return resolve({.provider = key.provider_id,
                  .model = key.model_id,
                  .base_url = std::move(base_url),
                  .source = std::move(source)});
}

ModelResolution ModelCatalog::resolve(const ModelSelection &selection) const {
  const auto source = selection.source.empty() ? "selection" : selection.source;
  if (selection.model.empty())
    return {.error = std::string(source) + ": model id must not be empty"};

  if (selection.provider && !selection.provider->empty()) {

    const auto *definition = provider(*selection.provider);
    std::string model_id = selection.model;
    const auto slash = model_id.find('/');
    if (definition != nullptr && slash != std::string::npos &&
        lower_ascii(model_id.substr(0, slash)) == lower_ascii(definition->id)) {
      model_id = model_id.substr(slash + 1);
    }

    if (definition == nullptr) {
      if (!selection.base_url || selection.base_url->empty()) {
        return {.error = std::string(source) + ": unknown provider '" +
                         *selection.provider + "'"};
      }
      Model model;
      model.id = model_id;
      model.name = model_id;
      model.api = "openai-completions";
      model.provider = lower_ascii(*selection.provider);
      model.base_url = *selection.base_url;
      model.context_window = 128000;
      model.max_tokens = 4096;
      return {.model = std::move(model)};
    }

    Model model;
    if (const auto *known = exact(definition->id, model_id)) {
      model = *known;
    } else {
      model.id = model_id;
      model.name = model_id;
      model.api = definition->api;
      model.provider = definition->id;
      model.base_url = definition->base_url;
      model.input_capabilities = {"text"};
      model.context_window = 128000;
      model.max_tokens = 4096;
      model.headers = definition->headers;
    }
    if (selection.base_url)
      model.base_url = *selection.base_url;
    return {.model = std::move(model)};
  }

  const auto raw = selection.model;
  if (const auto slash = raw.find('/'); slash != std::string::npos) {
    const auto prefix = raw.substr(0, slash);
    if (const auto *definition = provider(prefix)) {
      if (const auto *known = exact(definition->id, raw.substr(slash + 1))) {
        Model model = *known;
        if (selection.base_url)
          model.base_url = *selection.base_url;
        return {.model = std::move(model)};
      }
    }
  }

  std::vector<const Model *> matches;
  for (const auto &model : models_) {
    if (model.id == raw)
      matches.push_back(&model);
  }
  if (matches.size() == 1) {
    Model model = *matches.front();
    if (selection.base_url)
      model.base_url = *selection.base_url;
    return {.model = std::move(model)};
  }
  if (matches.size() > 1) {
    std::ranges::sort(matches, [](const Model *left, const Model *right) {
      return left->provider < right->provider;
    });
    std::string error = std::string(source) + ": ambiguous model '" + raw +
                        "'; choose one of: ";
    for (std::size_t i = 0; i < matches.size(); ++i) {
      if (i != 0)
        error += ", ";
      error += matches[i]->provider + "/" + matches[i]->id;
    }
    return {.error = std::move(error)};
  }

  if (selection.base_url) {
    Model model;
    model.id = raw;
    model.name = raw;
    model.api = "openai-completions";
    model.provider = "custom";
    model.base_url = *selection.base_url;
    model.input_capabilities = {"text"};
    model.context_window = 128000;
    model.max_tokens = 4096;
    return {.model = std::move(model)};
  }
  return {.error = std::string(source) + ": unknown model '" + raw + "'"};
}

std::vector<ModelCatalogEntry>
ModelCatalog::search(std::string_view filter) const {
  std::vector<ModelCatalogEntry> result;
  const auto needle = lower_ascii(filter);
  for (const auto &entry : view_.entries) {
    if (needle.empty() ||
        lower_ascii(entry.key.model_id + " " + entry.key.provider_id + " " +
                    entry.display_name)
            .contains(needle))
      result.push_back(entry);
  }
  return result;
}

std::vector<const Model *>
ModelCatalog::search_models(std::string_view filter) const {
  std::vector<const Model *> result;
  for (const auto &entry : search(filter))
    if (const auto *model = exact(entry.key.provider_id, entry.key.model_id))
      result.push_back(model);
  return result;
}

void ModelCatalog::validate_registered_apis() const {
  for (const auto &[id, provider] : providers_) {
    (void)id;
    if (!inference_adapters_->has_adapter(provider.inference.adapter_id))
      throw std::runtime_error("provider " + provider.id +
                               " uses unknown inference adapter '" +
                               provider.inference.adapter_id + "'");
  }
  for (const auto &model : models_) {
    if (!inference_adapters_->has_adapter(model.api)) {
      throw std::runtime_error("model " + model.provider + "/" + model.id +
                               " uses unregistered API '" + model.api + "'");
    }
  }
}

const std::vector<Model> &all_models() { return kModels; }

std::optional<Model> find_model(std::string_view spec,
                                std::string_view provider_hint) {
  auto try_exact = [](std::string_view id,
                      std::string_view prov) -> std::optional<Model> {
    for (const auto &m : kModels) {
      if (m.id == id && (prov.empty() || m.provider == prov))
        return m;
    }
    return std::nullopt;
  };

  // Full-id exact match
  if (auto m = try_exact(spec, provider_hint))
    return m;
  if (auto m = try_exact(spec, {}))
    return m;

  // If provider_hint present, try spec as OR model id directly
  if (!provider_hint.empty()) {
    // e.g. hint=openrouter, spec=deepseek/deepseek-v4-flash
    if (auto m = try_exact(spec, provider_hint))
      return m;
    if (spec.size() > provider_hint.size() &&
        spec.substr(0, provider_hint.size()) == provider_hint &&
        spec[provider_hint.size()] == '/') {
      std::string stripped(spec.substr(provider_hint.size() + 1));
      if (auto mm = try_exact(stripped, provider_hint))
        return mm;
    }
  }

  // Standard split on first slash
  std::string parsed_provider;
  std::string parsed_id;
  if (auto slash = spec.find('/'); slash != std::string_view::npos) {
    parsed_provider = std::string(spec.substr(0, slash));
    parsed_id = std::string(spec.substr(slash + 1));
  } else {
    parsed_provider = std::string(provider_hint);
    parsed_id = std::string(spec);
  }

  if (auto m = try_exact(parsed_id, parsed_provider))
    return m;
  if (auto m = try_exact(parsed_id, {}))
    return m;

  // --- generic fallback ---
  std::string effective_provider;
  std::string effective_id;

  if (!provider_hint.empty()) {
    effective_provider = std::string(provider_hint);
    if (spec.size() > provider_hint.size() &&
        spec.substr(0, provider_hint.size()) == provider_hint &&
        spec[provider_hint.size()] == '/') {
      effective_id = std::string(spec.substr(provider_hint.size() + 1));
    } else {
      // Keep full spec so deepseek/deepseek-v4-flash stays intact
      effective_id = std::string(spec);
    }
  } else {
    if (parsed_provider == "openrouter") {
      effective_provider = "openrouter";
      effective_id = parsed_id;
    } else {
      // spec = deepseek/deepseek-v4-flash with no hint -> treat deepseek as
      // part of id if ambiguous Heuristic: if parsed_provider not known as
      // provider, treat full spec as id
      bool known = false;
      for (const auto &km : kModels)
        if (km.provider == parsed_provider) {
          known = true;
          break;
        }
      if (!known && parsed_provider != "custom" && !parsed_provider.empty() &&
          !parsed_id.empty() && parsed_id.contains('/')) {
        effective_provider = "custom";
        effective_id = std::string(spec);
      } else if (parsed_provider == "openrouter" || parsed_provider.empty() ||
                 parsed_provider == "custom") {
        effective_provider =
            parsed_provider.empty() ? "custom" : parsed_provider;
        effective_id = parsed_id;
      } else {
        // default to hint-less split result
        effective_provider = parsed_provider;
        effective_id = parsed_id;
      }
      if (effective_provider.empty())
        effective_provider = "custom";
    }
  }

  if (effective_id.empty())
    return std::nullopt;
  // strip leading openrouter/ from id if provider is openrouter
  if (effective_provider == "openrouter" &&
      effective_id.starts_with("openrouter/")) {
    effective_id = effective_id.substr(std::string("openrouter/").size());
  }

  Model generic;
  generic.id = effective_id;
  generic.name = effective_id;
  generic.api = "openai-completions";
  generic.provider = effective_provider;
  generic.context_window = 128000;
  generic.max_tokens = 4096;

  auto try_infer = [&](std::string_view want) -> bool {
    if (want.empty() || want == "custom")
      return false;
    if (want == "openai-codex") {
      generic.base_url = "https://chatgpt.com/backend-api";
      generic.api = "openai-codex-responses";
      return true;
    }
    for (const auto &m : kModels) {
      if (m.provider == want) {
        generic.base_url = m.base_url;
        generic.api = m.api;
        return true;
      }
    }
    return false;
  };

  if (!provider_hint.empty())
    try_infer(provider_hint);
  if (generic.base_url.empty())
    try_infer(parsed_provider);
  if (generic.base_url.empty())
    try_infer(effective_provider);

  return generic;
}

std::vector<const Model *> search_models(std::string_view filter) {
  std::vector<const Model *> result;
  auto to_lower = [](std::string s) {
    for (char &c : s)
      c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
  };
  std::string needle = to_lower(std::string(filter));
  for (const auto &m : kModels) {
    if (needle.empty()) {
      result.push_back(&m);
      continue;
    }
    std::string hay = to_lower(m.id + " " + m.provider + " " + m.name);
    if (hay.contains(needle))
      result.push_back(&m);
  }
  return result;
}

ModelCatalogView ModelCatalog::view() const {
  std::shared_lock lock(mutex_);
  return view_;
}

void ModelCatalog::rebuild_from_reports() {
  models_.clear();
  indexes_.clear();
  for (const auto &model : base_models_)
    add_or_replace(model);

  for (const auto &[provider_id, report] : discovered_reports_) {
    const auto *definition = providers_.contains(provider_id)
                                 ? &providers_.at(provider_id)
                                 : nullptr;
    if (definition == nullptr)
      continue;
    for (const auto &entry : report.models) {
      const auto key = std::make_pair(lower_ascii(entry.key.provider_id),
                                      entry.key.model_id);
      if (configured_keys_.contains(key))
        continue;
      Model model;
      model.id = entry.key.model_id;
      model.name = entry.display_name.empty() ? model.id : entry.display_name;
      model.api = entry.api.empty() ? definition->api : entry.api;
      model.provider = definition->id;
      model.base_url = definition->base_url;
      model.reasoning = entry.reasoning;
      model.input_capabilities = entry.input_capabilities;
      model.context_window = entry.context_window;
      model.max_tokens = entry.max_tokens;
      model.headers = definition->headers;
      add_or_replace(std::move(model));
    }
  }

  view_.entries.clear();
  view_.generation = 1;
  view_.entries.reserve(models_.size());
  for (const auto &model : models_)
    view_.entries.push_back(project_model(model));
  view_.refresh_status.clear();
  view_.refresh_status.reserve(refresh_status_.size());
  for (const auto &[id, status] : refresh_status_) {
    (void)id;
    view_.refresh_status.push_back(status);
  }
}

std::vector<ProviderRefreshStatus>
ModelCatalog::refresh( // NOLINT(readability-function-cognitive-complexity)
    const std::vector<std::string> &provider_ids,
    std::stop_token stop_token) { // NOLINT(performance-unnecessary-value-param)
  std::scoped_lock refresh_lock(refresh_mutex_);
  std::map<std::string, Provider> providers;
  std::uint64_t generation = 0;
  {
    std::shared_lock lock(mutex_);
    providers = providers_;
    generation = view_.generation;
  }

  std::vector<std::string> selected;
  if (provider_ids.empty()) {
    for (const auto &[id, provider] : providers) {
      if (provider.discovery)
        selected.push_back(id);
    }
  } else {
    for (const auto &id : provider_ids) {
      const auto canonical = lower_ascii(id);
      if (!providers.contains(canonical))
        throw std::runtime_error("unknown provider: " + id);
      selected.push_back(canonical);
    }
  }

  std::map<std::string, ProviderModelReport> reports;
  std::map<std::string, ProviderRefreshStatus> statuses;
  std::map<std::string, ProviderRefreshStatus> prior_statuses;
  {
    std::shared_lock lock(mutex_);
    reports = discovered_reports_;
    statuses = refresh_status_;
    prior_statuses = refresh_status_;
  }

  {
    std::unique_lock lock(mutex_);
    for (const auto &id : selected) {
      const auto &provider = providers.at(id);
      refresh_status_[provider.id] = {.provider_id = provider.id,
                                      .state =
                                          ProviderRefreshState::refreshing};
    }
    view_.refresh_status.clear();
    for (const auto &[id, status] : refresh_status_) {
      (void)id;
      view_.refresh_status.push_back(status);
    }
  }

  auto cancel = [&]() {
    std::unique_lock lock(mutex_);
    refresh_status_ = prior_statuses;
    view_.refresh_status.clear();
    for (const auto &[id, status] : refresh_status_) {
      (void)id;
      view_.refresh_status.push_back(status);
    }
    return view_.refresh_status;
  };

  for (const auto &id : selected) {
    if (stop_token.stop_requested())
      return cancel();
    const auto &provider = providers.at(id);
    if (!provider.discovery) {
      statuses[provider.id] = {.provider_id = provider.id,
                               .state = ProviderRefreshState::idle};
      continue;
    }
    const auto adapter =
        discovery_adapters_->get_adapter(provider.discovery->adapter_id);
    if (!adapter) {
      statuses[provider.id] = {.provider_id = provider.id,
                               .state = ProviderRefreshState::failed,
                               .diagnostic = "unknown discovery adapter '" +
                                             provider.discovery->adapter_id +
                                             "'"};
      continue;
    }

    try {
      ProviderDiscoveryRequest request{.provider = provider,
                                       .options = provider.discovery->options};
      RequestAuthResolver auth_resolver;
      {
        std::shared_lock lock(mutex_);
        auth_resolver = request_auth_resolver_;
      }
      if (auth_resolver)
        request.auth = auth_resolver(provider.id, stop_token);
      auto report = adapter->discover(request, stop_token);
      if (stop_token.stop_requested())
        return cancel();
      if (report.provider_id != provider.id)
        throw std::runtime_error("report provider id '" + report.provider_id +
                                 "' does not match '" + provider.id + "'");
      if (report.models.size() > 10000)
        throw std::runtime_error("report exceeds the 10000 model limit");
      std::map<std::string, bool> seen;
      for (const auto &entry : report.models) {
        if (entry.key.provider_id != provider.id)
          throw std::runtime_error("report contains model for provider '" +
                                   entry.key.provider_id + "'");
        if (entry.key.model_id.empty() || entry.api.empty())
          throw std::runtime_error("report contains an incomplete model");
        if (!seen.emplace(entry.key.model_id, true).second)
          throw std::runtime_error("report contains duplicate model '" +
                                   entry.key.model_id + "'");
      }
      reports[provider.id] = std::move(report);
      statuses[provider.id] = {.provider_id = provider.id,
                               .state = ProviderRefreshState::succeeded};
    } catch (const std::exception &error) {
      statuses[provider.id] = {.provider_id = provider.id,
                               .state = ProviderRefreshState::failed,
                               .diagnostic = error.what()};
    }
  }

  if (stop_token.stop_requested())
    return cancel();

  {
    std::unique_lock lock(mutex_);
    if (view_.generation != generation)
      return view_.refresh_status;
    discovered_reports_ = std::move(reports);
    refresh_status_ = std::move(statuses);
    rebuild_from_reports();
    view_.generation = generation + 1;
  }
  return view().refresh_status;
}

void ModelCatalog::set_request_auth_resolver(
    RequestAuthResolver resolver) const {
  std::unique_lock lock(mutex_);
  request_auth_resolver_ = std::move(resolver);
}

} // namespace pi::core
