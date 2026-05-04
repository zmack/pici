#include <csignal>
#include <cstdlib>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/agent.h"
#include "core/agent_state.h"
#include "core/builtin_tools.h"
#include "core/env_api_keys.h"
#include "core/lua_tool.h"
#include "core/event_types.h"
#include "core/message_types.h"
#include "core/providers/openai_completions.h"

namespace pi::core {

class EchoTool : public ToolDefinition {
public:
  EchoTool() = default;

  std::string_view name() const override { return "echo"; }
  std::string_view description() const override {
    return "Echoes back the input text";
  }

  class EchoSchema : public ToolSchema {
  public:
    std::string serialize() const override {
      return R"({
                "type": "object",
                "properties": {
                    "text": {"type": "string", "description": "Text to echo"}
                },
                "required": ["text"]
            })";
    }

    std::map<std::string, std::string> to_definition() const override {
      std::map<std::string, std::string> result;
      result["type"] = "object";
      result["properties"] = R"({"text": {"type": "string"}})";
      result["required"] = R"(["text"])";
      return result;
    }
  };

  ToolSchema &schema() const override {
    if (!schema_) {
      schema_ = std::make_unique<EchoSchema>();
    }
    return *schema_;
  }

  class EchoResult : public ToolResult {
  public:
    EchoResult(std::string content, bool error = false)
        : content_(std::move(content)), error_(error) {}

    bool is_error() const override { return error_; }
    std::string content() const override { return content_; }
    std::optional<std::string> details() const override { return std::nullopt; }

  private:
    std::string content_;
    bool error_;
  };

  std::shared_ptr<ToolResult> execute(std::string_view call_id,
                                      std::string_view args_json,
                                      std::stop_token,
                                      ToolUpdateCallback) const override {
    (void)call_id;
    return std::make_shared<EchoResult>(std::string(args_json) + " (echoed)");
  }

private:
  mutable std::unique_ptr<ToolSchema> schema_;
};

} // namespace pi::core

namespace pi {

namespace {
constexpr std::string_view kDefaultLocalBaseUrl = "http://127.0.0.1:8080/v1";
constexpr std::string_view kDefaultLocalModel =
    "Qwen3.6-35B-A3B-UD-IQ4_NL.gguf";
constexpr std::string_view kDefaultLocalProvider = "llamacpp";
} // namespace

static void print_version() {
  std::cout << "pi-cpp " PI_CPP_VERSION << " - C++23 agent loop runtime\n";
  std::cout << "  HTTP client: enabled\n";
  std::cout << "  C++ standard: C++23\n";
  std::cout << "  Thread lib: "
            << ((std::thread::hardware_concurrency() != 0u)
                    ? std::to_string(std::thread::hardware_concurrency())
                    : "unknown")
            << " cores available\n";
}

static int demo() {
  std::cout << "=== pi-cpp Demo ===\n\n";

  pi::core::Model model;
  model.id = "demo-model";
  model.name = "Demo Model";
  model.api = "openai-completions";
  model.provider = std::string(kDefaultLocalProvider);
  model.base_url = std::string(kDefaultLocalBaseUrl);
  model.context_window = 128000;
  model.max_tokens = 4096;

  pi::core::Agent::Options opts;
  opts.model = model;
  opts.system_prompt = "You are a helpful demo assistant in C++. "
                       "You use the echo tool.";
  opts.get_api_key = [](std::string_view) -> std::optional<std::string> {
    return std::nullopt;
  };
  opts.should_stop_after_turn =
      [](const pi::core::Message &,
         const std::vector<pi::core::ToolResultMessage> &,
         const pi::core::AgentContext &) { return true; };

  pi::core::Agent agent(opts);
  agent.set_tools(pi::core::create_all_tools());

  std::shared_ptr<const pi::core::ToolDefinition> echo =
      std::static_pointer_cast<const pi::core::ToolDefinition>(
          std::make_shared<pi::core::EchoTool>());
  agent.add_tool(echo);

  std::cout << "Agent created with system prompt:\n";
  std::cout << "  " << agent.state().system_prompt() << "\n\n";

  std::cout << "Tools: " << agent.state().tools().size() << "\n";
  for (const auto &t : agent.state().tools()) {
    std::cout << "  - " << t->name() << ": " << t->description() << "\n";
  }

  std::cout << "\nAgent state:\n";
  std::cout << "  Model: " << agent.state().model().name << "\n";
  std::cout << "  Is streaming: "
            << (agent.state().is_streaming() ? "yes" : "no") << "\n";

  return 0;
}

static int chat(int argc, char *argv[]) {
  std::string model_id(kDefaultLocalModel);
  std::string provider(kDefaultLocalProvider);
  std::string base_url(kDefaultLocalBaseUrl);
  std::string system;
  std::string tools_dir;

  for (int i = 2; i < argc; ++i) {
    std::string_view arg = argv[i];
    auto next = [&]() -> std::string_view {
      return (i + 1 < argc) ? argv[++i] : "";
    };
    if (arg == "--model")
      model_id = next();
    else if (arg == "--provider")
      provider = next();
    else if (arg == "--base-url")
      base_url = next();
    else if (arg == "--system")
      system = next();
    else if (arg == "--tools")
      tools_dir = next();
  }

  pi::core::Model model;
  model.id = model_id;
  model.name = model_id;
  model.api = "openai-completions";
  model.provider = provider;
  model.base_url = base_url;

  pi::core::Agent::Options opts;
  opts.model = model;
  opts.system_prompt = system;
  opts.get_api_key =
      [prov = provider](std::string_view p) -> std::optional<std::string> {
    return pi::core::get_env_api_key(p);
  };
  opts.should_stop_after_turn = nullptr;

  pi::core::Agent agent(opts);
  agent.set_tools(pi::core::create_all_tools());
  if (!tools_dir.empty()) {
    for (auto &t : pi::core::load_lua_tools(tools_dir)) {
      agent.add_tool(t);
    }
  }

  std::string line;
  while (true) {
    std::cout << "\n> " << std::flush;
    if (!std::getline(std::cin, line))
      break;
    if (line.empty())
      continue;
    if (line == "/exit" || line == "/quit")
      break;

    auto stream = agent.prompt(line);
    bool done = false;
    bool printed_text = false;
    for (const auto &ev : stream) {
      if (done)
        break;
      std::visit(
          [&done, &printed_text](const auto &e) {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, pi::core::MessageUpdateEvent>) {
              std::visit(
                  [&printed_text](const auto &ae) {
                    using AE = std::decay_t<decltype(ae)>;
                    if constexpr (std::is_same_v<
                                      AE, pi::core::
                                              AssistantMessageTextDeltaEvent>) {
                      std::cout << ae.delta << std::flush;
                      printed_text = true;
                    } else if constexpr (
                        std::is_same_v<AE,
                                       pi::core::AssistantMessageErrorEvent>) {
                      std::cerr << "\nerror: "
                                << ae.error.error_message.value_or(
                                       "LLM request failed")
                                << "\n";
                    }
                  },
                  e.assistant_message_event);
            } else if constexpr (std::is_same_v<T, pi::core::MessageEndEvent>) {
              if (!printed_text) {
                if (const auto *am =
                        std::get_if<pi::core::AssistantMessage>(&e.message)) {
                  if (am->error_message) {
                    std::cerr << "\nerror: " << *am->error_message << "\n";
                  } else {
                    std::string final_text;
                    for (const auto &block : am->content) {
                      if (const auto *text =
                              std::get_if<pi::core::TextContent>(&block)) {
                        final_text += text->text;
                      }
                    }
                    if (!final_text.empty()) {
                      std::cout << final_text << std::flush;
                      printed_text = true;
                    }
                  }
                }
              }
              std::cout << "\n" << std::flush;
            } else if constexpr (std::is_same_v<
                                     T,
                                     pi::core::ToolExecutionStartEvent>) {
              std::cout << "\n[tool:" << e.tool_name << "]\n" << std::flush;
            } else if constexpr (std::is_same_v<
                                     T,
                                     pi::core::ToolExecutionEndEvent>) {
              if (e.result) {
                std::cout << e.result->content() << "\n" << std::flush;
              }
            } else if constexpr (std::is_same_v<T, pi::core::AgentEndEvent>) {
              done = true;
            }
          },
          ev);
    }
  }

  return 0;
}

static void print_help(const char *prog) {
  std::cout << "Usage: " << prog << " [command]\n\n"
            << "Commands:\n"
            << "  chat [--model <id>] [--provider <name>] [--base-url <url>] "
               "[--system <prompt>] [--tools <dir>]\n"
            << "             Interactive chat REPL. Defaults to "
            << kDefaultLocalBaseUrl << " with model " << kDefaultLocalModel
            << ". Built-in tools: read, bash, edit, write, grep, find, ls"
            << ". Use --tools <dir> to load Lua tools from a directory."
            << "\n"
            << "  demo       Run the demo (echo tool)\n"
            << "  version    Print version info\n"
            << "  help       Show this help\n";
}

} // namespace pi

int main(int argc, char *argv[]) {
  std::signal(SIGINT, [](int) { std::exit(0); });
  std::signal(SIGTERM, [](int) { std::exit(0); });
  pi::core::register_openai_completions_client();

  std::string command = argc > 1 ? argv[1] : "demo";

  if (command == "help" || command == "--help" || command == "-h") {
    pi::print_help(argv[0]);
  } else if (command == "version" || command == "--version" ||
             command == "-v") {
    pi::print_version();
  } else if (command == "demo") {
    return pi::demo();
  } else if (command == "chat") {
    return pi::chat(argc, argv);
  } else {
    std::cerr << "Unknown command: " << command << "\n";
    pi::print_help(argv[0]);
    return 1;
  }

  return 0;
}
