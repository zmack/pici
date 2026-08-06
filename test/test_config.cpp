#include "cli/config.h"
#include "cli/args.h"

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <source_location>
#include <vector>
#include <string_view>

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
  }

  // merge: CLI string wins over config
  {
    Args conf; conf.model = "gpt-4o";      conf.provider = "openai";
    Args cli;  cli.model  = "gpt-4o-mini"; cli.provider  = "";
    auto out = merge_args(conf, cli);
    CHECK_EQ(out.model,    std::string("gpt-4o-mini")); // CLI wins
    CHECK_EQ(out.provider, std::string("openai"));       // config wins (CLI empty)
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

  // parse error throws
  {
    auto p = write_toml("pici_bad.toml", "not = valid [ toml");
    bool threw = false;
    try { load_config(p); } catch (const std::exception &) { threw = true; }
    CHECK(threw);
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

  std::cout << "\n========================================\n"
            << "  Tests: " << tests::total  << " total, "
            << tests::passed << " passed, "
            << tests::failed << " failed\n"
            << "========================================\n";
  return tests::failed == 0 ? 0 : 1;
}
