#include "core/env_api_keys.h"

#include <cstdlib>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace pi::core {

std::optional<std::string> get_env_api_key(std::string_view provider) {
  auto try_vars = [](std::initializer_list<const char *> vars)
      -> std::optional<std::string> {
    for (const char *var : vars) {
      const char *val = std::getenv(var);
      if (val && val[0] != '\0')
        return std::string(val);
    }
    return std::nullopt;
  };

  if (provider == "openai")
    return try_vars({"OPENAI_API_KEY"});
  if (provider == "anthropic")
    return try_vars({"ANTHROPIC_OAUTH_TOKEN", "ANTHROPIC_API_KEY"});
  if (provider == "deepseek")
    return try_vars({"DEEPSEEK_API_KEY"});
  if (provider == "google")
    return try_vars({"GEMINI_API_KEY"});
  if (provider == "groq")
    return try_vars({"GROQ_API_KEY"});
  if (provider == "xai")
    return try_vars({"XAI_API_KEY"});
  if (provider == "openrouter")
    return try_vars({"OPENROUTER_API_KEY"});
  if (provider == "mistral")
    return try_vars({"MISTRAL_API_KEY"});
  if (provider == "cerebras")
    return try_vars({"CEREBRAS_API_KEY"});
  if (provider == "github-copilot")
    return try_vars({"COPILOT_GITHUB_TOKEN", "GH_TOKEN", "GITHUB_TOKEN"});

  return std::nullopt;
}

} // namespace pi::core
