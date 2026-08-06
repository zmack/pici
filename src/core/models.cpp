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
      for (auto &km : kModels)
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
    for (auto &m : kModels) {
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
  for (auto &m : kModels) {
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

} // namespace pi::core
