#include "core/lua_tool.h"

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "core/message_types.h"

namespace pi::core {
namespace {

// ─── JSON ↔ Lua bridge ──────────────────────────────────────────────────────

void json_to_lua(lua_State *L, const nlohmann::json &j) {
  if (j.is_object()) {
    lua_newtable(L);
    for (auto &[key, val] : j.items()) {
      lua_pushstring(L, key.c_str());
      json_to_lua(L, val);
      lua_rawset(L, -3);
    }
  } else if (j.is_array()) {
    lua_newtable(L);
    for (std::size_t i = 0; i < j.size(); ++i) {
      json_to_lua(L, j[i]);
      lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
    }
  } else if (j.is_string()) {
    lua_pushstring(L, j.get<std::string>().c_str());
  } else if (j.is_number_integer()) {
    lua_pushinteger(L, static_cast<lua_Integer>(j.get<std::int64_t>()));
  } else if (j.is_number()) {
    lua_pushnumber(L, j.get<double>());
  } else if (j.is_boolean()) {
    lua_pushboolean(L, j.get<bool>() ? 1 : 0);
  } else {
    lua_pushnil(L);
  }
}

nlohmann::json lua_to_json(lua_State *L, int idx);

nlohmann::json lua_table_to_json(lua_State *L, int idx) {
  if (idx < 0) {
    idx = lua_gettop(L) + idx + 1;
  }
  const int n = static_cast<int>(lua_rawlen(L, idx));
  int count = 0;
  bool all_int = true;
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    ++count;
    if (!lua_isinteger(L, -2)) {
      all_int = false;
    }
    lua_pop(L, 1);
  }
  if (all_int && count == n && n > 0) {
    nlohmann::json arr = nlohmann::json::array();
    for (int i = 1; i <= n; ++i) {
      lua_rawgeti(L, idx, i);
      arr.push_back(lua_to_json(L, lua_gettop(L)));
      lua_pop(L, 1);
    }
    return arr;
  }
  nlohmann::json obj = nlohmann::json::object();
  lua_pushnil(L);
  while (lua_next(L, idx) != 0) {
    std::string key;
    if (lua_type(L, -2) == LUA_TSTRING) {
      key = lua_tostring(L, -2);
    } else if (lua_type(L, -2) == LUA_TNUMBER) {
      if (lua_isinteger(L, -2)) {
        key = std::to_string(lua_tointeger(L, -2));
      } else {
        key = std::to_string(lua_tonumber(L, -2));
      }
    }
    if (!key.empty()) {
      obj[key] = lua_to_json(L, lua_gettop(L));
    }
    lua_pop(L, 1);
  }
  return obj;
}

nlohmann::json lua_to_json(lua_State *L, int idx) {
  switch (lua_type(L, idx)) {
  case LUA_TSTRING:
    return nlohmann::json(std::string(lua_tostring(L, idx)));
  case LUA_TNUMBER:
    if (lua_isinteger(L, idx)) {
      return nlohmann::json(lua_tointeger(L, idx));
    }
    return nlohmann::json(lua_tonumber(L, idx));
  case LUA_TBOOLEAN:
    return nlohmann::json(lua_toboolean(L, idx) != 0);
  case LUA_TTABLE:
    return lua_table_to_json(L, idx);
  default:
    return nlohmann::json(nullptr);
  }
}

// ─── json module exposed to Lua ─────────────────────────────────────────────

int lua_json_decode(lua_State *L) {
  const char *str = luaL_checkstring(L, 1);
  auto parsed = nlohmann::json::parse(str, nullptr, false);
  if (parsed.is_discarded()) {
    lua_pushnil(L);
    lua_pushstring(L, "invalid JSON");
    return 2;
  }
  json_to_lua(L, parsed);
  return 1;
}

int lua_json_encode(lua_State *L) {
  auto j = lua_to_json(L, 1);
  auto s = j.dump();
  lua_pushstring(L, s.c_str());
  return 1;
}

void register_json_module(lua_State *L) {
  lua_newtable(L);
  lua_pushcfunction(L, lua_json_decode);
  lua_setfield(L, -2, "decode");
  lua_pushcfunction(L, lua_json_encode);
  lua_setfield(L, -2, "encode");
  lua_setglobal(L, "json");
}

// ─── ToolResult ──────────────────────────────────────────────────────────────

class LuaToolResult : public ToolResult {
public:
  LuaToolResult(std::string content, bool is_error,
                std::optional<std::string> details = std::nullopt)
      : content_(std::move(content)), details_(std::move(details)),
        is_error_(is_error) {}

  bool is_error() const override { return is_error_; }
  std::string content() const override { return content_; }
  std::optional<std::string> details() const override { return details_; }

private:
  std::string content_;
  std::optional<std::string> details_;
  bool is_error_;
};

// ─── Schema ──────────────────────────────────────────────────────────────────

class LuaToolSchema : public JsonSchemaToolSchema {
public:
  explicit LuaToolSchema(std::string schema) : schema_(std::move(schema)) {}

  std::string serialize() const override { return schema_; }

  std::map<std::string, std::string> to_definition() const override {
    auto parsed = nlohmann::json::parse(schema_, nullptr, false);
    std::map<std::string, std::string> result;
    if (parsed.is_discarded() || !parsed.is_object()) {
      result["type"] = "object";
      return result;
    }
    for (auto it = parsed.begin(); it != parsed.end(); ++it) {
      if (it.value().is_string()) {
        result[it.key()] = it.value().get<std::string>();
      } else {
        result[it.key()] = it.value().dump();
      }
    }
    return result;
  }

private:
  std::string schema_;
};

// ─── LuaTool ─────────────────────────────────────────────────────────────────

class LuaTool final : public ToolDefinition {
public:
  explicit LuaTool(const std::filesystem::path &path) {
    L_ = luaL_newstate();
    if (!L_) {
      throw std::runtime_error("Failed to create Lua state");
    }
    luaL_openlibs(L_);
    register_json_module(L_);

    if (luaL_loadfile(L_, path.c_str()) != LUA_OK) {
      std::string err = lua_tostring(L_, -1);
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua load error in " + path.string() + ": " +
                               err);
    }
    if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
      std::string err = lua_tostring(L_, -1);
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua run error in " + path.string() + ": " +
                               err);
    }
    if (!lua_istable(L_, -1)) {
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua tool must return a table: " +
                               path.string());
    }

    lua_getfield(L_, -1, "name");
    if (!lua_isstring(L_, -1)) {
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua tool missing string 'name': " +
                               path.string());
    }
    name_ = lua_tostring(L_, -1);
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "description");
    if (!lua_isstring(L_, -1)) {
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua tool missing string 'description': " +
                               path.string());
    }
    description_ = lua_tostring(L_, -1);
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "schema");
    std::string schema_str;
    if (lua_isstring(L_, -1)) {
      schema_str = lua_tostring(L_, -1);
    } else {
      schema_str =
          R"json({"type":"object","additionalProperties":true})json";
    }
    lua_pop(L_, 1);
    schema_ = std::make_unique<LuaToolSchema>(std::move(schema_str));

    lua_getfield(L_, -1, "execute");
    if (!lua_isfunction(L_, -1)) {
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua tool missing 'execute' function: " +
                               path.string());
    }
    execute_ref_ = luaL_ref(L_, LUA_REGISTRYINDEX);
    source_path_ = path.string();

    lua_pop(L_, 1); // pop the module table
  }

  ~LuaTool() override {
    if (L_) {
      luaL_unref(L_, LUA_REGISTRYINDEX, execute_ref_);
      lua_close(L_);
    }
  }

  LuaTool(const LuaTool &) = delete;
  LuaTool &operator=(const LuaTool &) = delete;

  std::string_view name() const override { return name_; }
  std::string_view description() const override { return description_; }
  std::string_view source_path() const override { return source_path_; }
  ToolSchema &schema() const override { return *schema_; }

  std::shared_ptr<ToolResult> execute(std::string_view,
                                      std::string_view args_json, std::stop_token,
                                      ToolUpdateCallback) const override {
    std::lock_guard<std::mutex> lock(mutex_);

    lua_rawgeti(L_, LUA_REGISTRYINDEX, execute_ref_);

    auto args = nlohmann::json::parse(args_json, nullptr, false);
    if (args.is_discarded() || !args.is_object()) {
      args = nlohmann::json::object();
    }
    json_to_lua(L_, args);

    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
      std::string err = lua_tostring(L_, -1);
      lua_pop(L_, 1);
      return std::make_shared<LuaToolResult>(std::move(err), true);
    }

    std::shared_ptr<ToolResult> result;

    if (lua_isstring(L_, -1)) {
      result =
          std::make_shared<LuaToolResult>(std::string(lua_tostring(L_, -1)), false);
    } else if (lua_istable(L_, -1)) {
      lua_getfield(L_, -1, "content");
      std::string content =
          lua_isstring(L_, -1) ? lua_tostring(L_, -1) : std::string{};
      lua_pop(L_, 1);

      lua_getfield(L_, -1, "is_error");
      bool is_error = lua_toboolean(L_, -1) != 0;
      lua_pop(L_, 1);

      lua_getfield(L_, -1, "details");
      std::optional<std::string> details;
      if (lua_isstring(L_, -1)) {
        details = lua_tostring(L_, -1);
      }
      lua_pop(L_, 1);

      result = std::make_shared<LuaToolResult>(std::move(content), is_error,
                                               std::move(details));
    } else {
      result = std::make_shared<LuaToolResult>(std::string{}, false);
    }

    lua_pop(L_, 1);
    return result;
  }

private:
  lua_State *L_{nullptr};
  int execute_ref_{LUA_NOREF};
  std::string name_;
  std::string description_;
  std::string source_path_;
  std::unique_ptr<LuaToolSchema> schema_;
  mutable std::mutex mutex_;
};

// ─── Message serialization ───────────────────────────────────────────────────

static std::size_t count_turns(const std::vector<Message> &messages) {
  std::size_t n = 0;
  for (const auto &m : messages)
    if (std::holds_alternative<AssistantMessage>(m)) ++n;
  return n;
}

// Serialize message history to a Lua array.
// Each element: {index, role, content, [turn], [tool_name], [is_error]}
static void push_messages_to_lua(lua_State *L,
                                 const std::vector<Message> &messages) {
  lua_newtable(L);
  std::size_t turn = 0;
  for (std::size_t i = 0; i < messages.size(); ++i) {
    lua_newtable(L);

    // index (1-based)
    lua_pushinteger(L, static_cast<lua_Integer>(i + 1));
    lua_setfield(L, -2, "index");

    std::visit(
        [&](const auto &msg) {
          using T = std::decay_t<decltype(msg)>;
          if constexpr (std::is_same_v<T, UserMessage>) {
            lua_pushstring(L, "user");
            lua_setfield(L, -2, "role");
            std::string text;
            for (const auto &b : msg.content)
              if (const auto *tc = std::get_if<TextContent>(&b))
                text += tc->text;
            lua_pushstring(L, text.c_str());
            lua_setfield(L, -2, "content");
          } else if constexpr (std::is_same_v<T, AssistantMessage>) {
            ++turn;
            lua_pushstring(L, "assistant");
            lua_setfield(L, -2, "role");
            std::string text;
            for (const auto &b : msg.content)
              if (const auto *tc = std::get_if<TextContent>(&b))
                text += tc->text;
            lua_pushstring(L, text.c_str());
            lua_setfield(L, -2, "content");
            lua_pushinteger(L, static_cast<lua_Integer>(turn));
            lua_setfield(L, -2, "turn");
          } else if constexpr (std::is_same_v<T, ToolResultMessage>) {
            lua_pushstring(L, "tool_result");
            lua_setfield(L, -2, "role");
            lua_pushstring(L, msg.tool_name.c_str());
            lua_setfield(L, -2, "tool_name");
            std::string text;
            for (const auto &b : msg.content)
              if (const auto *tc = std::get_if<TextContent>(&b))
                text += tc->text;
            lua_pushstring(L, text.c_str());
            lua_setfield(L, -2, "content");
            lua_pushboolean(L, msg.is_error ? 1 : 0);
            lua_setfield(L, -2, "is_error");
          }
        },
        messages[i]);

    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
}

// ─── LuaHooksImpl ────────────────────────────────────────────────────────────

class LuaHooksImpl {
public:
  explicit LuaHooksImpl(const std::filesystem::path &path) {
    L_ = luaL_newstate();
    if (!L_) throw std::runtime_error("Failed to create Lua state for hooks");
    luaL_openlibs(L_);
    register_json_module(L_);

    if (luaL_loadfile(L_, path.c_str()) != LUA_OK) {
      std::string err = lua_tostring(L_, -1);
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua hooks load error in " + path.string() + ": " + err);
    }
    if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
      std::string err = lua_tostring(L_, -1);
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua hooks run error in " + path.string() + ": " + err);
    }
    if (!lua_istable(L_, -1)) {
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua hooks file must return a table: " + path.string());
    }

    // Extract optional hook refs
    auto extract = [&](const char *name) -> int {
      lua_getfield(L_, -1, name);
      if (lua_isfunction(L_, -1))
        return luaL_ref(L_, LUA_REGISTRYINDEX);
      lua_pop(L_, 1);
      return LUA_NOREF;
    };
    before_ref_         = extract("before_tool_call");
    after_ref_          = extract("after_tool_call");
    stop_after_ref_     = extract("should_stop_after_turn");
    command_ref_        = extract("on_command");
    complete_ref_       = extract("complete");

    // Extract commands array (data, not a function)
    lua_getfield(L_, -1, "commands");
    if (lua_istable(L_, -1)) {
      int n = static_cast<int>(lua_rawlen(L_, -1));
      for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L_, -1, i);
        if (lua_istable(L_, -1)) {
          LuaHooks::Command cmd;
          lua_getfield(L_, -1, "name");
          if (lua_isstring(L_, -1)) cmd.name = lua_tostring(L_, -1);
          lua_pop(L_, 1);
          lua_getfield(L_, -1, "description");
          if (lua_isstring(L_, -1)) cmd.description = lua_tostring(L_, -1);
          lua_pop(L_, 1);
          lua_getfield(L_, -1, "args_hint");
          if (lua_isstring(L_, -1)) cmd.args_hint = lua_tostring(L_, -1);
          lua_pop(L_, 1);
          if (!cmd.name.empty()) commands_.push_back(std::move(cmd));
        }
        lua_pop(L_, 1);
      }
    }
    lua_pop(L_, 1);

    lua_pop(L_, 1); // pop the module table

    // Register pici global — runtime fields filled in by configure_info()
    lua_newtable(L_);
    // pici.log — available immediately
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_log, 1);
    lua_setfield(L_, -2, "log");
    // pici.run_agent — available after configure_info()
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_run_agent, 1);
    lua_setfield(L_, -2, "run_agent");
    lua_setglobal(L_, "pici");
  }

  ~LuaHooksImpl() {
    if (L_) {
      if (before_ref_     != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, before_ref_);
      if (after_ref_      != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, after_ref_);
      if (stop_after_ref_ != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, stop_after_ref_);
      if (command_ref_    != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, command_ref_);
      if (complete_ref_   != LUA_NOREF) luaL_unref(L_, LUA_REGISTRYINDEX, complete_ref_);
      lua_close(L_);
    }
  }

  LuaHooksImpl(const LuaHooksImpl &) = delete;
  LuaHooksImpl &operator=(const LuaHooksImpl &) = delete;

  void set_source_path(std::string p) { source_path_ = std::move(p); }
  const std::string &source_path() const { return source_path_; }
  bool has_before()     const { return before_ref_     != LUA_NOREF; }
  bool has_after()      const { return after_ref_      != LUA_NOREF; }
  bool has_stop_after() const { return stop_after_ref_ != LUA_NOREF; }
  bool has_command()    const { return command_ref_    != LUA_NOREF; }
  bool has_complete()   const { return complete_ref_   != LUA_NOREF; }
  const std::vector<LuaHooks::Command> &commands() const { return commands_; }

  std::optional<BeforeToolCallResult>
  call_before(const BeforeToolCallContext &ctx) {
    std::lock_guard<std::mutex> lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, before_ref_);

    // Build context table
    lua_newtable(L_);
    lua_pushstring(L_, ctx.tool_call.name.c_str());
    lua_setfield(L_, -2, "tool_name");
    lua_pushstring(L_, ctx.tool_call.id.c_str());
    lua_setfield(L_, -2, "call_id");
    json_to_lua(L_, ctx.tool_call.arguments);
    lua_setfield(L_, -2, "args");
    lua_pushinteger(L_, static_cast<lua_Integer>(count_turns(ctx.context.messages)));
    lua_setfield(L_, -2, "turn");

    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return std::nullopt;
    }

    std::optional<BeforeToolCallResult> result;
    if (lua_istable(L_, -1)) {
      BeforeToolCallResult r;
      lua_getfield(L_, -1, "block");
      r.block = lua_toboolean(L_, -1) != 0;
      lua_pop(L_, 1);
      lua_getfield(L_, -1, "reason");
      if (lua_isstring(L_, -1)) r.reason = lua_tostring(L_, -1);
      lua_pop(L_, 1);
      result = std::move(r);
    }
    lua_pop(L_, 1);
    return result;
  }

  std::optional<AfterToolCallResult>
  call_after(const AfterToolCallContext &ctx) {
    std::lock_guard<std::mutex> lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, after_ref_);

    // Collect text content from result
    std::string content_str = ctx.result ? ctx.result->content() : std::string{};

    lua_newtable(L_);
    lua_pushstring(L_, ctx.tool_call.name.c_str());
    lua_setfield(L_, -2, "tool_name");
    lua_pushstring(L_, ctx.tool_call.id.c_str());
    lua_setfield(L_, -2, "call_id");
    json_to_lua(L_, ctx.tool_call.arguments);
    lua_setfield(L_, -2, "args");
    lua_pushstring(L_, content_str.c_str());
    lua_setfield(L_, -2, "content");
    lua_pushboolean(L_, ctx.is_error ? 1 : 0);
    lua_setfield(L_, -2, "is_error");
    lua_pushinteger(L_, static_cast<lua_Integer>(count_turns(ctx.context.messages)));
    lua_setfield(L_, -2, "turn");

    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return std::nullopt;
    }

    std::optional<AfterToolCallResult> result;
    if (lua_istable(L_, -1)) {
      AfterToolCallResult r;
      lua_getfield(L_, -1, "terminate");
      if (lua_toboolean(L_, -1)) r.terminate = true;
      lua_pop(L_, 1);
      lua_getfield(L_, -1, "content");
      if (lua_isstring(L_, -1)) {
        r.content = std::vector<ToolResultContentBlock>{
            TextContent{std::string(lua_tostring(L_, -1))}};
      }
      lua_pop(L_, 1);
      lua_getfield(L_, -1, "is_error");
      if (!lua_isnil(L_, -1)) r.is_error = lua_toboolean(L_, -1) != 0;
      lua_pop(L_, 1);
      result = std::move(r);
    }
    lua_pop(L_, 1);
    return result;
  }

  bool call_stop_after(const Message &msg,
                       const std::vector<ToolResultMessage> &tool_results,
                       const AgentContext &) {
    std::lock_guard<std::mutex> lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, stop_after_ref_);

    // Extract text from assistant message
    std::string msg_text;
    if (const auto *am = std::get_if<AssistantMessage>(&msg)) {
      for (const auto &block : am->content) {
        if (const auto *tc = std::get_if<TextContent>(&block))
          msg_text += tc->text;
      }
    }

    lua_newtable(L_);
    lua_pushstring(L_, msg_text.c_str());
    lua_setfield(L_, -2, "message");

    // tool_results array
    lua_newtable(L_);
    for (std::size_t i = 0; i < tool_results.size(); ++i) {
      const auto &tr = tool_results[i];
      lua_newtable(L_);
      lua_pushstring(L_, tr.tool_name.c_str());
      lua_setfield(L_, -2, "tool_name");
      // Collect text content
      std::string tr_content;
      for (const auto &block : tr.content)
        if (const auto *tc = std::get_if<TextContent>(&block))
          tr_content += tc->text;
      lua_pushstring(L_, tr_content.c_str());
      lua_setfield(L_, -2, "content");
      lua_pushboolean(L_, tr.is_error ? 1 : 0);
      lua_setfield(L_, -2, "is_error");
      lua_rawseti(L_, -2, static_cast<lua_Integer>(i) + 1);
    }
    lua_setfield(L_, -2, "tool_results");

    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return false;
    }
    bool stop = lua_toboolean(L_, -1) != 0;
    lua_pop(L_, 1);
    return stop;
  }

  // ── Storage helpers ──────────────────────────────────────────────────

  void load_storage() {
    if (storage_path_.empty()) return;
    std::ifstream f(storage_path_);
    if (!f) { storage_ = nlohmann::json::object(); return; }
    storage_ = nlohmann::json::parse(f, nullptr, false);
    if (storage_.is_discarded()) storage_ = nlohmann::json::object();
  }

  void save_storage() {
    if (storage_path_.empty()) return;
    std::ofstream f(storage_path_);
    f << storage_.dump(2);
  }

  // ── pici C closures ──────────────────────────────────────────────────

  static LuaHooksImpl *impl_from(lua_State *L) {
    return static_cast<LuaHooksImpl *>(lua_touserdata(L, lua_upvalueindex(1)));
  }

  static int lua_pici_log(lua_State *L) {
    int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
      if (i > 1) std::cerr << '\t';
      std::cerr << luaL_tolstring(L, i, nullptr);
      lua_pop(L, 1);
    }
    std::cerr << '\n';
    return 0;
  }

  static int lua_pici_run_agent(lua_State *L) {
    auto *impl = impl_from(L);
    // run_agent_fn_ set once before calls — no lock (would deadlock from on_command)
    const LuaHooks::RunAgentFn &fn = impl->run_agent_fn_;
    if (!fn) {
      lua_pushnil(L); lua_pushstring(L, "pici.run_agent not available"); return 2;
    }
    LuaHooks::AgentRunConfig cfg;
    if (lua_istable(L, 1)) {
      auto sfield = [&](const char *k) -> std::string {
        lua_getfield(L, 1, k);
        std::string s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1); return s;
      };
      cfg.prompt = sfield("prompt");
      auto sp = sfield("system_prompt");
      if (!sp.empty()) cfg.system_prompt = sp;
      auto mid = sfield("model");
      if (!mid.empty()) cfg.model_id = mid;
      lua_getfield(L, 1, "fork_at");
      if (lua_isinteger(L, -1)) {
        auto n = lua_tointeger(L, -1);
        if (n > 0) cfg.fork_at = static_cast<std::size_t>(n);
      }
      lua_pop(L, 1);
      lua_getfield(L, 1, "tools");
      if (lua_istable(L, -1)) {
        int n = static_cast<int>(lua_rawlen(L, -1));
        for (int i = 1; i <= n; ++i) {
          lua_rawgeti(L, -1, i);
          if (lua_isstring(L, -1)) cfg.tools.push_back(lua_tostring(L, -1));
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);
    }
    if (cfg.prompt.empty()) {
      lua_pushnil(L); lua_pushstring(L, "pici.run_agent: prompt is required"); return 2;
    }
    auto result = fn(cfg);
    lua_newtable(L);
    lua_pushstring(L, result.text.c_str()); lua_setfield(L, -2, "text");
    if (result.error) lua_pushstring(L, result.error->c_str()); else lua_pushnil(L);
    lua_setfield(L, -2, "error");
    return 1;
  }

  // Storage closures are only ever called from within a Lua pcall, which means
  // the outer hook-call mutex is already held by this thread — do not re-lock.
  static int lua_storage_get(lua_State *L) {
    auto *impl = impl_from(L);
    const char *key = luaL_checkstring(L, 1);
    if (!impl->storage_.contains(key)) { lua_pushnil(L); return 1; }
    json_to_lua(L, impl->storage_[key]);
    return 1;
  }

  static int lua_storage_set(lua_State *L) {
    auto *impl = impl_from(L);
    const char *key = luaL_checkstring(L, 1);
    impl->storage_[key] = lua_to_json(L, 2);
    impl->save_storage();
    return 0;
  }

  static int lua_storage_clear(lua_State *L) {
    auto *impl = impl_from(L);
    impl->storage_ = nlohmann::json::object();
    impl->save_storage();
    return 0;
  }

  // ── configure_info ────────────────────────────────────────────────────

  void configure_info(const LuaHooks::AgentInfo &info) {
    std::lock_guard<std::mutex> lk(mutex_);
    run_agent_fn_ = info.run_agent;
    storage_path_ = info.storage_path;
    load_storage();

    lua_getglobal(L_, "pici");

    // pici.model → {id, provider, api}
    lua_newtable(L_);
    lua_pushstring(L_, info.model_id.c_str());       lua_setfield(L_, -2, "id");
    lua_pushstring(L_, info.model_provider.c_str()); lua_setfield(L_, -2, "provider");
    lua_pushstring(L_, info.model_api.c_str());      lua_setfield(L_, -2, "api");
    lua_setfield(L_, -2, "model");

    // pici.tools → array of strings
    lua_newtable(L_);
    for (std::size_t i = 0; i < info.tool_names.size(); ++i) {
      lua_pushstring(L_, info.tool_names[i].c_str());
      lua_rawseti(L_, -2, static_cast<lua_Integer>(i) + 1);
    }
    lua_setfield(L_, -2, "tools");

    // pici.cwd → string
    lua_pushstring(L_, info.cwd.c_str());
    lua_setfield(L_, -2, "cwd");

    // pici.storage → {get, set, clear, path}
    lua_newtable(L_);
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_storage_get, 1);
    lua_setfield(L_, -2, "get");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_storage_set, 1);
    lua_setfield(L_, -2, "set");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_storage_clear, 1);
    lua_setfield(L_, -2, "clear");
    lua_pushstring(L_, storage_path_.string().c_str());
    lua_setfield(L_, -2, "path");
    lua_setfield(L_, -2, "storage");

    lua_pop(L_, 1);  // pop pici
  }

  std::vector<std::string> call_complete(std::string_view partial,
                                         const std::vector<Message> &transcript) {
    std::lock_guard<std::mutex> lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, complete_ref_);
    lua_pushlstring(L_, partial.data(), partial.size());
    push_messages_to_lua(L_, transcript);

    if (lua_pcall(L_, 2, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return {};
    }

    std::vector<std::string> result;
    if (lua_istable(L_, -1)) {
      int n = static_cast<int>(lua_rawlen(L_, -1));
      for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L_, -1, i);
        if (lua_isstring(L_, -1))
          result.push_back(lua_tostring(L_, -1));
        lua_pop(L_, 1);
      }
    }
    lua_pop(L_, 1);
    return result;
  }

  LuaHooks::CommandResult call_on_command(std::string_view cmd,
                                          std::string_view args,
                                          const std::vector<Message> &transcript) {
    std::lock_guard<std::mutex> lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, command_ref_);
    lua_pushlstring(L_, cmd.data(), cmd.size());
    lua_pushlstring(L_, args.data(), args.size());
    push_messages_to_lua(L_, transcript);

    if (lua_pcall(L_, 3, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return {};
    }

    LuaHooks::CommandResult result;
    if (lua_istable(L_, -1)) {
      lua_getfield(L_, -1, "handled");
      result.handled = lua_toboolean(L_, -1) != 0;
      lua_pop(L_, 1);

      lua_getfield(L_, -1, "truncate_to");
      if (lua_isinteger(L_, -1)) {
        auto n = lua_tointeger(L_, -1);
        if (n > 0)
          result.truncate_to = static_cast<std::size_t>(n);
      }
      lua_pop(L_, 1);

      lua_getfield(L_, -1, "prompt");
      if (lua_isstring(L_, -1))
        result.prompt = lua_tostring(L_, -1);
      lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    return result;
  }

private:
  lua_State *L_{nullptr};
  int before_ref_{LUA_NOREF};
  int after_ref_{LUA_NOREF};
  int stop_after_ref_{LUA_NOREF};
  int command_ref_{LUA_NOREF};
  int complete_ref_{LUA_NOREF};
  std::string source_path_;
  std::vector<LuaHooks::Command> commands_;
  LuaHooks::RunAgentFn run_agent_fn_;
  nlohmann::json storage_{nlohmann::json::object()};
  std::filesystem::path storage_path_;
  mutable std::mutex mutex_;
};

} // namespace

// ─── Public API ──────────────────────────────────────────────────────────────

std::shared_ptr<const ToolDefinition>
load_lua_tool(const std::filesystem::path &path) {
  return std::make_shared<LuaTool>(path);
}

std::vector<std::shared_ptr<const ToolDefinition>>
load_lua_tools(const std::filesystem::path &directory) {
  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  std::error_code ec;
  for (const auto &entry :
       std::filesystem::directory_iterator(directory, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    if (entry.path().extension() != ".lua") {
      continue;
    }
    try {
      tools.push_back(load_lua_tool(entry.path()));
    } catch (const std::exception &) {
      // skip tools that fail to load
    }
  }
  return tools;
}

std::shared_ptr<LuaHooks>
load_lua_hooks(const std::filesystem::path &path) {
  auto impl = std::make_shared<LuaHooksImpl>(path);
  auto hooks = std::make_shared<LuaHooks>();

  if (impl->has_before()) {
    hooks->before_tool_call =
        [impl](const BeforeToolCallContext &ctx,
               std::stop_token) -> std::optional<BeforeToolCallResult> {
      return impl->call_before(ctx);
    };
  }
  if (impl->has_after()) {
    hooks->after_tool_call =
        [impl](const AfterToolCallContext &ctx,
               std::stop_token) -> std::optional<AfterToolCallResult> {
      return impl->call_after(ctx);
    };
  }
  if (impl->has_stop_after()) {
    hooks->should_stop_after_turn =
        [impl](const Message &msg,
               const std::vector<ToolResultMessage> &results,
               const AgentContext &ctx) -> bool {
      return impl->call_stop_after(msg, results, ctx);
    };
  }
  if (impl->has_command()) {
    hooks->on_command =
        [impl](std::string_view cmd, std::string_view args,
               const std::vector<Message> &transcript) -> LuaHooks::CommandResult {
      return impl->call_on_command(cmd, args, transcript);
    };
  }
  hooks->configure = [impl](const LuaHooks::AgentInfo &info) {
    impl->configure_info(info);
  };
  impl->set_source_path(path.string());
  hooks->source_path = impl->source_path();
  hooks->commands    = impl->commands();
  if (impl->has_complete()) {
    hooks->complete =
        [impl](std::string_view partial,
               const std::vector<Message> &transcript) -> std::vector<std::string> {
      return impl->call_complete(partial, transcript);
    };
  }
  return hooks;
}

// ─── Test runner ─────────────────────────────────────────────────────────────

static const char kPiciTestLua[] = R"lua(
do
  local _r = {passed=0, failed=0, total=0}
  pici.test = {}

  function pici.test.run(name, fn)
    _r.total = _r.total + 1
    local ok, err = pcall(fn)
    if ok then
      _r.passed = _r.passed + 1
      io.write("  PASS " .. name .. "\n")
    else
      _r.failed = _r.failed + 1
      io.write("  FAIL " .. name .. ": " .. tostring(err) .. "\n")
    end
  end

  function pici.test.eq(a, b, msg)
    if a ~= b then
      error((msg and (msg .. ": ") or "") ..
            "expected " .. tostring(b) .. " got " .. tostring(a), 2)
    end
  end

  function pici.test.ok(val, msg)
    if not val then
      error((msg or "expected truthy") .. " got " .. tostring(val), 2)
    end
  end

  function pici.test.fail(msg) error(msg or "explicit failure", 2) end

  function pici.test.results() return _r.passed, _r.failed, _r.total end

  -- Override pici.run_agent with a Lua function for testing
  pici.mock_run_agent = function(fn) pici.run_agent = fn end
end
)lua";

TestResult run_lua_test_file(const std::filesystem::path &path) {
  lua_State *L = luaL_newstate();
  if (!L) throw std::runtime_error("Failed to create Lua state for test runner");
  luaL_openlibs(L);
  register_json_module(L);

  // Minimal pici global: log + stub run_agent (overridden by mock_run_agent)
  lua_newtable(L);
  lua_pushcfunction(L, [](lua_State *l) -> int {
    int n = lua_gettop(l);
    for (int i = 1; i <= n; ++i) {
      if (i > 1) std::cerr << '\t';
      std::cerr << luaL_tolstring(l, i, nullptr);
      lua_pop(l, 1);
    }
    std::cerr << '\n';
    return 0;
  });
  lua_setfield(L, -2, "log");
  lua_pushcfunction(L, [](lua_State *l) -> int {
    lua_pushnil(l);
    lua_pushstring(l, "pici.run_agent: use pici.mock_run_agent() in tests");
    return 2;
  });
  lua_setfield(L, -2, "run_agent");
  lua_setglobal(L, "pici");

  // Bootstrap pici.test and pici.mock_run_agent
  if (luaL_dostring(L, kPiciTestLua) != LUA_OK) {
    std::string err = lua_tostring(L, -1);
    lua_close(L);
    throw std::runtime_error("pici.test bootstrap error: " + err);
  }

  // Load and run the test file
  if (luaL_dofile(L, path.c_str()) != LUA_OK) {
    std::string err = lua_tostring(L, -1);
    lua_close(L);
    throw std::runtime_error(std::string(path) + ": " + err);
  }

  // Collect results via pici.test.results()
  TestResult r;
  lua_getglobal(L, "pici");
  lua_getfield(L, -1, "test");
  lua_getfield(L, -1, "results");
  if (lua_pcall(L, 0, 3, 0) == LUA_OK) {
    r.passed = static_cast<int>(lua_tointeger(L, -3));
    r.failed = static_cast<int>(lua_tointeger(L, -2));
    r.total  = static_cast<int>(lua_tointeger(L, -1));
  }
  lua_close(L);
  return r;
}

std::shared_ptr<LuaHooks>
load_lua_hooks_dir(const std::filesystem::path &directory) {
  std::vector<std::shared_ptr<LuaHooks>> list;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".lua")
      continue;
    try {
      list.push_back(load_lua_hooks(entry.path()));
    } catch (const std::exception &e) {
      std::cerr << "warning: skipping hooks file " << entry.path()
                << ": " << e.what() << "\n";
    }
  }
  return compose_hooks(std::move(list));
}

std::shared_ptr<LuaHooks>
compose_hooks(std::vector<std::shared_ptr<LuaHooks>> list) {
  // Drop nulls
  list.erase(std::remove_if(list.begin(), list.end(),
                             [](const auto &h) { return !h; }),
             list.end());
  if (list.empty())  return nullptr;
  if (list.size() == 1) return list[0];

  auto out = std::make_shared<LuaHooks>();

  // before_tool_call — run all; first block wins
  if (std::any_of(list.begin(), list.end(),
                  [](const auto &h) { return !!h->before_tool_call; })) {
    out->before_tool_call =
        [list](const BeforeToolCallContext &ctx,
               std::stop_token st) -> std::optional<BeforeToolCallResult> {
      for (const auto &h : list) {
        if (!h->before_tool_call) continue;
        auto r = h->before_tool_call(ctx, st);
        if (r && r->block) return r;
      }
      return std::nullopt;
    };
  }

  // after_tool_call — run all; first non-null wins
  if (std::any_of(list.begin(), list.end(),
                  [](const auto &h) { return !!h->after_tool_call; })) {
    out->after_tool_call =
        [list](const AfterToolCallContext &ctx,
               std::stop_token st) -> std::optional<AfterToolCallResult> {
      for (const auto &h : list) {
        if (!h->after_tool_call) continue;
        auto r = h->after_tool_call(ctx, st);
        if (r) return r;
      }
      return std::nullopt;
    };
  }

  // should_stop_after_turn — OR
  if (std::any_of(list.begin(), list.end(),
                  [](const auto &h) { return !!h->should_stop_after_turn; })) {
    out->should_stop_after_turn =
        [list](const Message &msg,
               const std::vector<ToolResultMessage> &results,
               const AgentContext &ctx) -> bool {
      for (const auto &h : list) {
        if (!h->should_stop_after_turn) continue;
        if (h->should_stop_after_turn(msg, results, ctx)) return true;
      }
      return false;
    };
  }

  // on_command — first handled wins
  if (std::any_of(list.begin(), list.end(),
                  [](const auto &h) { return !!h->on_command; })) {
    out->on_command =
        [list](std::string_view cmd, std::string_view args,
               const std::vector<Message> &transcript) -> LuaHooks::CommandResult {
      for (const auto &h : list) {
        if (!h->on_command) continue;
        auto r = h->on_command(cmd, args, transcript);
        if (r.handled) return r;
      }
      return {};
    };
  }

  // commands — union
  for (const auto &h : list)
    out->commands.insert(out->commands.end(), h->commands.begin(), h->commands.end());

  // configure — forward to all
  out->configure = [list](const LuaHooks::AgentInfo &info) {
    for (const auto &h : list)
      if (h->configure) h->configure(info);
  };

  // complete — union of all results
  if (std::any_of(list.begin(), list.end(),
                  [](const auto &h) { return !!h->complete; })) {
    out->complete =
        [list](std::string_view partial,
               const std::vector<Message> &transcript) -> std::vector<std::string> {
      std::vector<std::string> result;
      for (const auto &h : list) {
        if (!h->complete) continue;
        auto r = h->complete(partial, transcript);
        result.insert(result.end(), r.begin(), r.end());
      }
      return result;
    };
  }

  return out;
}

} // namespace pi::core
