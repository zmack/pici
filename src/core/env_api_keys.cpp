#include "core/env_api_keys.h"

#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pi::core {

namespace {

// get_env_api_key() is called from each Agent's background worker thread
// once per LLM turn (see agent_loop.cpp's use of config.get_api_key), so
// with multiple concurrent Agent instances this can run on several threads
// at once. std::getenv() is not thread-safe against concurrent
// setenv/putenv, so every key is read exactly once into an immutable cache.
// C++ guarantees thread-safe initialization of function-local statics, so
// the lambda below runs exactly once regardless of how many threads call in.
const std::unordered_map<std::string, std::optional<std::string>> &
env_api_key_cache() {
  static const std::unordered_map<std::string, std::optional<std::string>>
      cache = [] {
        auto try_vars = [](std::initializer_list<const char *> vars)
            -> std::optional<std::string> {
          for (const char *var : vars) {
            const char *val = std::getenv(var); // NOLINT(concurrency-mt-unsafe)
            if (val && val[0] != '\0')
              return std::string(val);
          }
          return std::nullopt;
        };

        return std::unordered_map<std::string, std::optional<std::string>>{
            {"openai", try_vars({"OPENAI_API_KEY"})},
            {"anthropic",
             try_vars({"ANTHROPIC_OAUTH_TOKEN", "ANTHROPIC_API_KEY"})},
            {"deepseek", try_vars({"DEEPSEEK_API_KEY"})},
            {"google", try_vars({"GEMINI_API_KEY"})},
            {"groq", try_vars({"GROQ_API_KEY"})},
            {"xai", try_vars({"XAI_API_KEY"})},
            {"openrouter", try_vars({"OPENROUTER_API_KEY"})},
            {"mistral", try_vars({"MISTRAL_API_KEY"})},
            {"cerebras", try_vars({"CEREBRAS_API_KEY"})},
            {"fireworks", try_vars({"FIREWORKS_API_KEY"})},
            {"meta", try_vars({"MODEL_API_KEY", "META_API_KEY"})},
            {"github-copilot",
             try_vars({"COPILOT_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN"})},
        };
      }();
  return cache;
}

} // namespace

std::optional<std::string> get_env_api_key(std::string_view provider) {
  const auto &cache = env_api_key_cache();
  auto it = cache.find(std::string(provider));
  return it != cache.end() ? it->second : std::nullopt;
}

} // namespace pi::core
