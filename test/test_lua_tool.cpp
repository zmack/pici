#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <ranges>
#include <source_location>
#include <string>
#include <string_view>
#include <vector>

#include "core/lua_tool.h"
#include <nlohmann/json.hpp>

using namespace pi::core;

namespace tests {

int passed{0};
int failed{0};
int total{0};
int current_failed{0};

bool CHECK_impl(bool cond, bool expected, std::string_view expr,
                std::source_location loc = std::source_location::current()) {
  if (cond != expected) {
    current_failed++;
    std::cerr << "  FAIL " << loc.file_name() << ":" << loc.line() << " - "
              << expr << " (expected " << expected << ", got " << cond
              << ")\n";
    return false;
  }
  return true;
}

#define CHECK(cond)                                                            \
  (::tests::CHECK_impl(static_cast<bool>(cond), true, #cond,                  \
                       std::source_location::current()))

#define CHECK_EQ(a, b)                                                         \
  (::tests::CHECK_impl((a) == (b), true, #a " == " #b,                        \
                       std::source_location::current()))

void register_test(std::string name, std::function<void()> fn) {
  total++;
  current_failed = 0;
  fn();
  if (current_failed == 0) {
    passed++;
    std::cout << "  PASS " << name << "\n";
  } else {
    failed++;
    std::cout << "  FAIL " << name << "\n";
  }
}

void print_summary() {
  std::cout << "\n========================================\n";
  std::cout << "  Tests: " << total << " total, " << passed << " passed, "
            << failed << " failed\n";
  std::cout << "========================================\n";
}

} // namespace tests

static std::filesystem::path write_lua(const std::filesystem::path &dir,
                                       const std::string &name,
                                       const std::string &src) {
  auto path = dir / name;
  std::ofstream(path) << src;
  return path;
}

static LuaContextSnapshot empty_context() { return {}; }

class ContextTool final : public ToolDefinition {
public:
  explicit ContextTool(std::string schema) : schema_(std::move(schema)) {}

  std::string_view name() const override { return "context_tool"; }
  std::string_view description() const override { return "context test tool"; }
  ToolSchema &schema() const override { return schema_; }
  std::shared_ptr<ToolResult>
  execute(std::string_view, std::string_view, std::stop_token,
          ToolUpdateCallback) const override {
    return nullptr;
  }

private:
  class Schema final : public ToolSchema {
  public:
    explicit Schema(std::string value) : value_(std::move(value)) {}
    std::string serialize() const override { return value_; }
    std::map<std::string, std::string> to_definition() const override {
      return {};
    }

  private:
    std::string value_;
  } mutable schema_;
};

void test_load_and_metadata() {
  tests::register_test("LuaTool: load and metadata", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-meta";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "greet.lua", R"lua(
return {
  name = "greet",
  description = "Returns a greeting",
  schema = '{"type":"object","properties":{"name":{"type":"string"}},"required":["name"],"additionalProperties":false}',
  execute = function(args)
    return "Hello, " .. args.name .. "!"
  end
}
)lua");

    auto tool = load_lua_tool(path);
    CHECK_EQ(std::string(tool->name()), "greet");
    CHECK_EQ(std::string(tool->description()), "Returns a greeting");
    CHECK(tool->schema().serialize().find("\"type\"") != std::string::npos);

    std::filesystem::remove_all(dir);
  });
}

void test_execute_string_return() {
  tests::register_test("LuaTool: execute returns string", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-str";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "echo.lua", R"lua(
return {
  name = "echo",
  description = "Echoes input",
  execute = function(args)
    return args.text
  end
}
)lua");

    auto tool = load_lua_tool(path);
    auto result = tool->execute("1", R"({"text":"hello world"})");
    CHECK(!result->is_error());
    CHECK_EQ(result->content(), "hello world");

    std::filesystem::remove_all(dir);
  });
}

void test_lua_tool_progress() {
  tests::register_test("LuaTool: execute reports progress updates", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-progress";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "progress.lua", R"lua(
return {
  name = "progress",
  description = "Reports progress",
  execute = function(args, ctx)
    ctx.update("first")
    ctx.update({content = "second"})
    return "done"
  end
}
)lua");

    auto tool = load_lua_tool(path);
    int updates = 0;
    std::string last_update;
    auto result = tool->execute(
        "1", "{}", {}, [&](const std::shared_ptr<ToolResult> &partial) {
          ++updates;
          last_update = partial->content();
        });
    CHECK(!result->is_error());
    CHECK_EQ(updates, 2);
    CHECK_EQ(last_update, "second");

    std::filesystem::remove_all(dir);
  });
}

void test_execute_table_return() {
  tests::register_test("LuaTool: execute returns table", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-tbl";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "fail.lua", R"lua(
return {
  name = "fail",
  description = "Always fails with details",
  execute = function(args)
    return { content = "something went wrong", is_error = true, details = "extra info" }
  end
}
)lua");

    auto tool = load_lua_tool(path);
    auto result = tool->execute("1", "{}");
    CHECK(result->is_error());
    CHECK_EQ(result->content(), "something went wrong");
    CHECK(result->details().has_value());
    CHECK_EQ(result->details().value(), "extra info");

    std::filesystem::remove_all(dir);
  });
}

void test_lua_runtime_error() {
  tests::register_test("LuaTool: Lua runtime error returns error result", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-err";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "boom.lua", R"lua(
return {
  name = "boom",
  description = "Crashes",
  execute = function(args)
    error("intentional crash")
  end
}
)lua");

    auto tool = load_lua_tool(path);
    auto result = tool->execute("1", "{}");
    CHECK(result->is_error());
    CHECK(result->content().find("intentional crash") != std::string::npos);

    std::filesystem::remove_all(dir);
  });
}

void test_load_syntax_error() {
  tests::register_test("LuaTool: syntax error throws", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-syn";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "bad.lua", "this is not valid lua !!!@#$");

    bool threw = false;
    try {
      load_lua_tool(path);
    } catch (const std::runtime_error &) {
      threw = true;
    }
    CHECK(threw);

    std::filesystem::remove_all(dir);
  });
}

void test_json_bridge() {
  tests::register_test("LuaTool: json module available in Lua", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-json";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    auto path = write_lua(dir, "jsontest.lua", R"lua(
return {
  name = "jsontest",
  description = "Tests json module",
  execute = function(args)
    local t = { x = 1, y = "two" }
    local encoded = json.encode(t)
    local decoded = json.decode(encoded)
    return tostring(decoded.x) .. ":" .. decoded.y
  end
}
)lua");

    auto tool = load_lua_tool(path);
    auto result = tool->execute("1", "{}");
    CHECK(!result->is_error());
    CHECK_EQ(result->content(), "1:two");

    std::filesystem::remove_all(dir);
  });
}

void test_load_lua_tools_directory() {
  tests::register_test("load_lua_tools: loads all .lua files", []() {
    const auto dir =
        std::filesystem::temp_directory_path() / "pici-lua-test-dir";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    write_lua(dir, "tool_a.lua", R"lua(
return { name="tool_a", description="A", execute=function(a) return "a" end }
)lua");
    write_lua(dir, "tool_b.lua", R"lua(
return { name="tool_b", description="B", execute=function(a) return "b" end }
)lua");
    // non-.lua file should be ignored
    std::ofstream(dir / "ignored.txt") << "not a tool";
    // bad lua should be skipped
    write_lua(dir, "bad.lua", "syntax error !!!");

    auto tools = load_lua_tools(dir);
    CHECK_EQ(tools.size(), std::size_t(2));

    std::filesystem::remove_all(dir);
  });
}

void test_lua_hooks() {
  const auto dir = std::filesystem::temp_directory_path() / "pici-lua-hooks-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);

  // Helper: write a hooks file and return its path
  auto write_hooks = [&](const char *name, const char *src) {
    auto p = dir / name;
    std::ofstream(p) << src;
    return p;
  };

  tests::register_test("LuaHooks: before_tool_call blocks", [&]() {
    auto p = write_hooks("before_block.lua", R"lua(
return {
  before_tool_call = function(ctx)
    if ctx.tool_name == "bash" then
      return {block=true, reason="bash not allowed"}
    end
  end
}

)lua");
    auto hooks = load_lua_hooks(p);
    CHECK(hooks->before_tool_call != nullptr);
    CHECK(!hooks->after_tool_call);
    CHECK(!hooks->should_stop_after_turn);

    // Build a minimal BeforeToolCallContext
    ToolCall tc;
    tc.id = "id1"; tc.name = "bash";
    tc.arguments = nlohmann::json::object();
    AgentContext ctx;
    AssistantMessage am;
    BeforeToolCallContext bctx{am, tc, "{}", ctx};

    auto result = hooks->before_tool_call(bctx, std::stop_token{});
    CHECK(result.has_value());
    CHECK(result->block);
    CHECK(result->reason == "bash not allowed");

    // Non-blocked tool
    tc.name = "read";
    BeforeToolCallContext bctx2{am, tc, "{}", ctx};
    auto result2 = hooks->before_tool_call(bctx2, std::stop_token{});
    CHECK(!result2.has_value());
  });

  tests::register_test("LuaHooks: permission hook errors fail closed", [&]() {
    auto p = write_hooks("before_error.lua", R"lua(
return {
  before_tool_call = function(_)
    error("policy crashed")
  end
}
)lua");
    auto hooks = load_lua_hooks(p);

    ToolCall tc;
    tc.id = "error-call";
    tc.name = "bash";
    tc.arguments = nlohmann::json::object();
    AgentContext ctx;
    AssistantMessage am;
    BeforeToolCallContext bctx{am, tc, "{}", ctx};
    auto result = hooks->before_tool_call(bctx, {});
    CHECK(result.has_value());
    CHECK(result->block);
    CHECK(result->reason.find("policy crashed") != std::string::npos);
  });

  tests::register_test("LuaHooks: on_event receives canonical envelope", [&]() {
    auto p = write_hooks("event.lua", R"lua(
local seen = ""
return {
  on_event = function(event)
    seen = event.event .. ":" .. tostring(event.sequence)
  end,
  on_command = function(cmd)
    if cmd == "seen" then return {handled=true, output=seen} end
  end,
}
)lua");
    auto hooks = load_lua_hooks(p);
    AgentStartEvent event;
    event.sequence = 7;
    hooks->on_event(event);
    auto result = hooks->on_command("seen", "", {}, empty_context());
    CHECK(result.handled);
    CHECK(result.output.has_value());
    CHECK_EQ(*result.output, "agent_start:7");
  });

  tests::register_test("LuaHooks: prepare_context returns request messages", [&]() {
    auto p = write_hooks("prepare.lua", R"lua(
return {
  prepare_context = function(ctx)
    if ctx.estimated_tokens > 100 then
      return {messages={ctx.messages[1]}}
    end
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    AgentContext context;
    UserMessage user;
    user.content.emplace_back(TextContent{.text = "keep me"});
    context.messages.emplace_back(std::move(user));
    auto result = hooks->prepare_context(context, 101, {});
    CHECK(result.has_value());
    CHECK_EQ(result->size(), std::size_t(1));
    CHECK(std::holds_alternative<UserMessage>((*result)[0]));
  });

  tests::register_test("LuaHooks: after_tool_call overrides content", [&]() {
    auto p = write_hooks("after_override.lua", R"lua(
return {
  after_tool_call = function(ctx)
    return {content = "OVERRIDDEN: " .. ctx.content, is_error = false}
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    CHECK(hooks->after_tool_call != nullptr);

    ToolCall tc;
    tc.id = "id2"; tc.name = "read";
    tc.arguments = nlohmann::json::object();

    struct FakeResult : public ToolResult {
      bool is_error() const override { return false; }
      std::string content() const override { return "original"; }
      std::optional<std::string> details() const override { return std::nullopt; }
    };

    AgentContext ctx;
    AssistantMessage am;
    AfterToolCallContext actx{am, tc, "{}", std::make_shared<FakeResult>(), false, ctx};

    auto result = hooks->after_tool_call(actx, std::stop_token{});
    CHECK(result.has_value());
    CHECK(result->content.has_value());
    const auto &blocks = *result->content;
    CHECK(!blocks.empty());
    const auto *tc2 = std::get_if<TextContent>(&blocks[0]);
    CHECK(tc2 != nullptr);
    CHECK(tc2->text == "OVERRIDDEN: original");
  });

  tests::register_test("LuaHooks: should_stop_after_turn", [&]() {
    auto p = write_hooks("stop.lua", R"lua(
return {
  should_stop_after_turn = function(ctx)
    return ctx.message:find("DONE") ~= nil
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    CHECK(hooks->should_stop_after_turn != nullptr);

    AgentContext ctx;

    AssistantMessage am_done;
    am_done.content.push_back(TextContent{"task is DONE"});
    CHECK(hooks->should_stop_after_turn(am_done, {}, ctx));

    AssistantMessage am_cont;
    am_cont.content.push_back(TextContent{"still working"});
    CHECK(!hooks->should_stop_after_turn(am_cont, {}, ctx));
  });

  tests::register_test("LuaHooks: on_command intercepts slash command", [&]() {
    auto p = write_hooks("cmd_rewind.lua", R"lua(
return {
  on_command = function(cmd, args, transcript)
    if cmd == "rewind" then
      local n = tonumber(args) or 1
      local turns = 0
      local idx = 0
      for i, msg in ipairs(transcript) do
        if msg.role == "assistant" then
          turns = turns + 1
          idx = i
          if turns >= n then break end
        end
      end
      return {handled=true, truncate_to=idx}
    end
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    CHECK(hooks->on_command != nullptr);
    CHECK(!hooks->before_tool_call);

    // Build a transcript: user → assistant(turn1) → tool_result → user → assistant(turn2)
    std::vector<Message> transcript;
    UserMessage u1; u1.content.push_back(TextContent{"hello"});
    transcript.push_back(u1);
    AssistantMessage a1; a1.content.push_back(TextContent{"hi there"});
    transcript.push_back(a1);
    ToolResultMessage tr; tr.tool_name = "bash"; tr.content.push_back(TextContent{"ok"});
    transcript.push_back(tr);
    UserMessage u2; u2.content.push_back(TextContent{"do more"});
    transcript.push_back(u2);
    AssistantMessage a2; a2.content.push_back(TextContent{"doing it"});
    transcript.push_back(a2);

    // /rewind 1 — keep through first assistant turn (index 2)
    auto r = hooks->on_command("rewind", "1", transcript, empty_context());
    CHECK(r.handled);
    CHECK(r.truncate_to.has_value());
    CHECK(*r.truncate_to == std::size_t(2));
    CHECK(!r.prompt.has_value());
  });

  tests::register_test("LuaHooks: on_command falls through when not handled", [&]() {
    auto p = write_hooks("cmd_passthrough.lua", R"lua(
return {
  on_command = function(cmd, args, transcript)
    -- only handle /rewind
    if cmd ~= "rewind" then return {handled=false} end
    return {handled=true}
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    auto r = hooks->on_command("help", "", {}, empty_context());
    CHECK(!r.handled);
  });

  tests::register_test("LuaHooks: on_command exposes complete context views", [&]() {
    auto p = write_hooks("context.lua", R"lua(
return {
  on_command = function(cmd, args, transcript, context)
    if cmd ~= "inspect" then return {handled=false} end
    local raw = context.raw
    local user = raw.messages[1]
    local assistant = raw.messages[2]
    local tool_result = raw.messages[3]
    local call = assistant.content[3]
    local valid_tool = raw.tools[1]
    local invalid_tool = raw.tools[2]

    -- Lua receives copies, so these assignments must not mutate native state.
    raw.system_prompt = "mutated"
    user.content[1].text = "mutated"

    local fields = {
      raw.system_prompt,
      user.role,
      user.content[1].text,
      user.content[2].mimeType,
      assistant.turn,
      assistant.content[1].thinkingSignature,
      tostring(assistant.content[1].redacted),
      assistant.content[2].textSignature,
      call.type .. ":" .. call.id .. ":" .. call.name,
      call.arguments.query,
      assistant.responseId,
      assistant.stopReason,
      tostring(assistant.usage.input),
      tool_result.toolCallId .. ":" .. tool_result.toolName,
      tool_result.details,
      tostring(tool_result.isError),
      valid_tool.input_schema.properties.query.type,
      tostring(valid_tool.source_path ~= ""),
      tostring(invalid_tool.schema_valid),
      raw.model.id .. ":" .. raw.model.provider .. ":" .. raw.model.baseUrl,
      tostring(raw.model.cost.inputPerMtok),
      tostring(context.effective.available),
      context.effective.provider or "-",
      context.effective.api or "-",
      context.effective.system_prompt or "-",
    }
    return {handled=true, output=table.concat(fields, "|")}
  end
}
)lua");
    auto hooks = load_lua_hooks(p);

    LuaContextSnapshot context;
    context.raw.system_prompt = "system prompt";
    context.raw.model.id = "model-1";
    context.raw.model.provider = "provider-1";
    context.raw.model.api = "api-1";
    context.raw.model.base_url = "https://example.test";
    context.raw.model.cost.input_per_mtok = 1.25;

    UserMessage user;
    user.content.emplace_back(TextContent{.text = "hello"});
    user.content.emplace_back(
        ImageContent{.data = "aGVsbG8=", .mime_type = "image/png"});
    context.raw.messages.emplace_back(user);

    AssistantMessage assistant;
    assistant.api = "api-1";
    assistant.provider = "provider-1";
    assistant.model = "model-1";
    assistant.response_id = "response-1";
    assistant.stop_reason = StopReason::tool_use;
    assistant.usage.input = 42;
    assistant.content.emplace_back(ThinkingContent{
        .thinking = "hidden", .thinking_signature = "think-sig", .redacted = true});
    assistant.content.emplace_back(
        TextContent{.text = "answer", .text_signature = "text-sig"});
    ToolCall call;
    call.id = "call-1";
    call.name = "search";
    call.arguments = {{"query", "pici"}};
    assistant.content.emplace_back(std::move(call));
    context.raw.messages.emplace_back(std::move(assistant));

    ToolResultMessage result;
    result.tool_call_id = "call-1";
    result.tool_name = "search";
    result.details = "details";
    result.is_error = true;
    result.content.emplace_back(TextContent{.text = "no result"});
    context.raw.messages.emplace_back(std::move(result));

    context.raw.tools.emplace_back(std::make_shared<ContextTool>(R"json(
{"type":"object","properties":{"query":{"type":"string"}}}
)json"));
    context.raw.tools.emplace_back(std::make_shared<ContextTool>("not json"));

    auto unavailable = hooks->on_command("inspect", "", context.raw.messages,
                                         context);
    CHECK(unavailable.handled);
    CHECK(unavailable.output.has_value());
    CHECK(*unavailable.output ==
          "mutated|user|mutated|image/png|1|think-sig|true|text-sig|toolCall:call-1:search|pici|response-1|toolUse|42|call-1:search|details|true|string|true|false|model-1:provider-1:https://example.test|1.25|false|-|-|-");
    CHECK(context.raw.system_prompt == "system prompt");
    CHECK(std::get<UserMessage>(context.raw.messages[0]).content.size() ==
          std::size_t(2));

    context.effective = context.raw;
    context.effective->system_prompt = "effective system";
    auto available = hooks->on_command("inspect", "", context.raw.messages,
                                        context);
    CHECK(available.handled);
    CHECK(available.output.has_value());
    CHECK(available.output->ends_with("|true|provider-1|api-1|effective system"));
  });

  tests::register_test("LuaHooks: ctx.turn in before_tool_call", [&]() {
    auto p = write_hooks("turn_capture.lua", R"lua(
local captured_turn = -1
return {
  before_tool_call = function(ctx)
    captured_turn = ctx.turn
    _G.last_turn = ctx.turn
  end
}
)lua");
    // Just verify it loads without error — turn value is tested through
    // the call_before path which requires a full AgentContext with messages
    auto hooks = load_lua_hooks(p);
    CHECK(hooks->before_tool_call != nullptr);
  });

  tests::register_test("LuaHooks: syntax error throws", [&]() {
    auto p = write_hooks("bad_hooks.lua", "not valid lua !!!");
    bool threw = false;
    try { load_lua_hooks(p); } catch (const std::exception &) { threw = true; }
    CHECK(threw);
  });

  tests::register_test("LuaHooks: empty hooks file loads ok", [&]() {
    auto p = write_hooks("empty_hooks.lua", "return {}");
    auto hooks = load_lua_hooks(p);
    CHECK(!hooks->before_tool_call);
    CHECK(!hooks->after_tool_call);
    CHECK(!hooks->should_stop_after_turn);
  });

  tests::register_test("compose_hooks: before_tool_call short-circuits on block", [&]() {
    auto p1 = write_hooks("comp_allow.lua", R"lua(
return { before_tool_call = function(ctx) return nil end }
)lua");
    auto p2 = write_hooks("comp_block.lua", R"lua(
return { before_tool_call = function(ctx)
  if ctx.tool_name == "bash" then return {block=true, reason="no bash"} end
end }
)lua");
    auto p3 = write_hooks("comp_never.lua", R"lua(
-- this should never fire for bash due to short-circuit
return { before_tool_call = function(ctx) return {block=true, reason="wrong"} end }
)lua");

    auto composed = compose_hooks({load_lua_hooks(p1), load_lua_hooks(p2), load_lua_hooks(p3)});
    CHECK(composed != nullptr);
    CHECK(composed->before_tool_call != nullptr);

    ToolCall tc; tc.id = "x"; tc.name = "bash"; tc.arguments = nlohmann::json::object();
    AgentContext ctx;
    AssistantMessage am;
    BeforeToolCallContext bctx{am, tc, "{}", ctx};

    auto r = composed->before_tool_call(bctx, std::stop_token{});
    CHECK(r.has_value());
    CHECK(r->block);
    CHECK(r->reason == "no bash");  // p2 fires, p3 never reached
  });

  tests::register_test("compose_hooks: on_command first-handled wins", [&]() {
    auto p1 = write_hooks("cmd_a.lua", R"lua(
return { on_command = function(cmd, args, t, context)
  if cmd == "foo" and context.raw.system_prompt == "shared" then
    return {handled=true, prompt="foo handled"}
  end
end }
)lua");
    auto p2 = write_hooks("cmd_b.lua", R"lua(
return { on_command = function(cmd, args, t, context)
  if cmd == "bar" and context.raw.system_prompt == "shared" then
    return {handled=true, prompt="bar handled"}
  end
end }
)lua");

    auto composed = compose_hooks({load_lua_hooks(p1), load_lua_hooks(p2)});
    LuaContextSnapshot shared;
    shared.raw.system_prompt = "shared";
    auto r1 = composed->on_command("foo", "", {}, shared);
    CHECK(r1.handled);
    CHECK(r1.prompt == "foo handled");

    auto r2 = composed->on_command("bar", "", {}, shared);
    CHECK(r2.handled);
    CHECK(r2.prompt == "bar handled");

    auto r3 = composed->on_command("unknown", "", {}, shared);
    CHECK(!r3.handled);
  });

  tests::register_test("compose_hooks: should_stop_after_turn is OR", [&]() {
    auto p1 = write_hooks("stop_never.lua", R"lua(
return { should_stop_after_turn = function(ctx) return false end }
)lua");
    auto p2 = write_hooks("stop_on_done.lua", R"lua(
return { should_stop_after_turn = function(ctx) return ctx.message:find("DONE") ~= nil end }
)lua");
    auto composed = compose_hooks({load_lua_hooks(p1), load_lua_hooks(p2)});

    AgentContext ctx;
    AssistantMessage yes; yes.content.push_back(TextContent{"task DONE"});
    CHECK(composed->should_stop_after_turn(yes, {}, ctx));

    AssistantMessage no_; no_.content.push_back(TextContent{"still going"});
    CHECK(!composed->should_stop_after_turn(no_, {}, ctx));
  });

  tests::register_test("compose_hooks: configure forwards to all", [&]() {
    auto p1 = write_hooks("ra1.lua", R"lua(
return { on_command = function(cmd, args, t)
  if cmd ~= "sub1" then return {handled=false} end
  local r = pici.run_agent({prompt="sub1"})
  return {handled=true, prompt=r.text}
end }
)lua");
    auto p2 = write_hooks("ra2.lua", R"lua(
return { on_command = function(cmd, args, t)
  if cmd ~= "sub2" then return {handled=false} end
  local r = pici.run_agent({prompt="sub2"})
  return {handled=true, prompt=r.text}
end }
)lua");

    auto composed = compose_hooks({load_lua_hooks(p1), load_lua_hooks(p2)});
    std::string last_prompt;
    LuaHooks::AgentInfo info;
    info.run_agent = [&](const LuaHooks::AgentRunConfig &cfg) -> LuaHooks::AgentRunResult {
      last_prompt = cfg.prompt;
      return {.text = "ok:" + cfg.prompt};
    };
    composed->configure(info);

    auto r1 = composed->on_command("sub1", "", {}, empty_context());
    CHECK(r1.handled);
    CHECK(r1.prompt == "ok:sub1");

    auto r2 = composed->on_command("sub2", "", {}, empty_context());
    CHECK(r2.handled);
    CHECK(r2.prompt == "ok:sub2");
  });

  tests::register_test("compose_hooks: null and single passthrough", [&]() {
    CHECK(compose_hooks({}) == nullptr);
    CHECK(compose_hooks({nullptr, nullptr}) == nullptr);
    auto h = load_lua_hooks(write_hooks("single.lua", "return {}"));
    auto composed = compose_hooks({nullptr, h, nullptr});
    CHECK(composed == h);  // same pointer, no wrapping
  });

  tests::register_test("LuaHooks: mailbox primitives convert JSON", [&]() {
    auto p = write_hooks("mailbox_bindings.lua", R"lua(
return {
  on_command = function(cmd)
    if cmd ~= "mailbox" then return {handled=false} end
    local self, self_err = pici.mailbox.self()
    local sent, send_err = pici.mailbox.send({
      target = {session_id = "session-b"}, text = "hello", kind = "note"
    })
    if self_err or send_err then
      return {handled=true, output=self_err or send_err}
    end
    return {handled=true, prompt=self.agent_id .. ":" .. sent.state}
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    LuaHooks::AgentInfo info;
    info.mailbox.self =
        [](const nlohmann::json &, const std::optional<AgentRuntimeIdentity> &,
           std::stop_token) { return nlohmann::json{{"agent_id", "agent-a"}}; };
    info.mailbox.send = [](const nlohmann::json &value,
                           const std::optional<AgentRuntimeIdentity> &,
                           std::stop_token) {
      CHECK(value.at("text") == "hello");
      return nlohmann::json{{"state", "queued"}};
    };
    hooks->configure(info);
    const auto result = hooks->on_command("mailbox", "", {}, {});
    CHECK(result.handled);
    CHECK_EQ(result.prompt.value(), "agent-a:queued");
  });

  tests::register_test("LuaHooks: mailbox unavailable is nil,error", [&]() {
    auto p = write_hooks("mailbox_unavailable.lua", R"lua(
return {
  on_command = function(cmd)
    if cmd ~= "mailbox" then return {handled=false} end
    local value, err = pici.mailbox.self()
    return {handled=true, output=tostring(value) .. ":" .. err}
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    LuaHooks::AgentInfo info;
    hooks->configure(info);
    const auto result = hooks->on_command("mailbox", "", {}, {});
    CHECK(result.handled);
    CHECK_EQ(result.output.value(), "nil:pici.mailbox is not available");
  });

  tests::register_test("LuaHooks: mailbox errors retain stable code", [&]() {
    auto p = write_hooks("mailbox_error.lua", R"lua(
return {
  on_command = function(cmd)
    if cmd ~= "mailbox" then return {handled=false} end
    local value, err = pici.mailbox.status()
    return {handled=true, output=tostring(value) .. ":" .. err}
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    LuaHooks::AgentInfo info;
    info.mailbox.status = [](const nlohmann::json &,
                             const std::optional<AgentRuntimeIdentity> &,
                             std::stop_token) {
      return nlohmann::json{
          {"error", {{"code", "invalid_message"}, {"message", "bad input"}}}};
    };
    hooks->configure(info);
    const auto result = hooks->on_command("mailbox", "", {}, {});
    CHECK(result.handled);
    CHECK_EQ(result.output.value(), "nil:invalid_message: bad input");
  });

  tests::register_test("pici.add_tool registers inline tool", [&]() {
    auto p = write_hooks("inline_tool.lua", R"lua(
pici.add_tool({
  name        = "echo_test",
  description = "Echoes the input back",
  schema      = '{"type":"object","properties":{"text":{"type":"string"}},"required":["text"]}',
  execute     = function(args)
    return "ECHO: " .. (args.text or "")
  end,
})
return {}
)lua");
    auto hooks = load_lua_hooks(p);
    CHECK_EQ(hooks->registered_tools.size(), std::size_t(1));
    const auto &tool = hooks->registered_tools[0];
    CHECK(tool->name() == "echo_test");
    CHECK(tool->description() == "Echoes the input back");
    CHECK(!tool->source_path().empty()); // should be the hooks file path

    // Execute the tool
    auto result = tool->execute("id", R"({"text":"hello"})", {}, {});
    CHECK(!result->is_error());
    CHECK(result->content() == "ECHO: hello");
  });

  tests::register_test("pici.add_tool reports progress updates", [&]() {
    auto p = write_hooks("inline_progress.lua", R"lua(
pici.add_tool({
  name = "progress_test",
  description = "Reports progress",
  execute = function(_, ctx)
    ctx.update("working")
    return "done"
  end,
})
return {}
)lua");
    auto hooks = load_lua_hooks(p);
    int updates = 0;
    auto result = hooks->registered_tools[0]->execute(
        "id", "{}", {}, [&](const std::shared_ptr<ToolResult> &partial) {
          ++updates;
          CHECK_EQ(partial->content(), "working");
        });
    CHECK(!result->is_error());
    CHECK_EQ(updates, 1);
  });

  tests::register_test("compose_hooks: registered_tools are unioned", [&]() {
    auto p1 = write_hooks("rt1.lua", R"lua(
pici.add_tool({name="tool_a", description="A", execute=function(a) return "a" end})
return {}
)lua");
    auto p2 = write_hooks("rt2.lua", R"lua(
pici.add_tool({name="tool_b", description="B", execute=function(a) return "b" end})
pici.add_tool({name="tool_c", description="C", execute=function(a) return "c" end})
return {}
)lua");
    auto composed = compose_hooks({load_lua_hooks(p1), load_lua_hooks(p2)});
    CHECK_EQ(composed->registered_tools.size(), std::size_t(3));
  });

  tests::register_test("LuaHooks: prompt_line returns custom prompt", [&]() {
    auto p = write_hooks("prompt.lua", R"lua(
return {
  prompt_line = function(ctx)
    return "[turn " .. ctx.turn .. "/" .. ctx.model .. "] > "
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    CHECK(hooks->prompt_line != nullptr);
    TokenUsage empty{};
    auto r = hooks->prompt_line(3, "gpt-4o", 7, empty, empty);
    CHECK(r.has_value());
    CHECK(*r == "[turn 3/gpt-4o] > ");
  });

  tests::register_test("LuaHooks: prompt_line nil returns nullopt", [&]() {
    auto p = write_hooks("prompt_nil.lua", R"lua(
return { prompt_line = function(ctx) return nil end }
)lua");
    auto hooks = load_lua_hooks(p);
    TokenUsage empty{};
    auto r = hooks->prompt_line(0, "model", 0, empty, empty);
    CHECK(!r.has_value());
  });

  tests::register_test("compose_hooks: prompt_line last non-nil wins", [&]() {
    auto p1 = write_hooks("pl1.lua", R"lua(
return { prompt_line = function(ctx) return "first> " end }
)lua");
    auto p2 = write_hooks("pl2.lua", R"lua(
return { prompt_line = function(ctx) return "second> " end }
)lua");
    auto composed = compose_hooks({load_lua_hooks(p1), load_lua_hooks(p2)});
    CHECK(composed->prompt_line != nullptr);
    TokenUsage empty{};
    auto r = composed->prompt_line(0, "m", 0, empty, empty);
    CHECK(r.has_value());
    CHECK(*r == "second> ");
  });

  tests::register_test("LuaHooks: prompt_line exposes last cost", [&]() {
    auto p = write_hooks("prompt_cost.lua", R"lua(
return {
  prompt_line = function(ctx)
    return string.format("cost=%.4f in=%d", ctx.last.cost.total, ctx.last.input)
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    CHECK(hooks->prompt_line != nullptr);
    TokenUsage u;
    u.input = 123;
    u.cost.total = 0.0042;
    TokenUsage sess{};
    auto r = hooks->prompt_line(1, "gpt-4o", 7, u, sess);
    CHECK(r.has_value());
    CHECK(*r == "cost=0.0042 in=123");
  });

  tests::register_test("LuaHooks: UI hooks expose session context", [&]() {
    auto p = write_hooks("ui.lua", R"lua(
return {
  status_line = function(ctx)
    return ctx.model .. ":" .. ctx.session_id .. ":" .. (ctx.session_name or "none")
  end,
  tab_title = function(ctx)
    return "pici " .. ctx.turn
  end,
}
)lua");
    auto hooks = load_lua_hooks(p);
    CHECK(hooks->status_line != nullptr);
    CHECK(hooks->tab_title != nullptr);
    LuaUiContext context;
    context.turn = 3;
    context.model = "gpt-4o";
    context.tools = 2;
    context.session_id = "session-1";
    context.session_name = "demo";
    CHECK_EQ(*hooks->status_line(context), "gpt-4o:session-1:demo");
    CHECK_EQ(*hooks->tab_title(context), "pici 3");
  });

  tests::register_test("compose_hooks: UI hooks last non-nil wins", [&]() {
    auto p1 = write_hooks("ui1.lua", R"lua(
return {
  status_line = function(ctx) return "first" end,
  tab_title = function(ctx) return nil end,
}
)lua");
    auto p2 = write_hooks("ui2.lua", R"lua(
return {
  status_line = function(ctx) return "second" end,
  tab_title = function(ctx) return "title" end,
}
)lua");
    auto composed = compose_hooks({load_lua_hooks(p1), load_lua_hooks(p2)});
    LuaUiContext context;
    CHECK_EQ(*composed->status_line(context), "second");
    CHECK_EQ(*composed->tab_title(context), "title");
  });

  tests::register_test("LuaHooks: pici.run_agent calls C++ factory", [&]() {
    auto p = write_hooks("subagent.lua", R"lua(
return {
  on_command = function(cmd, args, transcript)
    if cmd ~= "sub" then return {handled=false} end
    local result = pici.run_agent({
      prompt = "hello from sub",
      fork_at = #transcript,
    })
    if result.error then
      return {handled=true, prompt="subagent error: " .. result.error}
    end
    -- inject the subagent response as a steering prompt
    return {handled=true, prompt="subagent said: " .. result.text}
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    CHECK(hooks->configure != nullptr);

    bool factory_called = false;
    std::string captured_prompt;
    std::size_t captured_fork_at = 999;

    LuaHooks::AgentInfo info;
    info.run_agent = [&](const LuaHooks::AgentRunConfig &cfg) -> LuaHooks::AgentRunResult {
      factory_called = true;
      captured_prompt = cfg.prompt;
      captured_fork_at = cfg.fork_at;
      return {.text = "mocked response", .error = std::nullopt};
    };
    hooks->configure(info);

    // Build transcript with 2 messages
    std::vector<Message> transcript;
    UserMessage u; u.content.push_back(TextContent{"hi"});
    transcript.push_back(u);
    AssistantMessage a; a.content.push_back(TextContent{"hello"});
    transcript.push_back(a);

    auto r = hooks->on_command("sub", "", transcript, empty_context());
    CHECK(r.handled);
    CHECK(factory_called);
    CHECK(captured_prompt == "hello from sub");
    CHECK(captured_fork_at == std::size_t(2));
    CHECK(r.prompt.has_value());
    CHECK(*r.prompt == "subagent said: mocked response");
  });

  tests::register_test("pici.model / pici.tools / pici.cwd after configure", [&]() {
    auto p = write_hooks("info.lua", R"lua(
return {
  on_command = function(cmd, args, t)
    if cmd == "info" then
      return {handled=true, prompt=
        pici.model.id .. "|" .. pici.model.provider ..
        "|" .. tostring(#pici.tools) ..
        "|" .. pici.cwd}
    end
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    LuaHooks::AgentInfo info;
    info.model_id       = "gpt-4o";
    info.model_provider = "openai";
    info.model_api      = "openai-completions";
    info.tool_names     = {"read", "bash", "edit"};
    info.cwd            = "/tmp/test";
    hooks->configure(info);

    auto r = hooks->on_command("info", "", {}, empty_context());
    CHECK(r.handled);
    CHECK(r.prompt.has_value());
    CHECK(*r.prompt == "gpt-4o|openai|3|/tmp/test");
  });

  tests::register_test("pici.storage persists across calls", [&]() {
    auto storage_file = dir / "persist.lua.storage.json";
    std::filesystem::remove(storage_file);

    auto p = write_hooks("persist.lua", R"lua(
return {
  on_command = function(cmd, args, t)
    if cmd == "store" then
      pici.storage.set("key", args)
      return {handled=true}
    elseif cmd == "load" then
      local v = pici.storage.get("key")
      return {handled=true, prompt=tostring(v)}
    end
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    LuaHooks::AgentInfo info;
    info.storage_path = storage_file;
    hooks->configure(info);

    hooks->on_command("store", "hello", {}, empty_context());
    auto r = hooks->on_command("load", "", {}, empty_context());
    CHECK(r.handled);
    CHECK(r.prompt == "hello");

    // Verify it actually wrote to disk
    CHECK(std::filesystem::exists(storage_file));
    std::filesystem::remove(storage_file);
  });

  tests::register_test("LuaHooks: commands declared in table", [&]() {
    auto p = write_hooks("with_cmds.lua", R"lua(
return {
  commands = {
    {name="rewind", description="Rewind to a turn", args_hint="<turn>"},
    {name="fork",   description="Fork the session"},
  },
  on_command = function(cmd, args, t) return {handled=false} end,
}
)lua");
    auto hooks = load_lua_hooks(p);
    CHECK_EQ(hooks->commands.size(), std::size_t(2));
    CHECK(hooks->commands[0].name == "rewind");
    CHECK(hooks->commands[0].description == "Rewind to a turn");
    CHECK(hooks->commands[0].args_hint == "<turn>");
    CHECK(hooks->commands[1].name == "fork");
  });

  tests::register_test("compose_hooks: commands are unioned", [&]() {
    auto p1 = write_hooks("cmds_a.lua", R"lua(
return { commands = {{name="rewind"}, {name="fork"}} }
)lua");
    auto p2 = write_hooks("cmds_b.lua", R"lua(
return { commands = {{name="search"}} }
)lua");
    auto composed = compose_hooks({load_lua_hooks(p1), load_lua_hooks(p2)});
    CHECK_EQ(composed->commands.size(), std::size_t(3));
  });

  tests::register_test("LuaHooks: complete returns candidates", [&]() {
    auto p = write_hooks("completer.lua", R"lua(
return {
  complete = function(partial, transcript)
    if partial:sub(1,1) ~= "/" then return {} end
    local cmd = partial:sub(2)
    local cmds = {"/rewind", "/fork", "/sub"}
    local result = {}
    for _, c in ipairs(cmds) do
      if c:sub(1, #partial) == partial then
        table.insert(result, c)
      end
    end
    return result
  end
}
)lua");
    auto hooks = load_lua_hooks(p);
    CHECK(hooks->complete != nullptr);

    auto r1 = hooks->complete("/r", {});
    CHECK_EQ(r1.size(), std::size_t(1));
    CHECK(r1[0] == "/rewind");

    auto r2 = hooks->complete("/", {});
    CHECK_EQ(r2.size(), std::size_t(3));

    auto r3 = hooks->complete("hello", {});
    CHECK(r3.empty());
  });

  tests::register_test("compose_hooks: complete unions all results", [&]() {
    auto p1 = write_hooks("comp1.lua", R"lua(
return { complete = function(partial, t) return {"/rewind", "/fork"} end }
)lua");
    auto p2 = write_hooks("comp2.lua", R"lua(
return { complete = function(partial, t) return {"/search"} end }
)lua");
    auto composed = compose_hooks({load_lua_hooks(p1), load_lua_hooks(p2)});
    CHECK(composed->complete != nullptr);
    auto r = composed->complete("/", {});
    CHECK_EQ(r.size(), std::size_t(3));
  });

  tests::register_test("run_lua_test_file: pass and fail counts", [&]() {
    auto p = write_hooks("suite.lua", R"lua(
pici.test.run("passes",  function() pici.test.eq(1, 1) end)
pici.test.run("also ok", function() pici.test.ok(true) end)
pici.test.run("fails",   function() pici.test.fail("oops") end)
)lua");
    auto r = run_lua_test_file(p);
    CHECK_EQ(r.passed, 2);
    CHECK_EQ(r.failed, 1);
    CHECK_EQ(r.total,  3);
  });

  tests::register_test("run_lua_test_file: mock_run_agent works", [&]() {
    auto p = write_hooks("mock_test.lua", R"lua(
pici.mock_run_agent(function(cfg) return {text="hi", error=nil} end)
pici.test.run("mock works", function()
  local r = pici.run_agent({prompt="hello"})
  pici.test.eq(r.text, "hi")
end)
)lua");
    auto r = run_lua_test_file(p);
    CHECK_EQ(r.passed, 1);
    CHECK_EQ(r.failed, 0);
  });

  tests::register_test("run_lua_test_file: syntax error throws", [&]() {
    auto p = write_hooks("bad_test.lua", "not valid lua !!!");
    bool threw = false;
    try { run_lua_test_file(p); } catch (const std::exception &) { threw = true; }
    CHECK(threw);
  });

  std::filesystem::remove_all(dir);
}

void test_tool_formatter_hooks_only() {
  tests::register_test(
      "tool_formatter_hooks_only: strips non-formatting capabilities", []() {
        auto hooks = std::make_shared<LuaHooks>();
        hooks->source_path = "theme.lua";
        hooks->registered_tools.emplace_back(nullptr);
        hooks->commands.push_back({.name = "unsafe"});
        hooks->before_tool_call = [](const BeforeToolCallContext &,
                                     std::stop_token) {
          return std::optional<BeforeToolCallResult>{};
        };
        hooks->after_tool_call = [](const AfterToolCallContext &,
                                    std::stop_token) {
          return std::optional<AfterToolCallResult>{};
        };
        hooks->on_event = [](const AgentEvent &) {};
        hooks->prepare_context = [](const AgentContext &, std::size_t,
                                    std::stop_token) {
          return std::optional<std::vector<Message>>{};
        };
        hooks->should_stop_after_turn =
            [](const Message &, const std::vector<ToolResultMessage> &,
               const AgentContext &) { return true; };
        hooks->configure = [](const LuaHooks::AgentInfo &) {};
        hooks->prompt_line = [](std::size_t, std::string_view, std::size_t,
                                const TokenUsage &, const TokenUsage &) {
          return std::optional<std::string>{"unsafe"};
        };
        hooks->status_line = [](const LuaUiContext &) {
          return std::optional<std::string>{"unsafe"};
        };
        hooks->tab_title = [](const LuaUiContext &) {
          return std::optional<std::string>{"unsafe"};
        };
        hooks->complete = [](std::string_view, const std::vector<Message> &) {
          return std::vector<std::string>{"unsafe"};
        };
        hooks->on_command = [](std::string_view, std::string_view,
                               const std::vector<Message> &,
                               const LuaContextSnapshot &) {
          return LuaHooks::CommandResult{.handled = true};
        };
        hooks->format_tool_call = [](const LuaHooks::FormatToolCallContext &) {
          return std::optional<std::string>{"formatted call"};
        };
        hooks->format_tool_result =
            [](const LuaHooks::FormatToolResultContext &) {
              return std::optional<std::string>{"formatted result"};
            };

        auto filtered = tool_formatter_hooks_only(hooks);
        CHECK(filtered != nullptr);
        CHECK_EQ(filtered->source_path, std::string("theme.lua"));
        CHECK(filtered->registered_tools.empty());
        CHECK(filtered->commands.empty());
        CHECK(!filtered->before_tool_call);
        CHECK(!filtered->after_tool_call);
        CHECK(!filtered->on_event);
        CHECK(!filtered->prepare_context);
        CHECK(!filtered->should_stop_after_turn);
        CHECK(!filtered->configure);
        CHECK(!filtered->prompt_line);
        CHECK(!filtered->status_line);
        CHECK(!filtered->tab_title);
        CHECK(!filtered->complete);
        CHECK(!filtered->on_command);
        CHECK(filtered->format_tool_call);
        CHECK(filtered->format_tool_result);
        CHECK_EQ(*filtered->format_tool_call({}),
                 std::string("formatted call"));
        CHECK_EQ(*filtered->format_tool_result({}),
                 std::string("formatted result"));
      });
}

void test_mailbox_addon() {
  tests::register_test("mailbox addon registers intention tools", []() {
    const auto path =
        std::filesystem::path(PI_CPP_SOURCE_DIR) / "addons" / "mailbox.lua";
    auto hooks = load_lua_hooks(path, true);
    CHECK_EQ(hooks->registered_tools.size(), std::size_t(7));
    const std::vector<std::string> expected = {
        "agents_self",  "agents_list",  "agents_send", "agents_request",
        "agents_reply", "agents_inbox", "agents_close"};
    for (std::size_t i = 0; i < expected.size(); ++i) {
      CHECK_EQ(std::string(hooks->registered_tools[i]->name()), expected[i]);
      const auto schema = nlohmann::json::parse(
          hooks->registered_tools[i]->schema().serialize());
      CHECK(schema.is_object());
      CHECK(schema.value("additionalProperties", true) == false);
      const bool curated = hooks->registered_tools[i]->name() != "agents_close";
      CHECK_EQ(hooks->registered_tools[i]->capabilities().child_safe, curated);
    }
  });

  tests::register_test("mailbox addon validates and executes intentions", [&]() {
    const auto path =
        std::filesystem::path(PI_CPP_SOURCE_DIR) / "addons" / "mailbox.lua";
    auto hooks = load_lua_hooks(path);
    std::vector<nlohmann::json> acked;
    std::mutex actor_mutex;
    std::vector<std::string> actors;
    LuaHooks::AgentInfo info;
    info.mailbox.self = [&](const nlohmann::json &,
                            const std::optional<AgentRuntimeIdentity> &actor,
                            std::stop_token) {
      if (actor) {
        std::scoped_lock lock(actor_mutex);
        actors.push_back(actor->agent_id);
        CHECK(actor->agent_id == "root-a" || actor->agent_id == "child-a");
      }
      return nlohmann::json{{"agent_id", actor ? actor->agent_id : "root-a"},
                            {"process_id", "process-a"},
                            {"kind", actor ? actor->kind : "root"}};
    };
    info.mailbox.list = [](const nlohmann::json &,
                           const std::optional<AgentRuntimeIdentity> &,
                           std::stop_token) {
      return nlohmann::json::array({{{"agent_id", "root-a"},
                                     {"process_id", "process-a"},
                                     {"kind", "root"}},
                                    {{"agent_id", "child-a"},
                                     {"process_id", "process-a"},
                                     {"kind", "subagent"},
                                     {"task_id", "child-task"},
                                     {"owner_agent_id", "root-a"}},
                                    {{"agent_id", "remote-a"},
                                     {"process_id", "process-b"},
                                     {"kind", "subagent"},
                                     {"owner_agent_id", "root-b"}}});
    };
    info.mailbox.send = [](const nlohmann::json &value,
                           const std::optional<AgentRuntimeIdentity> &,
                           std::stop_token) {
      CHECK(value.at("target").at("session_id") == "session-b");
      return nlohmann::json{{"state", "queued"}, {"message_id", "m1"}};
    };
    info.mailbox.request = [](const nlohmann::json &,
                              const std::optional<AgentRuntimeIdentity> &,
                              std::stop_token) {
      return nlohmann::json{{"state", "pending"}, {"request_id", "r1"}};
    };
    info.mailbox.reply = [](const nlohmann::json &value,
                            const std::optional<AgentRuntimeIdentity> &,
                            std::stop_token) {
      CHECK(value.at("message_id") == "m2");
      return nlohmann::json{{"state", "queued"}, {"message_id", "m3"}};
    };
    info.mailbox.inbox = [](const nlohmann::json &value,
                            const std::optional<AgentRuntimeIdentity> &,
                            std::stop_token) {
      CHECK(value.at("claim") == true);
      return nlohmann::json::array({{{"message_id", "m2"},
                                     {"sender_agent_id", "sender-a"},
                                     {"sender_session_id", "session-s"},
                                     {"recipient_session_id", "session-a"},
                                     {"kind", "request"},
                                     {"text", "question"},
                                     {"created_at_ms", 42},
                                     {"claim_token", "secret"}},
                                    {{"message_id", "m4"},
                                     {"sender_agent_id", "sender-b"},
                                     {"sender_session_id", "session-t"},
                                     {"recipient_session_id", "session-a"},
                                     {"kind", "note"},
                                     {"text", "late"},
                                     {"created_at_ms", 43},
                                     {"claim_token", "secret-2"}}});
    };
    info.mailbox.ack = [&acked](const nlohmann::json &value,
                                const std::optional<AgentRuntimeIdentity> &,
                                std::stop_token) {
      acked.push_back(value);
      if (acked.size() > 1)
        return nlohmann::json{
            {"error", {{"code", "busy"}, {"message", "lease busy"}}}};
      return nlohmann::json{{"state", "acknowledged"}};
    };
    info.agents.close = [](const nlohmann::json &value) {
      CHECK(value.at("target") == "child-task");
      return nlohmann::json{{"closed", "child-task"}};
    };
    hooks->configure(info);

    auto find = [&](std::string_view name) {
      return *std::ranges::find_if(
          hooks->registered_tools,
          [&](const auto &tool) { return tool->name() == name; });
    };
    const auto actor = AgentRuntimeIdentity{.agent_id = "child-a",
                                            .session_id = "session-a",
                                            .kind = "subagent",
                                            .task_id = "child-task"};
    auto self = find("agents_self")
                    ->execute("{}", ToolExecutionContext{.call_id = "self",
                                                         .actor = actor});
    CHECK(!self->is_error());
    CHECK(nlohmann::json::parse(self->content()).at("agent_id") == "child-a");
    auto invoke_self = [&](std::string agent_id) {
      return find("agents_self")
          ->execute("{}",
                    ToolExecutionContext{
                        .call_id = agent_id,
                        .actor = AgentRuntimeIdentity{
                            .agent_id = agent_id,
                            .session_id = "session-a",
                            .kind = agent_id == "root-a" ? "root" : "subagent",
                            .task_id = agent_id == "root-a"
                                           ? std::nullopt
                                           : std::optional<std::string>{
                                                 "child-task"}}});
    };
    auto root_future = std::async(std::launch::async, invoke_self, "root-a");
    auto child_future = std::async(std::launch::async, invoke_self, "child-a");
    CHECK(!root_future.get()->is_error());
    CHECK(!child_future.get()->is_error());
    {
      std::scoped_lock lock(actor_mutex);
      CHECK(std::ranges::find(actors, "root-a") != actors.end());
      CHECK(std::ranges::find(actors, "child-a") != actors.end());
    }
    auto sent =
        find("agents_send")
            ->execute("1", R"({"session_id":"session-b","text":"hello"})");
    CHECK(!sent->is_error());
    CHECK(nlohmann::json::parse(sent->content()).at("message_id") == "m1");

    auto bad_target =
        find("agents_send")
            ->execute("2", R"({"agent_id":"a","session_id":"s","text":"x"})");
    CHECK(bad_target->is_error());
    CHECK(bad_target->content().find("invalid_message:") == 0);

    auto pending =
        find("agents_request")
            ->execute(
                "3",
                R"({"agent_id":"child-a","text":"question","timeout_ms":0})");
    CHECK(!pending->is_error());
    CHECK(nlohmann::json::parse(pending->content()).at("request_id") == "r1");

    auto inbox = find("agents_inbox")->execute("4", R"({})");
    CHECK(!inbox->is_error());
    const auto messages = nlohmann::json::parse(inbox->content());
    CHECK_EQ(messages.size(), std::size_t(2));
    CHECK(!messages[0].contains("claim_token"));
    CHECK(!messages[1].contains("claim_token"));
    CHECK_EQ(acked.size(), std::size_t(2));
    CHECK(acked[0].at("claim_token") == "secret");

    auto reply = find("agents_reply")
                     ->execute("5", R"({"message_id":"m2","text":"answer"})");
    CHECK(!reply->is_error());
    auto close =
        find("agents_close")->execute("6", R"({"agent_id":"child-a"})");
    CHECK(!close->is_error());
    auto remote_close =
        find("agents_close")->execute("7", R"({"agent_id":"remote-a"})");
    CHECK(remote_close->is_error());
  });

  tests::register_test(
      "mailbox addon clears actor after error and cancellation", [&]() {
        const auto path =
            std::filesystem::path(PI_CPP_SOURCE_DIR) / "addons" / "mailbox.lua";
        auto hooks = load_lua_hooks(path, true);
        LuaHooks::AgentInfo info;
        info.mailbox.self = [](const nlohmann::json &,
                               const std::optional<AgentRuntimeIdentity> &actor,
                               std::stop_token) {
          if (actor && actor->agent_id == "actor-a")
            return nlohmann::json{
                {"error", {{"code", "busy"}, {"message", "try again"}}}};
          return nlohmann::json{
              {"agent_id", actor ? actor->agent_id : "missing"}};
        };
        hooks->configure(info);
        auto find = [&](std::string_view name) {
          return *std::ranges::find_if(
              hooks->registered_tools,
              [&](const auto &tool) { return tool->name() == name; });
        };
        const auto actor = [](std::string id) {
          return AgentRuntimeIdentity{.agent_id = std::move(id),
                                      .session_id = "session",
                                      .kind = "subagent"};
        };
        auto failed =
            find("agents_self")
                ->execute("a", ToolExecutionContext{.call_id = "a",
                                                    .actor = actor("actor-a")});
        CHECK(failed->is_error());
        std::stop_source cancelled;
        cancelled.request_stop();
        auto stopped =
            find("agents_self")
                ->execute("cancel", ToolExecutionContext{
                                        .call_id = "cancel",
                                        .actor = actor("actor-a"),
                                        .stop_token = cancelled.get_token()});
        CHECK(stopped->is_error());
        auto recovered =
            find("agents_self")
                ->execute("b", ToolExecutionContext{.call_id = "b",
                                                    .actor = actor("actor-b")});
        CHECK(!recovered->is_error());
        CHECK_EQ(nlohmann::json::parse(recovered->content()).at("agent_id"),
                 "actor-b");
      });
}

int main() {
  std::cout << "=== pi-cpp lua tool tests ===\n\n";

  test_load_and_metadata();
  test_execute_string_return();
  test_lua_tool_progress();
  test_execute_table_return();
  test_lua_runtime_error();
  test_load_syntax_error();
  test_json_bridge();
  test_load_lua_tools_directory();
  test_lua_hooks();
  test_tool_formatter_hooks_only();
  test_mailbox_addon();

  tests::print_summary();
  return tests::failed == 0 ? 0 : 1;
}
