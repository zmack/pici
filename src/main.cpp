#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <source_location>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "core/agent.h"
#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"
#include "core/stream.h"

#ifdef PI_CPP_HAVE_HTTP
#include "http/http_client.h"
#endif

namespace pi::core {

// ─── Simple tool implementations ──────────────────────────────────────────

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

    ToolSchema& schema() override {
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

    std::shared_ptr<ToolResult> execute(
        std::string_view call_id,
        std::string_view args_json,
        std::stop_token) const override {
        (void)call_id;
        // Parse args and echo back
        return std::make_shared<EchoResult>(
            std::string(args_json) + " (echoed)");
    }

private:
    std::unique_ptr<ToolSchema> schema_;
};

} // namespace pi::core

namespace pi {

// ─── Version ──────────────────────────────────────────────────────────────

void print_version() {
    std::cout << "pi-cpp " PI_CPP_VERSION
              << " - C++23 agent loop runtime\n";
#ifdef PI_CPP_HAVE_HTTP
    std::cout << "  HTTP client: enabled\n";
#else
    std::cout << "  HTTP client: disabled\n";
#endif
    std::cout << "  C++ standard: C++23\n";
    std::cout << "  Thread lib: "
              << (std::thread::hardware_concurrency() ? std::to_string(
                     std::thread::hardware_concurrency())
                                                       : "unknown")
              << " cores available\n";
}

// ─── Demo: simple agent with echo tool ────────────────────────────────────

int demo() {
    std::cout << "=== pi-cpp Demo ===\n\n";

    // Create model config
    pi::core::Model model;
    model.id = "demo-model";
    model.name = "Demo Model";
    model.api = "openai-completions";
    model.provider = "demo";
    model.base_url = "http://localhost:11434/v1";
    model.context_window = 128000;
    model.max_tokens = 4096;

    // Create agent
    pi::core::Agent::Options opts;
    opts.model = model;
    opts.system_prompt = "You are a helpful demo assistant in C++. "
                         "You use the echo tool.";
    opts.get_api_key = [](std::string_view) -> std::optional<std::string> {
        return std::nullopt; // No API key needed for demo
    };
    opts.should_stop_after_turn = [](const pi::core::Message&,
                                     const std::vector<pi::core::ToolResultMessage>&,
                                     const pi::core::AgentContext&) {
        // Stop after one turn
        return true;
    };

    pi::core::Agent agent(opts);

    // Add echo tool
    std::shared_ptr<const pi::core::ToolDefinition> echo = std::static_pointer_cast<const pi::core::ToolDefinition>(std::make_shared<pi::core::EchoTool>());
    agent.add_tool(echo);

    // Print state
    std::cout << "Agent created with system prompt:\n";
    std::cout << "  " << agent.state().system_prompt() << "\n\n";

    std::cout << "Tools: " << agent.state().tools().size() << "\n";
    for (const auto& t : agent.state().tools()) {
        std::cout << "  - " << t->name() << ": " << t->description() << "\n";
    }

    // The agent can't actually call an LLM without a real client,
    // so we just demonstrate the structure
    std::cout << "\nAgent state:\n";
    std::cout << "  Model: " << agent.state().model().name << "\n";
    std::cout << "  Is streaming: " << (agent.state().is_streaming() ? "yes" : "no")
              << "\n";

    return 0;
}

// ─── Help ──────────────────────────────────────────────────────────────────

void print_help(const char* prog) {
    std::cout << "Usage: " << prog << " [command]\n\n"
              << "Commands:\n"
              << "  demo       Run the demo (echo tool)\n"
              << "  version    Print version info\n"
              << "  help       Show this help\n";
}

} // namespace pi

int main(int argc, char* argv[]) {
    std::signal(SIGINT, [](int) { std::exit(0); });
    std::signal(SIGTERM, [](int) { std::exit(0); });

    std::string command = argc > 1 ? argv[1] : "demo";

    if (command == "help" || command == "--help" || command == "-h") {
        pi::print_help(argv[0]);
    } else if (command == "version" || command == "--version" ||
               command == "-v") {
        pi::print_version();
    } else if (command == "demo") {
        return pi::demo();
    } else {
        std::cerr << "Unknown command: " << command << "\n";
        pi::print_help(argv[0]);
        return 1;
    }

    return 0;
}
