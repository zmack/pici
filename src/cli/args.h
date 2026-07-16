#pragma once

#include <string>
#include <vector>

namespace pi::cli {

enum class ThinkingLevel { off, minimal, low, medium, high, xhigh };

struct Args {
  // Model selection
  std::string model;    // model id or "provider/id"
  std::string provider; // explicit provider override
  std::string base_url; // explicit base URL (for local / custom endpoints)
  std::string api_key;  // explicit API key

  // Prompt / session
  std::string system_prompt;
  std::vector<std::string> append_system_prompts;
  std::vector<std::string> messages; // positional prompt args

  // Thinking
  ThinkingLevel thinking{ThinkingLevel::off};

  // Tools
  bool no_tools{false};
  bool no_builtin_tools{false};
  std::vector<std::string> tools;       // allowlist of tool names
  std::string tools_dir;                // directory for Lua tools
  std::vector<std::string> hooks_files; // Lua hooks files (repeatable)
  std::string hooks_dir; // load all .lua files from dir as add-ons

  // Output / rendering
  bool print_mode{false}; // -p: run prompt and exit
  bool rpc_mode{false};   // --mode rpc: JSONL control protocol on stdio
  std::string render;     // auto | markdown | raw

  // Listing
  bool list_models{false};
  std::string list_models_filter; // optional search pattern
  bool list_tools{false};
  bool list_addons{false};

  // Test mode
  std::vector<std::string>
      test_files; // --test <file>, run Lua test files and exit

  // Context files
  bool no_context_files{false}; // disable AGENTS.md / CLAUDE.md discovery

  // Session persistence
  bool session_continue{false}; // --continue / -c
  std::string session_resume;   // --resume / -r <prefix>  (empty = unset)
  std::string session_dir;      // --session-dir <path>

  // Config file
  std::string config_path; // --config <path>; empty = use default location

  // Observability
  std::string otel_endpoint; // --otel-endpoint <url>; empty = no OTel export
  std::string stream_trace;  // --stream-trace <path>; privacy-safe JSONL trace

  // Meta
  bool verbose{false};
  bool help{false};
  bool version{false};

  // Diagnostics collected during parsing
  struct Diagnostic {
    bool is_error;
    std::string message;
  };
  std::vector<Diagnostic> diagnostics;
};

Args parse_args(int argc, char *argv[]);
void print_help(const char *prog);

} // namespace pi::cli
