#include "cli/args.h"

#include "cli/config.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <source_location>
#include <string_view>
#include <vector>

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

TEST(Config, ParsingAndMerging) {

  // default_config_path doesn't throw
  EXPECT_TRUE(!default_config_path().empty());

  // Missing file returns empty Args (not an error)
  {
    auto cfg = load_config("/tmp/pici-nonexistent-config.toml");
    EXPECT_TRUE(cfg.model.empty());
    EXPECT_TRUE(cfg.provider.empty());
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

[agents]
write_tools = "core"

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
    EXPECT_EQ(cfg.model, std::string("gpt-4o"));
    EXPECT_EQ(cfg.provider, std::string("openai"));
    EXPECT_EQ(cfg.base_url, std::string("https://api.openai.com/v1"));
    EXPECT_EQ(cfg.system_prompt, std::string("You are helpful."));
    EXPECT_TRUE(cfg.thinking == ThinkingLevel::medium);
    EXPECT_EQ(cfg.tools_dir, std::string("/tmp/tools"));
    EXPECT_TRUE(!cfg.no_tools);
    EXPECT_EQ(cfg.tools.size(), std::size_t(2));
    EXPECT_EQ(cfg.hooks_files.size(), std::size_t(2));
    EXPECT_EQ(cfg.hooks_dir, std::string("/tmp/addons"));
    EXPECT_EQ(cfg.render, std::string("markdown"));
    EXPECT_TRUE(cfg.verbose);
    EXPECT_TRUE(cfg.no_context_files);
    EXPECT_TRUE(!cfg.no_skills); // [skills] absent in this fixture
    EXPECT_EQ(cfg.sandbox_mode, std::string("disabled"));
    EXPECT_EQ(cfg.agent_write_tools, std::string("core"));
    EXPECT_TRUE(cfg.mailbox_enabled);
    EXPECT_EQ(cfg.mailbox_path, std::string("~/custom-mailbox.sqlite3"));
    auto document = load_config_document(p);
    EXPECT_TRUE(document.mailbox.enabled);
    EXPECT_EQ(document.mailbox.scope, std::string("global"));
    EXPECT_EQ(document.mailbox.heartbeat_interval_ms, std::int64_t{3000});
    EXPECT_EQ(document.mailbox.retention_days, std::int64_t{45});
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
    EXPECT_TRUE(!config.has_errors());
    EXPECT_EQ(config.defaults.model, std::string("gpt-4.1"));
    EXPECT_EQ(config.providers.size(), std::size_t(2));

    const auto &local = config.providers.at("local");
    EXPECT_EQ(local.api.value(), std::string("openai-completions"));
    EXPECT_EQ(local.base_url.value(), std::string("http://127.0.0.1:8080/v1"));
    EXPECT_EQ(local.api_key.env_var.value(), std::string("PICI_LOCAL_KEY"));
    EXPECT_EQ(local.headers.at("X-Tenant"), std::string("engineering"));
    EXPECT_EQ(local.models.size(), std::size_t(2));
    EXPECT_EQ(local.models[0].input_capabilities->size(), std::size_t(2));
    EXPECT_EQ(local.models[0].input_capabilities->at(1), std::string("image"));
    EXPECT_EQ(local.models[0].headers.at("X-Model-Route"),
              std::string("vision"));
    EXPECT_EQ(local.models[0].cost.output_per_mtok.value(), 1.25);
    EXPECT_EQ(local.models[1].id, std::string("accounts/company/models/coder"));
    EXPECT_EQ(local.model_overrides.at("accounts/company/models/coder")
                  .max_tokens.value(),
              std::uint64_t(32768));
    EXPECT_EQ(config.providers.at("openai")
                  .model_overrides.at("gpt-4.1")
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
    EXPECT_TRUE(config.has_errors());
    EXPECT_TRUE(has_diagnostic(config, "providers.bad.auth"));
    EXPECT_TRUE(has_diagnostic(config, "providers.bad.api_key"));
    EXPECT_TRUE(has_diagnostic(config, "providers.bad.headers.Authorization"));
    EXPECT_TRUE(has_diagnostic(config, "providers.bad.headers.X-Number"));
    EXPECT_TRUE(
        has_diagnostic(config, "providers.bad.models[0].context_window"));
    EXPECT_TRUE(has_diagnostic(
        config, "providers.bad.models[0].input_capabilities[1]"));
    EXPECT_TRUE(has_diagnostic(
        config, "providers.bad.models[0].thinking_level_map.unsupported"));
    EXPECT_TRUE(has_diagnostic(config, "duplicate model id"));
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
    EXPECT_TRUE(has_diagnostic(config, "providers.openai-codex"));
  }

  // merge: CLI string wins over config
  {
    Args conf;
    conf.model = "gpt-4o";
    conf.provider = "openai";
    Args cli;
    cli.model = "gpt-4o-mini";
    cli.provider = "";
    auto out = merge_args(conf, cli);
    EXPECT_EQ(out.model, std::string("gpt-4o-mini")); // CLI wins
    EXPECT_EQ(out.provider, std::string("openai"));   // config wins (CLI empty)
  }

  // sandbox mode merges like other scalar configuration values
  {
    Args conf;
    conf.sandbox_mode = "required";
    Args cli;
    cli.sandbox_mode = "disabled";
    auto out = merge_args(conf, cli);
    EXPECT_EQ(out.sandbox_mode, std::string("disabled"));
  }

  // stream trace is a CLI-only diagnostic path
  {
    Args cli;
    cli.stream_trace = "/tmp/pici-stream.jsonl";
    auto out = merge_args({}, cli);
    EXPECT_EQ(out.stream_trace, std::string("/tmp/pici-stream.jsonl"));
  }

  // merge: booleans are OR'd
  {
    Args conf;
    conf.no_tools = true;
    conf.verbose = false;
    Args cli;
    cli.no_tools = false;
    cli.verbose = true;
    auto out = merge_args(conf, cli);
    EXPECT_TRUE(out.no_tools); // conf true OR cli false → true
    EXPECT_TRUE(out.verbose);  // conf false OR cli true → true
  }

  // merge: hooks_files accumulate (config first then CLI)
  {
    Args conf;
    conf.hooks_files = {"/conf/hook.lua"};
    Args cli;
    cli.hooks_files = {"/cli/hook.lua"};
    auto out = merge_args(conf, cli);
    EXPECT_EQ(out.hooks_files.size(), std::size_t(2));
    EXPECT_EQ(out.hooks_files[0], std::string("/conf/hook.lua"));
    EXPECT_EQ(out.hooks_files[1], std::string("/cli/hook.lua"));
  }

  // merge: CLI tools vector wins over config
  {
    Args conf;
    conf.tools = {"read", "bash"};
    Args cli;
    cli.tools = {"grep"};
    auto out = merge_args(conf, cli);
    EXPECT_EQ(out.tools.size(), std::size_t(1));
    EXPECT_EQ(out.tools[0], std::string("grep")); // CLI wins
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
    auto environment =
        load_and_merge(static_cast<int>(argv.size()), argv.data());
    EXPECT_EQ(environment.config_path, p.string());
    EXPECT_EQ(environment.mailbox_path, std::string("/env/mailbox.sqlite3"));
    EXPECT_TRUE(environment.mailbox_enabled);
    values.push_back("--mailbox");
    values.push_back("/cli/mailbox.sqlite3");
    argv.clear();
    for (auto &value : values)
      argv.push_back(value.data());
    auto command_line =
        load_and_merge(static_cast<int>(argv.size()), argv.data());
    EXPECT_EQ(command_line.mailbox_path, std::string("/cli/mailbox.sqlite3"));
    EXPECT_TRUE(command_line.mailbox_enabled);
    values = {"pi", "--no-mailbox"};
    argv.clear();
    for (auto &value : values)
      argv.push_back(value.data());
    auto disabled = load_and_merge(static_cast<int>(argv.size()), argv.data());
    EXPECT_TRUE(!disabled.mailbox_enabled);
    unsetenv("PICI_MAILBOX");
  }

  // parse error throws
  {
    auto p = write_toml("pici_bad.toml", "not = valid [ toml");
    bool threw = false;
    try {
      load_config(p);
    } catch (const std::exception &) {
      threw = true;
    }
    EXPECT_TRUE(threw);
  }

  // faux-control selects a Unix socket without consuming unrelated arguments
  {
    auto args =
        parse({"pi", "--faux-control", "/tmp/faux.sock", "--render", "region"});
    EXPECT_EQ(args.faux_control_socket, std::string("/tmp/faux.sock"));
    EXPECT_EQ(args.render, std::string("region"));
    EXPECT_TRUE(args.diagnostics.empty());
    std::vector<std::string> values{"pi", "--faux-control", "/tmp/merged.sock"};
    std::vector<char *> argv;
    for (auto &value : values)
      argv.push_back(value.data());
    auto merged = load_and_merge(static_cast<int>(argv.size()), argv.data());
    EXPECT_EQ(merged.faux_control_socket, std::string("/tmp/merged.sock"));
  }

  // auth commands have an explicit, CLI-only grammar
  {
    auto args = parse({"pi", "--config", "/tmp/config.toml", "auth", "login",
                       "openai-codex", "--device", "--verbose"});
    EXPECT_TRUE(args.auth_action == AuthAction::login);
    EXPECT_EQ(args.auth_provider, std::string("openai-codex"));
    EXPECT_TRUE(args.auth_device);
    EXPECT_TRUE(args.verbose);
    EXPECT_TRUE(args.diagnostics.empty());

    auto invalid =
        parse({"pi", "auth", "login", "openai-codex", "--model", "gpt-5.5"});
    EXPECT_TRUE(!invalid.diagnostics.empty());
    EXPECT_TRUE(invalid.diagnostics.front().is_error);
  }

  // --compaction-threshold parses a valid fraction
  {
    auto args =
        parse({"pi", "--remote-compaction", "--compaction-threshold", "0.7"});
    EXPECT_TRUE(args.remote_compaction_enabled);
    EXPECT_TRUE(args.compaction_threshold_pct > 0.699);
    EXPECT_TRUE(args.compaction_threshold_pct < 0.701);
    EXPECT_TRUE(args.diagnostics.empty());
  }

  // --compaction-threshold rejects an out-of-range fraction
  {
    auto args = parse({"pi", "--compaction-threshold", "1.5"});
    EXPECT_TRUE(!args.diagnostics.empty());
    EXPECT_TRUE(args.diagnostics.front().is_error);
  }

  // --compaction-threshold rejects garbage
  {
    auto args = parse({"pi", "--compaction-threshold", "not-a-number"});
    EXPECT_TRUE(!args.diagnostics.empty());
    EXPECT_TRUE(args.diagnostics.front().is_error);
  }

  // [compaction] threshold_pct loads from TOML
  {
    auto p = write_toml("pici_compaction.toml", R"toml(
[compaction]
remote_enabled = true
threshold_pct = 0.65
)toml");
    auto cfg = load_config(p);
    EXPECT_TRUE(cfg.remote_compaction_enabled);
    EXPECT_TRUE(cfg.compaction_threshold_pct > 0.649);
    EXPECT_TRUE(cfg.compaction_threshold_pct < 0.651);
  }

  // [skills] disabled loads from TOML and merges with the CLI flag
  {
    auto p = write_toml("pici_skills.toml", R"toml(
[skills]
disabled = true
)toml");
    auto cfg = load_config(p);
    EXPECT_TRUE(cfg.no_skills);

    auto merged = merge_args(cfg, parse({"pi"}));
    EXPECT_TRUE(merged.no_skills);
    auto cli_wins = merge_args(cfg, parse({"pi"}));
    EXPECT_TRUE(cli_wins.no_skills);
  }

  // an out-of-range TOML threshold_pct is ignored (falls back to the
  // effective default applied at the SessionRuntime construction site)
  {
    auto p = write_toml("pici_compaction_invalid.toml", R"toml(
[compaction]
threshold_pct = 1.5
)toml");
    auto cfg = load_config(p);
    EXPECT_EQ(cfg.compaction_threshold_pct, 0.0);
  }

  // merge: CLI threshold wins over config
  {
    Args conf;
    conf.compaction_threshold_pct = 0.6;
    Args cli;
    cli.compaction_threshold_pct = 0.9;
    auto out = merge_args(conf, cli);
    EXPECT_TRUE(out.compaction_threshold_pct > 0.899);
    EXPECT_TRUE(out.compaction_threshold_pct < 0.901);
  }

  // merge: config threshold survives when CLI leaves it unset
  {
    Args conf;
    conf.compaction_threshold_pct = 0.6;
    Args cli;
    auto out = merge_args(conf, cli);
    EXPECT_TRUE(out.compaction_threshold_pct > 0.599);
    EXPECT_TRUE(out.compaction_threshold_pct < 0.601);
  }
}
