#include "cli/config.h"
#include "cli/args.h"

#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <algorithm>
#include <source_location>
#include <string_view>
#include <vector>

namespace tests {
int passed{0}, failed{0}, total{0};
bool check(bool cond, std::string_view expr,
           std::source_location loc = std::source_location::current()) {
  ++total;
  if (cond) { ++passed; return true; }
  ++failed;
  std::cout << "  FAIL " << loc.file_name() << ":" << loc.line()
            << " — " << expr << "\n";
  return false;
}
} // namespace tests
#define CHECK(e) tests::check(!!(e), #e)
#define CHECK_EQ(a,b) tests::check((a)==(b), #a " == " #b)

using namespace pi::cli;

static std::filesystem::path write_toml(const char *name, const char *src) {
  auto p = std::filesystem::temp_directory_path() / name;
  std::ofstream(p) << src;
  return p;
}

static Args parse(std::initializer_list<std::string> values) {
  std::vector<std::string> storage(values);
  std::vector<char *> argv;
  argv.reserve(storage.size());
  for (auto &value : storage)
    argv.push_back(value.data());
  return parse_args(static_cast<int>(argv.size()), argv.data());
}

static bool has_diagnostic(const Config &config, std::string_view needle) {
  return std::ranges::any_of(config.diagnostics, [&](const auto &diagnostic) {
    return diagnostic.message.contains(needle);
  });
}

int main() {
  std::cout << "=== pi-cpp config tests ===\n\n";

  // default_config_path doesn't throw
  CHECK(!default_config_path().empty());

  // Missing file returns empty Args (not an error)
  {
    auto cfg = load_config("/tmp/pici-nonexistent-config.toml");
    CHECK(cfg.model.empty());
    CHECK(cfg.provider.empty());
  }

  // Full config round-trip
  {
    auto p = write_toml("pici_test.toml", R"toml(
[model]
id       = "gpt-4o"
provider = "openai"
base_url = "https://api.openai.com/v1"

[agent]
system_prompt = "You are helpful."
thinking      = "medium"

[tools]
dir      = "/tmp/tools"
disabled = false
no_builtin = false
list     = ["read", "bash"]

[addons]
files = ["/tmp/hook1.lua", "/tmp/hook2.lua"]
dir   = "/tmp/addons"

[display]
render  = "markdown"
verbose = true

[context]
disabled = true

[sandbox]
mode = "disabled"

[mailbox]
enabled = true
path = "~/custom-mailbox.sqlite3"
scope = "global"
heartbeat_interval_ms = 3000
stale_after_ms = 11000
poll_interval_ms = 400
claim_lease_ms = 31000
retention_days = 45
)toml");
    auto cfg = load_config(p);
    CHECK_EQ(cfg.model,          std::string("gpt-4o"));
    CHECK_EQ(cfg.provider,       std::string("openai"));
    CHECK_EQ(cfg.base_url,       std::string("https://api.openai.com/v1"));
    CHECK_EQ(cfg.system_prompt,  std::string("You are helpful."));
    CHECK(cfg.thinking == ThinkingLevel::medium);
    CHECK_EQ(cfg.tools_dir,      std::string("/tmp/tools"));
    CHECK(!cfg.no_tools);
    CHECK_EQ(cfg.tools.size(),   std::size_t(2));
    CHECK_EQ(cfg.hooks_files.size(), std::size_t(2));
    CHECK_EQ(cfg.hooks_dir,      std::string("/tmp/addons"));
    CHECK_EQ(cfg.render,         std::string("markdown"));
    CHECK(cfg.verbose);
    CHECK(cfg.no_context_files);
    CHECK_EQ(cfg.sandbox_mode, std::string("disabled"));
    CHECK(cfg.mailbox_enabled);
    CHECK_EQ(cfg.mailbox_path, std::string("~/custom-mailbox.sqlite3"));
    auto document = load_config_document(p);
    CHECK(document.mailbox.enabled);
    CHECK_EQ(document.mailbox.scope, std::string("global"));
    CHECK_EQ(document.mailbox.heartbeat_interval_ms, std::int64_t{3000});
    CHECK_EQ(document.mailbox.retention_days, std::int64_t{45});
  }

  // Provider and custom-model definitions remain separate from Args defaults.
  {
    auto p = write_toml("pici_providers.toml", R"toml(
[model]
id = "gpt-4.1"
provider = "openai"

[providers.local]
api = "openai-completions"
base_url = "http://127.0.0.1:8080/v1"
api_key_env = "PICI_LOCAL_KEY"
headers = { X-Tenant = "engineering" }

[[providers.local.models]]
id = "qwen3-coder"
name = "Qwen 3 Coder (Local)"
context_window = 131072
max_tokens = 16384
reasoning = true
input_capabilities = ["text", "text", "image"]
headers = { X-Model-Route = "vision" }
thinking_level_map = { off = false, low = "low", high = "high" }

[providers.local.models.cost]
input_per_mtok = 0.0
output_per_mtok = 1.25

[[providers.local.models]]
id = "accounts/company/models/coder"

[providers.local.model_overrides."accounts/company/models/coder"]
max_tokens = 32768

[providers.openai.model_overrides."gpt-4.1"]
context_window = 200000
)toml");
    auto config = load_config_document(p);
    CHECK(!config.has_errors());
    CHECK_EQ(config.defaults.model, std::string("gpt-4.1"));
    CHECK_EQ(config.providers.size(), std::size_t(2));

    const auto &local = config.providers.at("local");
    CHECK_EQ(local.api.value(), std::string("openai-completions"));
    CHECK_EQ(local.base_url.value(), std::string("http://127.0.0.1:8080/v1"));
    CHECK_EQ(local.api_key.env_var.value(), std::string("PICI_LOCAL_KEY"));
    CHECK_EQ(local.headers.at("X-Tenant"), std::string("engineering"));
    CHECK_EQ(local.models.size(), std::size_t(2));
    CHECK_EQ(local.models[0].input_capabilities->size(), std::size_t(2));
    CHECK_EQ(local.models[0].input_capabilities->at(1), std::string("image"));
    CHECK_EQ(local.models[0].headers.at("X-Model-Route"),
             std::string("vision"));
    CHECK_EQ(local.models[0].cost.output_per_mtok.value(), 1.25);
    CHECK_EQ(local.models[1].id, std::string("accounts/company/models/coder"));
    CHECK_EQ(local.model_overrides.at("accounts/company/models/coder")
                 .max_tokens.value(),
             std::uint64_t(32768));
    CHECK_EQ(config.providers.at("openai").model_overrides.at("gpt-4.1")
                 .context_window.value(),
             std::uint64_t(200000));
  }

  // Recognized provider/model schema errors carry their TOML paths.
  {
    auto p = write_toml("pici_invalid_providers.toml", R"toml(
[providers.bad]
api = "unregistered-api"
base_url = "http://localhost/v1"
auth = "sometimes"
api_key = "literal"
api_key_env = "PICI_BAD_KEY"
headers = { Authorization = "wrong", X-Number = 42 }

[[providers.bad.models]]
id = "duplicate"
reasoning = false
context_window = 0
thinking_level_map = { high = "high", unsupported = "low" }
input_capabilities = ["text", "audio", "text"]

[[providers.bad.models]]
id = "duplicate"
)toml");
    auto config = load_config_document(p);
    CHECK(config.has_errors());
    CHECK(has_diagnostic(config, "providers.bad.auth"));
    CHECK(has_diagnostic(config, "providers.bad.api_key"));
    CHECK(has_diagnostic(config, "providers.bad.headers.Authorization"));
    CHECK(has_diagnostic(config, "providers.bad.headers.X-Number"));
    CHECK(has_diagnostic(config, "providers.bad.models[0].context_window"));
    CHECK(has_diagnostic(config,
                        "providers.bad.models[0].input_capabilities[1]"));
    CHECK(has_diagnostic(config,
                        "providers.bad.models[0].thinking_level_map.unsupported"));
    CHECK(has_diagnostic(config, "duplicate model id"));
  }

  // OAuth-only built-in providers cannot be given a plaintext or env key.
  {
    auto p = write_toml("pici_invalid_oauth.toml", R"toml(
[providers.openai-codex]
api = "openai-codex-responses"
base_url = "https://chatgpt.com/backend-api"
auth = "oauth"
api_key_env = "PICI_CODEX_KEY"
)toml");
    auto config = load_config_document(p);
    CHECK(has_diagnostic(config, "providers.openai-codex"));
  }

  // merge: CLI string wins over config
  {
    Args conf; conf.model = "gpt-4o";      conf.provider = "openai";
    Args cli;  cli.model  = "gpt-4o-mini"; cli.provider  = "";
    auto out = merge_args(conf, cli);
    CHECK_EQ(out.model,    std::string("gpt-4o-mini")); // CLI wins
    CHECK_EQ(out.provider, std::string("openai"));       // config wins (CLI empty)
  }

  // sandbox mode merges like other scalar configuration values
  {
    Args conf; conf.sandbox_mode = "required";
    Args cli;  cli.sandbox_mode = "disabled";
    auto out = merge_args(conf, cli);
    CHECK_EQ(out.sandbox_mode, std::string("disabled"));
  }

  // stream trace is a CLI-only diagnostic path
  {
    Args cli;
    cli.stream_trace = "/tmp/pici-stream.jsonl";
    auto out = merge_args({}, cli);
    CHECK_EQ(out.stream_trace, std::string("/tmp/pici-stream.jsonl"));
  }

  // merge: booleans are OR'd
  {
    Args conf; conf.no_tools = true;  conf.verbose = false;
    Args cli;  cli.no_tools  = false; cli.verbose  = true;
    auto out = merge_args(conf, cli);
    CHECK(out.no_tools); // conf true OR cli false → true
    CHECK(out.verbose);  // conf false OR cli true → true
  }

  // merge: hooks_files accumulate (config first then CLI)
  {
    Args conf; conf.hooks_files = {"/conf/hook.lua"};
    Args cli;  cli.hooks_files  = {"/cli/hook.lua"};
    auto out = merge_args(conf, cli);
    CHECK_EQ(out.hooks_files.size(), std::size_t(2));
    CHECK_EQ(out.hooks_files[0], std::string("/conf/hook.lua"));
    CHECK_EQ(out.hooks_files[1], std::string("/cli/hook.lua"));
  }

  // merge: CLI tools vector wins over config
  {
    Args conf; conf.tools = {"read", "bash"};
    Args cli;  cli.tools  = {"grep"};
    auto out = merge_args(conf, cli);
    CHECK_EQ(out.tools.size(), std::size_t(1));
    CHECK_EQ(out.tools[0], std::string("grep")); // CLI wins
  }

  // Mailbox path precedence is CLI, then environment, then TOML.
  {
    auto p = write_toml("pici_mailbox_precedence.toml", R"toml(
[mailbox]
enabled = false
path = "/toml/mailbox.sqlite3"
)toml");
    setenv("PICI_MAILBOX", "/env/mailbox.sqlite3", 1);
    std::vector<std::string> values{"pi", "--config", p.string()};
    std::vector<char *> argv;
    for (auto &value : values)
      argv.push_back(value.data());
    auto environment = load_and_merge(static_cast<int>(argv.size()), argv.data());
    CHECK_EQ(environment.config_path, p.string());
    CHECK_EQ(environment.mailbox_path, std::string("/env/mailbox.sqlite3"));
    CHECK(environment.mailbox_enabled);
    values.push_back("--mailbox");
    values.push_back("/cli/mailbox.sqlite3");
    argv.clear();
    for (auto &value : values)
      argv.push_back(value.data());
    auto command_line = load_and_merge(static_cast<int>(argv.size()), argv.data());
    CHECK_EQ(command_line.mailbox_path, std::string("/cli/mailbox.sqlite3"));
    CHECK(command_line.mailbox_enabled);
    values = {"pi", "--no-mailbox"};
    argv.clear();
    for (auto &value : values)
      argv.push_back(value.data());
    auto disabled = load_and_merge(static_cast<int>(argv.size()), argv.data());
    CHECK(!disabled.mailbox_enabled);
    unsetenv("PICI_MAILBOX");
  }

  // parse error throws
  {
    auto p = write_toml("pici_bad.toml", "not = valid [ toml");
    bool threw = false;
    try { load_config(p); } catch (const std::exception &) { threw = true; }
    CHECK(threw);
  }

  // faux-control selects a Unix socket without consuming unrelated arguments
  {
    auto args =
        parse({"pi", "--faux-control", "/tmp/faux.sock", "--render", "region"});
    CHECK_EQ(args.faux_control_socket, std::string("/tmp/faux.sock"));
    CHECK_EQ(args.render, std::string("region"));
    CHECK(args.diagnostics.empty());
    std::vector<std::string> values{"pi", "--faux-control", "/tmp/merged.sock"};
    std::vector<char *> argv;
    for (auto &value : values)
      argv.push_back(value.data());
    auto merged = load_and_merge(static_cast<int>(argv.size()), argv.data());
    CHECK_EQ(merged.faux_control_socket, std::string("/tmp/merged.sock"));
  }

  // auth commands have an explicit, CLI-only grammar
  {
    auto args = parse({"pi", "--config", "/tmp/config.toml", "auth", "login",
                       "openai-codex", "--device", "--verbose"});
    CHECK(args.auth_action == AuthAction::login);
    CHECK_EQ(args.auth_provider, std::string("openai-codex"));
    CHECK(args.auth_device);
    CHECK(args.verbose);
    CHECK(args.diagnostics.empty());

    auto invalid = parse({"pi", "auth", "login", "openai-codex", "--model",
                          "gpt-5.5"});
    CHECK(!invalid.diagnostics.empty());
    CHECK(invalid.diagnostics.front().is_error);
  }

  // --compaction-threshold parses a valid fraction
  {
    auto args = parse({"pi", "--remote-compaction", "--compaction-threshold",
                       "0.7"});
    CHECK(args.remote_compaction_enabled);
    CHECK(args.compaction_threshold_pct > 0.699);
    CHECK(args.compaction_threshold_pct < 0.701);
    CHECK(args.diagnostics.empty());
  }

  // --compaction-threshold rejects an out-of-range fraction
  {
    auto args = parse({"pi", "--compaction-threshold", "1.5"});
    CHECK(!args.diagnostics.empty());
    CHECK(args.diagnostics.front().is_error);
  }

  // --compaction-threshold rejects garbage
  {
    auto args = parse({"pi", "--compaction-threshold", "not-a-number"});
    CHECK(!args.diagnostics.empty());
    CHECK(args.diagnostics.front().is_error);
  }

  // [compaction] threshold_pct loads from TOML
  {
    auto p = write_toml("pici_compaction.toml", R"toml(
[compaction]
remote_enabled = true
threshold_pct = 0.65
)toml");
    auto cfg = load_config(p);
    CHECK(cfg.remote_compaction_enabled);
    CHECK(cfg.compaction_threshold_pct > 0.649);
    CHECK(cfg.compaction_threshold_pct < 0.651);
  }

  // an out-of-range TOML threshold_pct is ignored (falls back to the
  // effective default applied at the AgentSession construction site)
  {
    auto p = write_toml("pici_compaction_invalid.toml", R"toml(
[compaction]
threshold_pct = 1.5
)toml");
    auto cfg = load_config(p);
    CHECK_EQ(cfg.compaction_threshold_pct, 0.0);
  }

  // merge: CLI threshold wins over config
  {
    Args conf;
    conf.compaction_threshold_pct = 0.6;
    Args cli;
    cli.compaction_threshold_pct = 0.9;
    auto out = merge_args(conf, cli);
    CHECK(out.compaction_threshold_pct > 0.899);
    CHECK(out.compaction_threshold_pct < 0.901);
  }

  // merge: config threshold survives when CLI leaves it unset
  {
    Args conf;
    conf.compaction_threshold_pct = 0.6;
    Args cli;
    auto out = merge_args(conf, cli);
    CHECK(out.compaction_threshold_pct > 0.599);
    CHECK(out.compaction_threshold_pct < 0.601);
  }

  std::cout << "\n========================================\n"
            << "  Tests: " << tests::total  << " total, "
            << tests::passed << " passed, "
            << tests::failed << " failed\n"
            << "========================================\n";
  return tests::failed == 0 ? 0 : 1;
}
