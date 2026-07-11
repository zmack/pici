#include "core/models.h"
#include "core/message_types.h"

#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pi::core {

// clang-format off

// OpenAI — https://openai.com/api/pricing/
static const std::vector<Model> kModels = {
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

  // DeepSeek — https://api-docs.deepseek.com/quick_start/pricing
  { .id="deepseek-chat",     .name="DeepSeek V3",     .api="openai-completions", .provider="deepseek",
    .base_url="https://api.deepseek.com/v1", .reasoning=false,
    .cost={.input_per_mtok=0.27, .output_per_mtok=1.10, .cache_read_per_mtok=0.07, .cache_write_per_mtok=0},
    .context_window=64000,  .max_tokens=8192 },
  { .id="deepseek-reasoner", .name="DeepSeek R1",     .api="openai-completions", .provider="deepseek",
    .base_url="https://api.deepseek.com/v1", .reasoning=true,
    .cost={.input_per_mtok=0.55, .output_per_mtok=2.19, .cache_read_per_mtok=0.14, .cache_write_per_mtok=0},
    .context_window=64000,  .max_tokens=8192 },

  // Groq — https://groq.com/pricing/ (no cache token pricing)
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

  // xAI — https://x.ai/api (no published cache pricing)
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

  // Cerebras — https://cerebras.ai/pricing (no cache pricing)
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

  // OpenRouter — https://openrouter.ai/models (cache pricing omitted unless listed)
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

  // Fireworks — https://fireworks.ai/pricing (GLM-5.2 not publicly listed)
  { .id="accounts/fireworks/models/glm-5p2", .name="GLM 5.2", .api="openai-completions", .provider="fireworks",
    .base_url="https://api.fireworks.ai/inference/v1", .reasoning=false,
    .context_window=128000, .max_tokens=4096 },

  // Google (direct) — https://ai.google.dev/pricing (cache_write billed by storage-hour, not per call)
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

  // Meta Muse — docs/muse/messages_api.md (Messages API, Anthropic-compatible)
  // Context window and pricing are not published in the local API docs.
  { .id="muse-spark-1.1", .name="Muse Spark 1.1", .api="muse-messages", .provider="meta",
    .base_url="https://api.meta.ai", .reasoning=true,
    .input_capabilities={"text", "image"},
    .context_window=0, .max_tokens=8192,
    .thinking_level_map={
      {"off", std::nullopt}, {"minimal", "low"}, {"low", "low"},
      {"medium", "medium"}, {"high", "high"}, {"xhigh", "xhigh"},
    } },
};
// clang-format on

const std::vector<Model> &all_models() { return kModels; }

std::optional<Model> find_model(std::string_view spec,
                                std::string_view provider_hint) {
  // Full-ID match first (handles slash-heavy IDs like accounts/foo/models/bar)
  for (const auto &m : kModels) {
    bool id_match = m.id == spec;
    bool prov_match = provider_hint.empty() || m.provider == provider_hint;
    if (id_match && prov_match)
      return m;
  }

  // Try "provider/id" split
  std::string provider;
  std::string model_id;
  auto slash = spec.find('/');
  if (slash != std::string_view::npos) {
    provider = std::string(spec.substr(0, slash));
    model_id = std::string(spec.substr(slash + 1));
  } else {
    provider = std::string(provider_hint);
    model_id = std::string(spec);
  }

  for (const auto &m : kModels) {
    bool id_match = m.id == model_id;
    bool prov_match = provider.empty() || m.provider == provider;
    if (id_match && prov_match)
      return m;
  }

  // No registry hit — build a generic model so the user can still run with
  // --base-url + arbitrary model IDs (local servers, etc.)
  if (!model_id.empty()) {
    Model generic;
    generic.id = model_id;
    generic.name = model_id;
    generic.api = "openai-completions";
    generic.provider = provider.empty() ? "custom" : provider;
    generic.context_window = 128000;
    generic.max_tokens = 4096;
    return generic;
  }
  return std::nullopt;
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
    std::string haystack = to_lower(m.id + " " + m.provider + " " + m.name);
    if (haystack.contains(needle))
      result.push_back(&m);
  }
  return result;
}

} // namespace pi::core
