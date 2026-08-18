#include "cli/args.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

namespace pi::cli {

namespace {

bool parse_thinking(std::string_view s, ThinkingLevel &out) {
  if (s == "off") {
    out = ThinkingLevel::off;
    return true;
  }
  if (s == "minimal") {
    out = ThinkingLevel::minimal;
    return true;
  }
  if (s == "low") {
    out = ThinkingLevel::low;
    return true;
  }
  if (s == "medium") {
    out = ThinkingLevel::medium;
    return true;
  }
  if (s == "high") {
    out = ThinkingLevel::high;
    return true;
  }
  if (s == "xhigh") {
    out = ThinkingLevel::xhigh;
    return true;
  }
  return false;
}

} // namespace

Args parse_args(int argc, char **argv) {
  Args result;
  const std::span args{argv, static_cast<std::size_t>(argc)};

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = args[static_cast<std::size_t>(i)];
    auto need = [&](std::string_view flag) -> std::string_view {
      if (i + 1 >= argc) {
        result.diagnostics.push_back(
            {.is_error = true,
             .message = std::string(flag) + " requires an argument"});
        return {};
      }
      return args[static_cast<std::size_t>(++i)];
    };

    if (arg == "auth" && result.messages.empty() &&
        result.auth_action == AuthAction::none) {
      if (++i >= argc) {
        result.auth_action = AuthAction::help;
        result.diagnostics.push_back(
            {.is_error = true,
             .message = "auth requires login, status, or logout"});
        break;
      }
      const std::string_view action = args[static_cast<std::size_t>(i)];
      if (action == "--help" || action == "-h") {
        result.auth_action = AuthAction::help;
        continue;
      }
      if (action == "login") {
        result.auth_action = AuthAction::login;
      } else if (action == "status") {
        result.auth_action = AuthAction::status;
      } else if (action == "logout") {
        result.auth_action = AuthAction::logout;
      } else {
        result.auth_action = AuthAction::help;
        result.diagnostics.push_back(
            {.is_error = true,
             .message = "unknown auth action \"" + std::string(action) + "\""});
        break;
      }

      for (++i; i < argc; ++i) {
        const std::string_view auth_arg = args[static_cast<std::size_t>(i)];
        if (auth_arg == "--help" || auth_arg == "-h") {
          result.auth_action = AuthAction::help;
        } else if (auth_arg == "--verbose") {
          result.verbose = true;
        } else if (auth_arg == "--config") {
          if (i + 1 >= argc) {
            result.diagnostics.push_back(
                {.is_error = true, .message = "--config requires an argument"});
          } else {
            result.config_path = args[static_cast<std::size_t>(++i)];
          }
        } else if (auth_arg == "--device") {
          result.auth_device = true;
        } else if (auth_arg == "--browser") {
          result.auth_browser = true;
        } else if (!auth_arg.empty() && auth_arg.front() == '-') {
          result.diagnostics.push_back(
              {.is_error = true,
               .message = "option " + std::string(auth_arg) +
                          " is not valid for auth commands"});
        } else if (result.auth_provider.empty()) {
          result.auth_provider = std::string(auth_arg);
        } else {
          result.diagnostics.push_back(
              {.is_error = true,
               .message = "unexpected auth argument \"" +
                          std::string(auth_arg) + "\""});
        }
      }
      if (result.auth_device && result.auth_browser) {
        result.diagnostics.push_back(
            {.is_error = true,
             .message =
                 "auth login accepts only one of --device or --browser"});
      }
      break;
    }

    if (result.auth_action != AuthAction::none) {
      result.diagnostics.push_back(
          {.is_error = true,
           .message = "auth must be the first non-option argument"});
      break;
    }

    if (arg == "--help" || arg == "-h") {
      result.help = true;
    } else if (arg == "--version" || arg == "-v") {
      result.version = true;
    } else if (arg == "--print" || arg == "-p") {
      result.print_mode = true;
    } else if (arg == "--mode") {
      auto v = need("--mode");
      if (v == "rpc") {
        result.rpc_mode = true;
      } else {
        result.diagnostics.push_back({.is_error = true,
                                      .message = "unsupported mode \"" +
                                                 std::string(v) +
                                                 "\"; supported modes: rpc"});
      }
    } else if (arg == "--verbose") {
      result.verbose = true;
    } else if (arg == "--model" || arg == "-m") {
      result.model = std::string(need("--model"));
      result.model_explicit = true;
    } else if (arg == "--provider") {
      result.provider = std::string(need("--provider"));
      result.provider_explicit = true;
    } else if (arg == "--base-url") {
      result.base_url = std::string(need("--base-url"));
      result.base_url_explicit = true;
    } else if (arg == "--api-key") {
      result.api_key = std::string(need("--api-key"));
      result.api_key_explicit = true;
    } else if (arg == "--system-prompt" || arg == "--system") {
      result.system_prompt = std::string(need("--system-prompt"));
    } else if (arg == "--append-system-prompt") {
      auto v = need("--append-system-prompt");
      if (!v.empty())
        result.append_system_prompts.emplace_back(v);
    } else if (arg == "--thinking") {
      auto v = need("--thinking");
      if (!v.empty() && !parse_thinking(v, result.thinking)) {
        result.diagnostics.push_back(
            {.is_error = false,
             .message = "invalid thinking level \"" + std::string(v) +
                        "\"; valid: off, minimal, low, medium, high, xhigh"});
      }
    } else if (arg == "--no-tools" || arg == "-nt") {
      result.no_tools = true;
    } else if (arg == "--no-builtin-tools" || arg == "-nbt") {
      result.no_builtin_tools = true;
    } else if (arg == "--tools" || arg == "-t") {
      auto v = need("--tools");
      // comma-separated list
      std::string s(v);
      std::string tok;
      for (char c : s) {
        if (c == ',') {
          if (!tok.empty()) {
            result.tools.push_back(tok);
            tok.clear();
          }
        } else {
          tok += c;
        }
      }
      if (!tok.empty())
        result.tools.push_back(tok);
    } else if (arg == "--tools-dir") {
      result.tools_dir = std::string(need("--tools-dir"));
    } else if (arg == "--hooks-file") {
      auto v = need("--hooks-file");
      if (!v.empty())
        result.hooks_files.emplace_back(v);
    } else if (arg == "--hooks-dir") {
      result.hooks_dir = std::string(need("--hooks-dir"));
    } else if (arg == "--render") {
      result.render = std::string(need("--render"));
    } else if (arg == "--test") {
      auto v = need("--test");
      if (!v.empty())
        result.test_files.emplace_back(v);
    } else if (arg == "--continue" || arg == "-c") {
      result.session_continue = true;
    } else if (arg == "--resume" || arg == "-r") {
      result.session_resume = std::string(need("--resume"));
      if (result.session_resume.empty()) {
        result.diagnostics.push_back(
            {.is_error = true,
             .message = "--resume requires a non-empty prefix"});
      }
    } else if (arg == "--session-dir") {
      result.session_dir = std::string(need("--session-dir"));
    } else if (arg == "--faux-control") {
      result.faux_control_socket = std::string(need("--faux-control"));
    } else if (arg == "--sandbox") {
      auto v = need("--sandbox");
      if (v == "auto" || v == "required" || v == "disabled" || v == "off") {
        result.sandbox_mode = v == "off" ? "disabled" : std::string(v);
        result.sandbox_mode_explicit = true;
      } else {
        result.diagnostics.push_back(
            {.is_error = true,
             .message = "invalid sandbox mode \"" + std::string(v) +
                        "\"; valid: auto, required, disabled"});
      }
    } else if (arg == "--no-sandbox") {
      result.sandbox_mode = "disabled";
      result.sandbox_mode_explicit = true;
    } else if (arg == "--mailbox") {
      result.mailbox_path = std::string(need("--mailbox"));
      result.mailbox_path_explicit = true;
      result.mailbox_enabled = true;
      result.mailbox_enabled_explicit = true;
    } else if (arg == "--no-mailbox") {
      result.mailbox_enabled = false;
      result.mailbox_enabled_explicit = true;
    } else if (arg == "--message" || arg == "-M") {
      auto v = need("--message");
      if (!v.empty())
        result.messages.emplace_back(v);
    } else if (arg == "--no-context-files" || arg == "-nc") {
      result.no_context_files = true;
    } else if (arg == "--remote-compaction") {
      result.remote_compaction_enabled = true;
    } else if (arg == "--compaction-threshold") {
      auto v = need("--compaction-threshold");
      if (!v.empty()) {
        try {
          std::size_t consumed = 0;
          const double parsed = std::stod(std::string(v), &consumed);
          if (consumed != v.size() || !(parsed > 0.0) || parsed > 1.0) {
            result.diagnostics.push_back(
                {.is_error = true,
                 .message = "--compaction-threshold expects a fraction in "
                            "(0, 1], got \"" +
                            std::string(v) + "\""});
          } else {
            result.compaction_threshold_pct = parsed;
          }
        } catch (const std::exception &) {
          result.diagnostics.push_back(
              {.is_error = true,
               .message = "--compaction-threshold expects a fraction in "
                          "(0, 1], got \"" +
                          std::string(v) + "\""});
        }
      }
    } else if (arg == "--config") {
      result.config_path = std::string(need("--config"));
    } else if (arg == "--list-tools") {
      result.list_tools = true;
    } else if (arg == "--list-addons") {
      result.list_addons = true;
    } else if (arg == "--otel-endpoint") {
      result.otel_endpoint = std::string(need("--otel-endpoint"));
    } else if (arg == "--stream-trace") {
      result.stream_trace = std::string(need("--stream-trace"));
    } else if (arg == "--list-models") {
      result.list_models = true;
      // optional next arg that isn't a flag
      const auto next_index = static_cast<std::size_t>(i) + 1;
      const std::string_view next_arg =
          next_index < args.size() ? args[next_index] : "";
      if (!next_arg.empty() && next_arg.front() != '-') {
        result.list_models_filter = args[static_cast<std::size_t>(++i)];
      }
    } else if (!arg.empty() && arg[0] == '-') {
      result.diagnostics.push_back(
          {.is_error = false,
           .message = "unknown option: " + std::string(arg)});
    } else {
      result.messages.emplace_back(arg);
    }
  }

  return result;
}

void print_help(const char *prog) {
  std::cout
      << "Usage: " << prog
      << " [options] [message...]\n\n"
         "Options:\n"
         "  --model, -m <id>            Model ID or \"provider/id\" shorthand\n"
         "  --provider <name>           Provider name (openai, anthropic, "
         "google, ...)\n"
         "  --base-url <url>            Base URL for OpenAI-compatible "
         "endpoint\n"
         "  --api-key <key>             API key (overrides env var)\n"
         "  --system-prompt <text>      System prompt\n"
         "  --append-system-prompt <t>  Append text to system prompt "
         "(repeatable)\n"
         "  --thinking <level>          Thinking level: off, minimal, low, "
         "medium, high, xhigh\n"
         "  --no-tools, -nt             Disable all tools\n"
         "  --no-builtin-tools, -nbt    Disable built-in tools only\n"
         "  --tools, -t <names>         Comma-separated allowlist of tool "
         "names\n"
         "  --tools-dir <dir>           Load Lua tools from directory\n"
         "  --hooks-file <file>         Lua hooks file, repeatable to stack "
         "add-ons\n"
         "  --hooks-dir <dir>           Load all .lua files from dir as "
         "add-ons\n"
         "  --render <mode>             Rendering: auto (default), markdown, "
         "raw, viewport, region\n"
         "  --message, -M <text>        Send an initial message then enter "
         "REPL\n"
         "  --print, -p                 Non-interactive: run prompt and exit\n"
         "  --mode rpc                  JSONL control protocol on "
         "stdin/stdout\n"
         "  --faux-control <socket>     JSONL faux control over Unix socket\n"
         "  --test <file>               Run Lua test file and exit "
         "(repeatable)\n"
         "  --continue, -c              Resume the most recent session\n"
         "  --resume, -r <prefix>       Resume session matching partial ID\n"
         "  --session-dir <path>        Override default session storage "
         "directory\n"
         "  --sandbox <mode>            Sandbox bash: auto, required, "
         "disabled\n"
         "  --no-sandbox                Alias for --sandbox disabled\n"
         "  --no-context-files, -nc     Disable AGENTS.md / CLAUDE.md "
         "discovery\n"
         "  --config <file>             Config file (default: "
         "~/.config/pici/config.toml)\n"
         "  --list-tools                List active tools and their sources\n"
         "  --list-addons               List loaded add-ons and their hooks\n"
         "  --list-models [filter]      List known models (optional search "
         "filter)\n"
         "  --otel-endpoint <url>       Export OpenTelemetry traces (e.g. "
         "http://localhost:4318)\n"
         "  --stream-trace <path>      Write privacy-safe streaming timing "
         "JSONL\n"
         "  --verbose                   Verbose output\n"
         "  --version, -v               Show version\n"
         "  --help, -h                  Show this help\n\n"
         "Arguments:\n"
         "  message                     Initial prompt message(s)\n\n"
         "Examples:\n"
         "  "
      << prog
      << "\n"
         "  "
      << prog
      << " \"What files are in this directory?\"\n"
         "  "
      << prog
      << " -p \"Summarize main.cpp\"\n"
         "  "
      << prog
      << " --model openai/gpt-4o --thinking high \"Explain this code\"\n"
         "  "
      << prog
      << " --model deepseek-chat --thinking medium\n"
         "  "
      << prog
      << " --base-url http://localhost:8080/v1 --model mymodel\n"
         "  "
      << prog
      << " --list-models\n"
         "  "
      << prog
      << " --list-models gpt\n\n"
         "Environment Variables:\n"
         "  OPENAI_API_KEY        OpenAI\n"
         "  ANTHROPIC_API_KEY     Anthropic (also ANTHROPIC_OAUTH_TOKEN)\n"
         "  GEMINI_API_KEY        Google Gemini\n"
         "  DEEPSEEK_API_KEY      DeepSeek\n"
         "  GROQ_API_KEY          Groq\n"
         "  XAI_API_KEY           xAI Grok\n"
         "  OPENROUTER_API_KEY    OpenRouter\n"
         "  MISTRAL_API_KEY       Mistral\n"
         "  CEREBRAS_API_KEY      Cerebras\n\n"
         "Built-in Tools: read, write, edit, bash, grep, find, ls\n";
}

} // namespace pi::cli
