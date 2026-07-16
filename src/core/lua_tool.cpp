#include "core/lua_tool.h"
#include "core/agent_loop.h"
#include "core/agent_state.h"
#include "nlohmann/json_fwd.hpp"
#include <cstddef>
#include <cstdint>
#include <exception>
#include <type_traits>
#include <utility>
#include <variant>

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
#include "core/event_json.h"

namespace pi::core {
namespace {

// NOLINTNEXTLINE(misc-no-recursion)
void json_to_lua(lua_State *L, const nlohmann::json &j) {
  if (j.is_object()) {
    lua_newtable(L);
    for (const auto &[key, val] : j.items()) {
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
    const auto &value = j.get_ref<const std::string &>();
    lua_pushlstring(L, value.data(), value.size());
  } else if (j.is_number_integer()) {
    lua_pushinteger(L, static_cast<lua_Integer>(j.get<std::int64_t>()));
  } else if (j.is_number_unsigned()) {
    lua_pushinteger(L, static_cast<lua_Integer>(j.get<std::uint64_t>()));
  } else if (j.is_number()) {
    lua_pushnumber(L, j.get<double>());
  } else if (j.is_boolean()) {
    lua_pushboolean(L, j.get<bool>() ? 1 : 0);
  } else {
    lua_pushnil(L);
  }
}

nlohmann::json lua_to_json(lua_State *L, int idx);

// NOLINTNEXTLINE(misc-no-recursion)
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
    if (lua_isinteger(L, -2) == 0) {
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
      if (lua_isinteger(L, -2) != 0) {
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

// NOLINTNEXTLINE(misc-no-recursion)
nlohmann::json lua_to_json(lua_State *L, int idx) {
  switch (lua_type(L, idx)) {
  case LUA_TSTRING:
    return nlohmann::json( // NOLINT(modernize-return-braced-init-list)
        std::string(lua_tostring(L, idx)));
  case LUA_TNUMBER:
    if (lua_isinteger(L, idx) != 0) {
      return nlohmann::json( // NOLINT(modernize-return-braced-init-list)
          lua_tointeger(L, idx));
    }
    return nlohmann::json( // NOLINT(modernize-return-braced-init-list)
        lua_tonumber(L, idx));
  case LUA_TBOOLEAN:
    return nlohmann::json( // NOLINT(modernize-return-braced-init-list)
        lua_toboolean(L, idx) != 0);
  case LUA_TTABLE:
    return lua_table_to_json(L, idx);
  default:
    return {nullptr};
  }
}

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

int lua_tool_update_callback(lua_State *L) {
  auto *callback = static_cast<ToolUpdateCallback *>(
      lua_touserdata(L, lua_upvalueindex(1)));
  if (callback == nullptr || !*callback)
    return 0;

  auto value = lua_to_json(L, 1);
  std::string content;
  bool is_error = false;
  if (value.is_string()) {
    content = value.get<std::string>();
  } else if (value.is_object() && value.value("content", nlohmann::json{})
                                      .is_string()) {
    content = value["content"].get<std::string>();
    is_error = value.value("is_error", false);
  } else {
    content = value.dump();
  }
  try {
    (*callback)(std::make_shared<LuaToolResult>(std::move(content), is_error));
  } catch (...) {
    // Never let a C++ observer exception cross the Lua C ABI boundary.
  }
  return 0;
}

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

class LuaTool final : public ToolDefinition {
public:
  explicit LuaTool(const std::filesystem::path &path) : L_(luaL_newstate()) {

    if (L_ == nullptr) {
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
    if (lua_isstring(L_, -1) == 0) {
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua tool missing string 'name': " +
                               path.string());
    }
    name_ = lua_tostring(L_, -1);
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "description");
    if (lua_isstring(L_, -1) == 0) {
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua tool missing string 'description': " +
                               path.string());
    }
    description_ = lua_tostring(L_, -1);
    lua_pop(L_, 1);

    lua_getfield(L_, -1, "schema");
    std::string schema_str;
    if (lua_isstring(L_, -1) != 0) {
      schema_str = lua_tostring(L_, -1);
    } else {
      schema_str = R"json({"type":"object","additionalProperties":true})json";
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
    if (L_ != nullptr) {
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
                                      std::string_view args_json,
                                      std::stop_token,
                                      ToolUpdateCallback on_update) const override {
    std::scoped_lock lock(mutex_);

    lua_rawgeti(L_, LUA_REGISTRYINDEX, execute_ref_);

    auto args = nlohmann::json::parse(args_json, nullptr, false);
    if (args.is_discarded() || !args.is_object()) {
      args = nlohmann::json::object();
    }
    json_to_lua(L_, args);

    lua_newtable(L_);
    lua_pushlightuserdata(L_, &on_update);
    lua_pushcclosure(L_, &lua_tool_update_callback, 1);
    lua_setfield(L_, -2, "update");

    if (lua_pcall(L_, 2, 1, 0) != LUA_OK) {
      std::string err = lua_tostring(L_, -1);
      lua_pop(L_, 1);
      return std::make_shared<LuaToolResult>(std::move(err), true);
    }

    std::shared_ptr<ToolResult> result;

    if (lua_isstring(L_, -1) != 0) {
      result = std::make_shared<LuaToolResult>(
          std::string(lua_tostring(L_, -1)), false);
    } else if (lua_istable(L_, -1)) {
      lua_getfield(L_, -1, "content");
      std::string content =
          (lua_isstring(L_, -1) != 0) ? lua_tostring(L_, -1) : std::string{};
      lua_pop(L_, 1);

      lua_getfield(L_, -1, "is_error");
      bool is_error = lua_toboolean(L_, -1) != 0;
      lua_pop(L_, 1);

      lua_getfield(L_, -1, "details");
      std::optional<std::string> details;
      if (lua_isstring(L_, -1) != 0) {
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

std::size_t count_turns(const std::vector<Message> &messages) {
  std::size_t n = 0;
  for (const auto &m : messages)
    if (std::holds_alternative<AssistantMessage>(m))
      ++n;
  return n;
}

// Serialize message history to a Lua array.
// Each element: {index, role, content, [turn], [tool_name], [is_error]}
void push_messages_to_lua(lua_State *L, const std::vector<Message> &messages) {
  lua_newtable(L);
  std::size_t turn = 0;
  for (std::size_t i = 0; i < messages.size(); ++i) {
    lua_newtable(L);

    // index (1-based)
    lua_pushinteger(L, static_cast<lua_Integer>(i) + 1);
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

    lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
  }
}

// Serialize the complete internal message representation. The canonical JSON
// serializer is shared with session persistence so this view does not acquire
// a second, lossy message format.
void push_complete_messages_to_lua(lua_State *L,
                                   const std::vector<Message> &messages) {
  lua_newtable(L);
  std::size_t turn = 0;
  for (std::size_t i = 0; i < messages.size(); ++i) {
    auto value =
        nlohmann::json::parse(json::to_json(messages[i]), nullptr, false);
    if (!value.is_object()) {
      value = nlohmann::json{{"role", "unknown"},
                             {"content", nlohmann::json::array()}};
    }
    value["index"] = i + 1;
    if (std::holds_alternative<AssistantMessage>(messages[i]))
      value["turn"] = ++turn;

    json_to_lua(L, value);
    lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
  }
}

void push_model_to_lua(lua_State *L, const Model &model) {
  auto value = nlohmann::json::parse(json::to_json(model), nullptr, false);
  if (!value.is_object())
    value = nlohmann::json::object();

  value["cost"] = {
      {"inputPerMtok", model.cost.input_per_mtok},
      {"outputPerMtok", model.cost.output_per_mtok},
      {"cacheReadPerMtok", model.cost.cache_read_per_mtok},
      {"cacheWritePerMtok", model.cost.cache_write_per_mtok},
  };
  json_to_lua(L, value);
}

void push_tools_to_lua(
    lua_State *L,
    const std::vector<std::shared_ptr<const ToolDefinition>> &tools) {
  lua_newtable(L);
  for (std::size_t i = 0; i < tools.size(); ++i) {
    const auto &tool = tools[i];
    lua_newtable(L);
    lua_pushlstring(L, tool->name().data(), tool->name().size());
    lua_setfield(L, -2, "name");
    lua_pushlstring(L, tool->description().data(), tool->description().size());
    lua_setfield(L, -2, "description");
    const auto source = tool->source_path();
    lua_pushlstring(L, source.data(), source.size());
    lua_setfield(L, -2, "source_path");

    auto schema =
        nlohmann::json::parse(tool->schema().serialize(), nullptr, false);
    const bool valid_schema = schema.is_object();
    if (!valid_schema)
      schema = nlohmann::json::object();
    json_to_lua(L, schema);
    lua_setfield(L, -2, "input_schema");
    lua_pushboolean(L, valid_schema ? 1 : 0);
    lua_setfield(L, -2, "schema_valid");

    lua_rawseti(L, -2, static_cast<lua_Integer>(i) + 1);
  }
}

void push_agent_context_to_lua(lua_State *L, const AgentContext &context) {
  lua_newtable(L);
  lua_pushlstring(L, context.system_prompt.data(),
                  context.system_prompt.size());
  lua_setfield(L, -2, "system_prompt");
  push_complete_messages_to_lua(L, context.messages);
  lua_setfield(L, -2, "messages");
  push_model_to_lua(L, context.model);
  lua_setfield(L, -2, "model");
  push_tools_to_lua(L, context.tools);
  lua_setfield(L, -2, "tools");
}

void push_context_snapshot_to_lua(lua_State *L,
                                  const LuaContextSnapshot &snapshot) {
  lua_newtable(L);
  const int context_index = lua_gettop(L);
  push_agent_context_to_lua(L, snapshot.raw);
  lua_setfield(L, -2, "raw");

  if (!snapshot.effective) {
    lua_newtable(L);
    lua_pushboolean(L, 0);
    lua_setfield(L, -2, "available");
  } else {
    push_agent_context_to_lua(L, *snapshot.effective);
    lua_pushboolean(L, 1);
    lua_setfield(L, -2, "available");
    lua_pushlstring(L, snapshot.effective->model.provider.data(),
                    snapshot.effective->model.provider.size());
    lua_setfield(L, -2, "provider");
    lua_pushlstring(L, snapshot.effective->model.api.data(),
                    snapshot.effective->model.api.size());
    lua_setfield(L, -2, "api");
  }
  lua_setfield(L, context_index, "effective");
}

// Forward-declared; defined after LuaHooksImpl.
class LuaHooksImpl;

class InlineLuaTool final : public ToolDefinition {
public:
  InlineLuaTool(std::shared_ptr<LuaHooksImpl> impl, int exec_ref,
                std::string name, std::string description,
                std::string schema_str, std::string source)
      : impl_(std::move(impl)), exec_ref_(exec_ref), name_(std::move(name)),
        description_(std::move(description)), source_(std::move(source)),
        schema_(std::make_unique<LuaToolSchema>(std::move(schema_str))) {}

  ~InlineLuaTool() override;
  InlineLuaTool(const InlineLuaTool &) = delete;
  InlineLuaTool &operator=(const InlineLuaTool &) = delete;
  InlineLuaTool(InlineLuaTool &&) = delete;
  InlineLuaTool &operator=(InlineLuaTool &&) = delete;

  std::string_view name() const override { return name_; }
  std::string_view description() const override { return description_; }
  std::string_view source_path() const override { return source_; }
  ToolSchema &schema() const override { return *schema_; }

  std::shared_ptr<ToolResult> execute(std::string_view call_id,
                                      std::string_view args_json,
                                      std::stop_token st,
                                      ToolUpdateCallback cb) const override;

private:
  std::shared_ptr<LuaHooksImpl> impl_;
  int exec_ref_;
  std::string name_, description_, source_;
  std::unique_ptr<LuaToolSchema> schema_;
};

class LuaHooksImpl : public std::enable_shared_from_this<LuaHooksImpl> {
public:
  explicit LuaHooksImpl(const std::filesystem::path &path)
      : L_(luaL_newstate()) {

    if (L_ == nullptr)
      throw std::runtime_error("Failed to create Lua state for hooks");
    luaL_openlibs(L_);
    register_json_module(L_);

    // Register pici global BEFORE executing the file so pici.add_tool() works
    lua_newtable(L_);
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_log, 1);
    lua_setfield(L_, -2, "log");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_run_agent, 1);
    lua_setfield(L_, -2, "run_agent");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_add_tool, 1);
    lua_setfield(L_, -2, "add_tool");
    lua_setglobal(L_, "pici");

    if (luaL_loadfile(L_, path.c_str()) != LUA_OK) {
      std::string err = lua_tostring(L_, -1);
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua hooks load error in " + path.string() +
                               ": " + err);
    }
    if (lua_pcall(L_, 0, 1, 0) != LUA_OK) {
      std::string err = lua_tostring(L_, -1);
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua hooks run error in " + path.string() +
                               ": " + err);
    }
    if (!lua_istable(L_, -1)) {
      lua_close(L_);
      L_ = nullptr;
      throw std::runtime_error("Lua hooks file must return a table: " +
                               path.string());
    }

    // Extract optional hook refs
    auto extract = [&](const char *name) -> int {
      lua_getfield(L_, -1, name);
      if (lua_isfunction(L_, -1))
        return luaL_ref(L_, LUA_REGISTRYINDEX);
      lua_pop(L_, 1);
      return LUA_NOREF;
    };
    before_ref_ = extract("before_tool_call");
    on_event_ref_ = extract("on_event");
    prepare_context_ref_ = extract("prepare_context");
    after_ref_ = extract("after_tool_call");
    stop_after_ref_ = extract("should_stop_after_turn");
    command_ref_ = extract("on_command");
    complete_ref_ = extract("complete");
    prompt_line_ref_ = extract("prompt_line");
    status_line_ref_ = extract("status_line");
    tab_title_ref_ = extract("tab_title");

    // Extract commands array (data, not a function)
    lua_getfield(L_, -1, "commands");
    if (lua_istable(L_, -1)) {
      int n = static_cast<int>(lua_rawlen(L_, -1));
      for (int i = 1; i <= n; ++i) {
        lua_rawgeti(L_, -1, i);
        if (lua_istable(L_, -1)) {
          LuaHooks::Command cmd;
          lua_getfield(L_, -1, "name");
          if (lua_isstring(L_, -1) != 0)
            cmd.name = lua_tostring(L_, -1);
          lua_pop(L_, 1);
          lua_getfield(L_, -1, "description");
          if (lua_isstring(L_, -1) != 0)
            cmd.description = lua_tostring(L_, -1);
          lua_pop(L_, 1);
          lua_getfield(L_, -1, "args_hint");
          if (lua_isstring(L_, -1) != 0)
            cmd.args_hint = lua_tostring(L_, -1);
          lua_pop(L_, 1);
          if (!cmd.name.empty())
            commands_.push_back(std::move(cmd));
        }
        lua_pop(L_, 1);
      }
    }
    lua_pop(L_, 1);

    lua_pop(L_, 1); // pop the module table
  }

  ~LuaHooksImpl() {
    if (L_ != nullptr) {
      if (before_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, before_ref_);
      if (on_event_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, on_event_ref_);
      if (prepare_context_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, prepare_context_ref_);
      if (after_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, after_ref_);
      if (stop_after_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, stop_after_ref_);
      if (command_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, command_ref_);
      if (complete_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, complete_ref_);
      if (prompt_line_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, prompt_line_ref_);
      if (status_line_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, status_line_ref_);
      if (tab_title_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, tab_title_ref_);
      lua_close(L_);
    }
  }

  LuaHooksImpl(const LuaHooksImpl &) = delete;
  LuaHooksImpl &operator=(const LuaHooksImpl &) = delete;

  void set_source_path(std::string p) { source_path_ = std::move(p); }
  const std::string &source_path() const { return source_path_; }
  bool has_before() const { return before_ref_ != LUA_NOREF; }
  bool has_on_event() const { return on_event_ref_ != LUA_NOREF; }
  bool has_prepare_context() const {
    return prepare_context_ref_ != LUA_NOREF;
  }
  bool has_after() const { return after_ref_ != LUA_NOREF; }
  bool has_stop_after() const { return stop_after_ref_ != LUA_NOREF; }
  bool has_command() const { return command_ref_ != LUA_NOREF; }
  bool has_complete() const { return complete_ref_ != LUA_NOREF; }
  bool has_prompt_line() const { return prompt_line_ref_ != LUA_NOREF; }
  bool has_status_line() const { return status_line_ref_ != LUA_NOREF; }
  bool has_tab_title() const { return tab_title_ref_ != LUA_NOREF; }

  static void push_usage(lua_State *Ls, const TokenUsage &u) {
    lua_newtable(Ls);
    lua_pushinteger(Ls, static_cast<lua_Integer>(u.input));
    lua_setfield(Ls, -2, "input");
    lua_pushinteger(Ls, static_cast<lua_Integer>(u.output));
    lua_setfield(Ls, -2, "output");
    lua_pushinteger(Ls, static_cast<lua_Integer>(u.cache_read));
    lua_setfield(Ls, -2, "cache_read");
    lua_pushinteger(Ls, static_cast<lua_Integer>(u.cache_write));
    lua_setfield(Ls, -2, "cache_write");
    lua_pushinteger(Ls, static_cast<lua_Integer>(u.total_tokens));
    lua_setfield(Ls, -2, "total_tokens");
    lua_newtable(Ls);
    lua_pushnumber(Ls, u.cost.input);
    lua_setfield(Ls, -2, "input");
    lua_pushnumber(Ls, u.cost.output);
    lua_setfield(Ls, -2, "output");
    lua_pushnumber(Ls, u.cost.cache_read);
    lua_setfield(Ls, -2, "cache_read");
    lua_pushnumber(Ls, u.cost.cache_write);
    lua_setfield(Ls, -2, "cache_write");
    lua_pushnumber(Ls, u.cost.total);
    lua_setfield(Ls, -2, "total");
    lua_setfield(Ls, -2, "cost");
  }

  static void push_ui_context(lua_State *Ls, const LuaUiContext &context) {
    lua_newtable(Ls);
    lua_pushinteger(Ls, static_cast<lua_Integer>(context.turn));
    lua_setfield(Ls, -2, "turn");
    lua_pushlstring(Ls, context.model.data(), context.model.size());
    lua_setfield(Ls, -2, "model");
    lua_pushinteger(Ls, static_cast<lua_Integer>(context.tools));
    lua_setfield(Ls, -2, "tools");
    push_usage(Ls, context.last);
    lua_setfield(Ls, -2, "last");
    push_usage(Ls, context.session);
    lua_setfield(Ls, -2, "session");
    lua_pushlstring(Ls, context.session_id.data(), context.session_id.size());
    lua_setfield(Ls, -2, "session_id");
    if (context.session_name)
      lua_pushlstring(Ls, context.session_name->data(),
                      context.session_name->size());
    else
      lua_pushnil(Ls);
    lua_setfield(Ls, -2, "session_name");
  }

  std::optional<std::string> call_prompt_line(std::size_t turn,
                                              std::string_view model_id,
                                              std::size_t tools_count,
                                              const TokenUsage &last,
                                              const TokenUsage &session) {
    std::scoped_lock lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, prompt_line_ref_);

    lua_newtable(L_);
    lua_pushinteger(L_, static_cast<lua_Integer>(turn));
    lua_setfield(L_, -2, "turn");
    lua_pushlstring(L_, model_id.data(), model_id.size());
    lua_setfield(L_, -2, "model");
    lua_pushinteger(L_, static_cast<lua_Integer>(tools_count));
    lua_setfield(L_, -2, "tools");
    push_usage(L_, last);
    lua_setfield(L_, -2, "last");
    push_usage(L_, session);
    lua_setfield(L_, -2, "session");

    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return std::nullopt;
    }

    std::optional<std::string> result;
    if (lua_isstring(L_, -1) != 0) {
      std::string s = lua_tostring(L_, -1);
      if (!s.empty())
        result = std::move(s);
    }
    lua_pop(L_, 1);
    return result;
  }

  std::optional<std::string> call_ui_line(int ref,
                                          const LuaUiContext &context) {
    std::scoped_lock lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    push_ui_context(L_, context);

    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return std::nullopt;
    }

    std::optional<std::string> result;
    if (lua_isstring(L_, -1) != 0)
      result = lua_tostring(L_, -1);
    lua_pop(L_, 1);
    return result;
  }

  std::optional<std::string> call_status_line(const LuaUiContext &context) {
    return call_ui_line(status_line_ref_, context);
  }

  std::optional<std::string> call_tab_title(const LuaUiContext &context) {
    return call_ui_line(tab_title_ref_, context);
  }

  void call_on_event(const AgentEvent &event) {
    std::scoped_lock lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, on_event_ref_);
    json_to_lua(L_, event_to_json(event));
    if (lua_pcall(L_, 1, 0, 0) != LUA_OK)
      lua_pop(L_, 1);
  }

  std::optional<std::vector<Message>>
  call_prepare_context(const AgentContext &context,
                       std::size_t estimated_tokens,
                       std::stop_token stop_tok) {
    std::scoped_lock lk(mutex_);
    if (stop_tok.stop_requested())
      return std::nullopt;

    lua_rawgeti(L_, LUA_REGISTRYINDEX, prepare_context_ref_);
    push_agent_context_to_lua(L_, context);
    lua_pushinteger(L_, static_cast<lua_Integer>(estimated_tokens));
    lua_setfield(L_, -2, "estimated_tokens");
    lua_pushinteger(L_, static_cast<lua_Integer>(context.model.context_window));
    lua_setfield(L_, -2, "context_window");

    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return std::nullopt;
    }
    if (lua_isnil(L_, -1) != 0) {
      lua_pop(L_, 1);
      return std::nullopt;
    }

    std::optional<std::vector<Message>> result;
    if (lua_istable(L_, -1)) {
      lua_getfield(L_, -1, "messages");
      const auto messages_json = lua_to_json(L_, -1);
      lua_pop(L_, 1);
      if (messages_json.is_array()) {
        std::vector<Message> messages;
        bool valid = true;
        for (const auto &message_json : messages_json) {
          auto message = json::from_json(message_json.dump());
          if (!message) {
            valid = false;
            break;
          }
          messages.push_back(std::move(*message));
        }
        if (valid)
          result = std::move(messages);
      }
    }
    lua_pop(L_, 1);
    return result;
  }

  const std::vector<LuaHooks::Command> &commands() const { return commands_; }

  std::optional<BeforeToolCallResult>
  call_before(const BeforeToolCallContext &ctx) {
    std::scoped_lock lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, before_ref_);

    // Build context table
    lua_newtable(L_);
    lua_pushstring(L_, ctx.tool_call.name.c_str());
    lua_setfield(L_, -2, "tool_name");
    lua_pushstring(L_, ctx.tool_call.id.c_str());
    lua_setfield(L_, -2, "call_id");
    json_to_lua(L_, ctx.tool_call.arguments);
    lua_setfield(L_, -2, "args");
    lua_pushinteger(
        L_, static_cast<lua_Integer>(count_turns(ctx.context.messages)));
    lua_setfield(L_, -2, "turn");

    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
      std::string error = lua_tostring(L_, -1);
      lua_pop(L_, 1);
      return BeforeToolCallResult{
          .block = true,
          .reason = "before_tool_call hook failed: " + error};
    }

    std::optional<BeforeToolCallResult> result;
    if (lua_istable(L_, -1)) {
      BeforeToolCallResult r;
      lua_getfield(L_, -1, "block");
      r.block = lua_toboolean(L_, -1) != 0;
      lua_pop(L_, 1);
      lua_getfield(L_, -1, "reason");
      if (lua_isstring(L_, -1) != 0)
        r.reason = lua_tostring(L_, -1);
      lua_pop(L_, 1);
      result = std::move(r);
    }
    lua_pop(L_, 1);
    return result;
  }

  std::optional<AfterToolCallResult>
  call_after(const AfterToolCallContext &ctx) {
    std::scoped_lock lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, after_ref_);

    // Collect text content from result
    std::string content_str =
        ctx.result ? ctx.result->content() : std::string{};

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
    lua_pushinteger(
        L_, static_cast<lua_Integer>(count_turns(ctx.context.messages)));
    lua_setfield(L_, -2, "turn");

    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return std::nullopt;
    }

    std::optional<AfterToolCallResult> result;
    if (lua_istable(L_, -1)) {
      AfterToolCallResult r;
      lua_getfield(L_, -1, "terminate");
      if (lua_toboolean(L_, -1) != 0)
        r.terminate = true;
      lua_pop(L_, 1);
      lua_getfield(L_, -1, "content");
      if (lua_isstring(L_, -1) != 0) {
        r.content = std::vector<ToolResultContentBlock>{
            TextContent{.text = std::string(lua_tostring(L_, -1))}};
      }
      lua_pop(L_, 1);
      lua_getfield(L_, -1, "is_error");
      if (!lua_isnil(L_, -1))
        r.is_error = lua_toboolean(L_, -1) != 0;
      lua_pop(L_, 1);
      result = std::move(r);
    }
    lua_pop(L_, 1);
    return result;
  }

  bool call_stop_after(const Message &msg,
                       const std::vector<ToolResultMessage> &tool_results,
                       const AgentContext &) {
    std::scoped_lock lk(mutex_);
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
    if (storage_path_.empty())
      return;
    std::ifstream f(storage_path_);
    if (!f) {
      storage_ = nlohmann::json::object();
      return;
    }
    storage_ = nlohmann::json::parse(f, nullptr, false);
    if (storage_.is_discarded())
      storage_ = nlohmann::json::object();
  }

  void save_storage() {
    if (storage_path_.empty())
      return;
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
      if (i > 1)
        std::cerr << '\t';
      std::cerr << luaL_tolstring(L, i, nullptr);
      lua_pop(L, 1);
    }
    std::cerr << '\n';
    return 0;
  }

  static int lua_pici_run_agent(lua_State *L) {
    auto *impl = impl_from(L);
    // run_agent_fn_ set once before calls — no lock (would deadlock from
    // on_command)
    const LuaHooks::RunAgentFn &fn = impl->run_agent_fn_;
    if (!fn) {
      lua_pushnil(L);
      lua_pushstring(L, "pici.run_agent not available");
      return 2;
    }
    LuaHooks::AgentRunConfig cfg;
    if (lua_istable(L, 1)) {
      auto sfield = [&](const char *k) -> std::string {
        lua_getfield(L, 1, k);
        std::string s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
        lua_pop(L, 1);
        return s;
      };
      cfg.prompt = sfield("prompt");
      auto sp = sfield("system_prompt");
      if (!sp.empty())
        cfg.system_prompt = sp;
      auto mid = sfield("model");
      if (!mid.empty())
        cfg.model_id = mid;
      lua_getfield(L, 1, "fork_at");
      if (lua_isinteger(L, -1) != 0) {
        auto n = lua_tointeger(L, -1);
        if (n > 0)
          cfg.fork_at = static_cast<std::size_t>(n);
      }
      lua_pop(L, 1);
      lua_getfield(L, 1, "tools");
      if (lua_istable(L, -1)) {
        int n = static_cast<int>(lua_rawlen(L, -1));
        for (int i = 1; i <= n; ++i) {
          lua_rawgeti(L, -1, i);
          if (lua_isstring(L, -1) != 0)
            cfg.tools.emplace_back(lua_tostring(L, -1));
          lua_pop(L, 1);
        }
      }
      lua_pop(L, 1);
    }
    if (cfg.prompt.empty()) {
      lua_pushnil(L);
      lua_pushstring(L, "pici.run_agent: prompt is required");
      return 2;
    }
    auto result = fn(cfg);
    lua_newtable(L);
    lua_pushstring(L, result.text.c_str());
    lua_setfield(L, -2, "text");
    if (result.error)
      lua_pushstring(L, result.error->c_str());
    else
      lua_pushnil(L);
    lua_setfield(L, -2, "error");
    return 1;
  }

  // Storage closures are only ever called from within a Lua pcall, which means
  // the outer hook-call mutex is already held by this thread — do not re-lock.
  static int lua_storage_get(lua_State *L) {
    auto *impl = impl_from(L);
    const char *key = luaL_checkstring(L, 1);
    if (!impl->storage_.contains(key)) {
      lua_pushnil(L);
      return 1;
    }
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
    std::scoped_lock lk(mutex_);
    run_agent_fn_ = info.run_agent;
    storage_path_ = info.storage_path;
    load_storage();

    lua_getglobal(L_, "pici");

    // pici.model → {id, provider, api}
    lua_newtable(L_);
    lua_pushstring(L_, info.model_id.c_str());
    lua_setfield(L_, -2, "id");
    lua_pushstring(L_, info.model_provider.c_str());
    lua_setfield(L_, -2, "provider");
    lua_pushstring(L_, info.model_api.c_str());
    lua_setfield(L_, -2, "api");
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

    lua_pop(L_, 1); // pop pici
  }

  std::vector<std::string>
  call_complete(std::string_view partial,
                const std::vector<Message> &transcript) {
    std::scoped_lock lk(mutex_);
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
        if (lua_isstring(L_, -1) != 0)
          result.emplace_back(lua_tostring(L_, -1));
        lua_pop(L_, 1);
      }
    }
    lua_pop(L_, 1);
    return result;
  }

  // ── Inline tool support ──────────────────────────────────────────────

  static int lua_tool_update(lua_State *L) {
    auto *callback = static_cast<ToolUpdateCallback *>(
        lua_touserdata(L, lua_upvalueindex(1)));
    if (callback == nullptr || !*callback)
      return 0;

    auto value = lua_to_json(L, 1);
    std::string content;
    bool is_error = false;
    if (value.is_string()) {
      content = value.get<std::string>();
    } else if (value.is_object() &&
               value.value("content", nlohmann::json{}).is_string()) {
      content = value["content"].get<std::string>();
      is_error = value.value("is_error", false);
    } else {
      content = value.dump();
    }
    try {
      (*callback)(
          std::make_shared<LuaToolResult>(std::move(content), is_error));
    } catch (...) {
      // Never let a C++ observer exception cross the Lua C ABI boundary.
    }
    return 0;
  }

  std::shared_ptr<ToolResult>
  execute_inline_tool(int ref, std::string_view args_json,
                      ToolUpdateCallback on_update) {
    std::scoped_lock lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    auto args = nlohmann::json::parse(args_json, nullptr, false);
    if (args.is_discarded() || !args.is_object())
      args = nlohmann::json::object();
    json_to_lua(L_, args);
    lua_newtable(L_);
    lua_pushlightuserdata(L_, &on_update);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_tool_update, 1);
    lua_setfield(L_, -2, "update");
    if (lua_pcall(L_, 2, 1, 0) != LUA_OK) {
      std::string err = lua_tostring(L_, -1);
      lua_pop(L_, 1);
      return std::make_shared<LuaToolResult>(std::move(err), true);
    }
    std::shared_ptr<ToolResult> result;
    if (lua_isstring(L_, -1) != 0) {
      result = std::make_shared<LuaToolResult>(
          std::string(lua_tostring(L_, -1)), false);
    } else if (lua_istable(L_, -1)) {
      lua_getfield(L_, -1, "content");
      std::string content =
          (lua_isstring(L_, -1) != 0) ? lua_tostring(L_, -1) : std::string{};
      lua_pop(L_, 1);
      lua_getfield(L_, -1, "is_error");
      bool is_err = lua_toboolean(L_, -1) != 0;
      lua_pop(L_, 1);
      result = std::make_shared<LuaToolResult>(std::move(content), is_err);
    } else {
      result = std::make_shared<LuaToolResult>(std::string{}, false);
    }
    lua_pop(L_, 1);
    return result;
  }

  void unref_tool(int ref) {
    std::scoped_lock lk(mutex_);
    if (L_ != nullptr)
      luaL_unref(L_, LUA_REGISTRYINDEX, ref);
  }

  // Phase 1: called from lua_pici_add_tool during construction.
  // Stores specs; InlineLuaTool objects created in finalize_inline_tools().
  struct PendingTool {
    int exec_ref;
    std::string name, description, schema;
  };

  void add_pending_tool(PendingTool spec) {
    pending_tools_.push_back(std::move(spec));
  }

  // Phase 2: called from load_lua_hooks after make_shared returns.
  void finalize_inline_tools() {
    for (auto &spec : pending_tools_) {
      auto tool = std::make_shared<InlineLuaTool>(
          shared_from_this(), spec.exec_ref, std::move(spec.name),
          std::move(spec.description), std::move(spec.schema), source_path_);
      inline_tools_.push_back(std::move(tool));
    }
    pending_tools_.clear();
  }

  const std::vector<std::shared_ptr<const ToolDefinition>> &
  inline_tools() const {
    return inline_tools_;
  }

  static int lua_pici_add_tool(lua_State *L) {
    auto *impl = impl_from(L);
    if (!lua_istable(L, 1)) {
      lua_pushboolean(L, 0);
      lua_pushstring(L, "pici.add_tool: expected a table");
      return 2;
    }
    auto sfield = [&](const char *k) {
      lua_getfield(L, 1, k);
      std::string s = lua_isstring(L, -1) ? lua_tostring(L, -1) : "";
      lua_pop(L, 1);
      return s;
    };
    std::string name = sfield("name");
    std::string desc = sfield("description");
    std::string schema = sfield("schema");
    if (schema.empty())
      schema = R"({"type":"object","additionalProperties":true})";

    lua_getfield(L, 1, "execute");
    if (!lua_isfunction(L, -1)) {
      lua_pop(L, 1);
      lua_pushboolean(L, 0);
      lua_pushstring(L, "pici.add_tool: execute must be a function");
      return 2;
    }
    int exec_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (name.empty()) {
      luaL_unref(L, LUA_REGISTRYINDEX, exec_ref);
      lua_pushboolean(L, 0);
      lua_pushstring(L, "pici.add_tool: name is required");
      return 2;
    }
    impl->add_pending_tool({.exec_ref = exec_ref,
                            .name = std::move(name),
                            .description = std::move(desc),
                            .schema = std::move(schema)});
    lua_pushboolean(L, 1);
    return 1;
  }

  LuaHooks::CommandResult
  call_on_command(std::string_view cmd, std::string_view args,
                  const std::vector<Message> &transcript,
                  const LuaContextSnapshot &context) {
    std::scoped_lock lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, command_ref_);
    lua_pushlstring(L_, cmd.data(), cmd.size());
    lua_pushlstring(L_, args.data(), args.size());
    push_messages_to_lua(L_, transcript);
    push_context_snapshot_to_lua(L_, context);

    if (lua_pcall(L_, 4, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return {};
    }

    LuaHooks::CommandResult result;
    if (lua_istable(L_, -1)) {
      lua_getfield(L_, -1, "handled");
      result.handled = lua_toboolean(L_, -1) != 0;
      lua_pop(L_, 1);

      lua_getfield(L_, -1, "truncate_to");
      if (lua_isinteger(L_, -1) != 0) {
        auto n = lua_tointeger(L_, -1);
        if (n > 0)
          result.truncate_to = static_cast<std::size_t>(n);
      }
      lua_pop(L_, 1);

      lua_getfield(L_, -1, "prompt");
      if (lua_isstring(L_, -1) != 0)
        result.prompt = lua_tostring(L_, -1);
      lua_pop(L_, 1);

      lua_getfield(L_, -1, "output");
      if (lua_isstring(L_, -1) != 0)
        result.output = lua_tostring(L_, -1);
      lua_pop(L_, 1);
    }
    lua_pop(L_, 1);
    return result;
  }

private:
  lua_State *L_{nullptr};
  int before_ref_{LUA_NOREF};
  int on_event_ref_{LUA_NOREF};
  int prepare_context_ref_{LUA_NOREF};
  int after_ref_{LUA_NOREF};
  int stop_after_ref_{LUA_NOREF};
  int command_ref_{LUA_NOREF};
  int complete_ref_{LUA_NOREF};
  int prompt_line_ref_{LUA_NOREF};
  int status_line_ref_{LUA_NOREF};
  int tab_title_ref_{LUA_NOREF};
  std::string source_path_;
  std::vector<PendingTool> pending_tools_;
  std::vector<std::shared_ptr<const ToolDefinition>> inline_tools_;
  std::vector<LuaHooks::Command> commands_;
  LuaHooks::RunAgentFn run_agent_fn_;
  nlohmann::json storage_{nlohmann::json::object()};
  std::filesystem::path storage_path_;
  mutable std::mutex mutex_;
};

InlineLuaTool::~InlineLuaTool() {
  if (impl_ && exec_ref_ != LUA_NOREF)
    impl_->unref_tool(exec_ref_);
}

std::shared_ptr<ToolResult> InlineLuaTool::execute(std::string_view,
                                                   std::string_view args_json,
                                                   std::stop_token,
                                                   ToolUpdateCallback on_update) const {
  return impl_->execute_inline_tool(exec_ref_, args_json,
                                    std::move(on_update));
}

} // namespace

std::shared_ptr<const ToolDefinition>
load_lua_tool(const std::filesystem::path &path) {
  return std::make_shared<LuaTool>(path);
}

std::vector<std::shared_ptr<const ToolDefinition>>
load_lua_tools(const std::filesystem::path &directory) {
  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
    if (!entry.is_regular_file(ec)) {
      continue;
    }
    if (entry.path().extension() != ".lua") {
      continue;
    }
    try {
      tools.push_back(load_lua_tool(entry.path()));
    } catch (const std::exception &) { // NOLINT(bugprone-empty-catch)
      // skip tools that fail to load
    }
  }
  return tools;
}

std::shared_ptr<LuaHooks> load_lua_hooks(const std::filesystem::path &path) {
  auto impl = std::make_shared<LuaHooksImpl>(path);
  auto hooks = std::make_shared<LuaHooks>();

  if (impl->has_before()) {
    hooks->before_tool_call =
        [impl](const BeforeToolCallContext &ctx,
               const std::stop_token &) -> std::optional<BeforeToolCallResult> {
      return impl->call_before(ctx);
    };
  }
  if (impl->has_on_event()) {
    hooks->on_event = [impl](const AgentEvent &event) {
      impl->call_on_event(event);
    };
  }
  if (impl->has_prepare_context()) {
    hooks->prepare_context =
        [impl](const AgentContext &context, std::size_t estimated_tokens,
               std::stop_token stop_tok) {
          return impl->call_prepare_context(context, estimated_tokens,
                                            std::move(stop_tok));
        };
  }
  if (impl->has_after()) {
    hooks->after_tool_call =
        [impl](const AfterToolCallContext &ctx,
               const std::stop_token &) -> std::optional<AfterToolCallResult> {
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
               const std::vector<Message> &transcript,
               const LuaContextSnapshot &context) -> LuaHooks::CommandResult {
      return impl->call_on_command(cmd, args, transcript, context);
    };
  }
  hooks->configure = [impl](const LuaHooks::AgentInfo &info) {
    impl->configure_info(info);
  };
  impl->set_source_path(path.string());
  impl->finalize_inline_tools(); // needs shared_ptr; safe here post-make_shared
  hooks->source_path = impl->source_path();
  hooks->commands = impl->commands();
  hooks->registered_tools = impl->inline_tools();
  if (impl->has_complete()) {
    hooks->complete = [impl](std::string_view partial,
                             const std::vector<Message> &transcript)
        -> std::vector<std::string> {
      return impl->call_complete(partial, transcript);
    };
  }
  if (impl->has_prompt_line()) {
    hooks->prompt_line =
        [impl](std::size_t turn, std::string_view model_id,
               std::size_t tools_count, const TokenUsage &last,
               const TokenUsage &session) -> std::optional<std::string> {
      return impl->call_prompt_line(turn, model_id, tools_count, last, session);
    };
  }
  if (impl->has_status_line()) {
    hooks->status_line =
        [impl](const LuaUiContext &context) -> std::optional<std::string> {
      return impl->call_status_line(context);
    };
  }
  if (impl->has_tab_title()) {
    hooks->tab_title =
        [impl](const LuaUiContext &context) -> std::optional<std::string> {
      return impl->call_tab_title(context);
    };
  }
  return hooks;
}

// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
static constexpr char kPiciTestLua[] = R"lua(
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
  if (L == nullptr)
    throw std::runtime_error("Failed to create Lua state for test runner");
  luaL_openlibs(L);
  register_json_module(L);

  // Minimal pici global: log + stub run_agent (overridden by mock_run_agent)
  lua_newtable(L);
  lua_pushcfunction(L, [](lua_State *l) -> int {
    int n = lua_gettop(l);
    for (int i = 1; i <= n; ++i) {
      if (i > 1)
        std::cerr << '\t';
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
    r.total = static_cast<int>(lua_tointeger(L, -1));
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
      std::cerr << "warning: skipping hooks file " << entry.path() << ": "
                << e.what() << "\n";
    }
  }
  return compose_hooks(std::move(list));
}

std::shared_ptr<LuaHooks>
compose_hooks(std::vector<std::shared_ptr<LuaHooks>> list) {
  // Drop nulls
  auto removed = std::ranges::remove_if(list, [](const auto &h) { return !h; });
  list.erase(removed.begin(), removed.end());
  if (list.empty())
    return nullptr;
  if (list.size() == 1)
    return list[0];

  auto out = std::make_shared<LuaHooks>();

  // before_tool_call — run all; first block wins
  if (std::ranges::any_of(
          list, [](const auto &h) { return !!h->before_tool_call; })) {
    out->before_tool_call =
        [list](
            const BeforeToolCallContext &ctx,
            const std::stop_token &st) -> std::optional<BeforeToolCallResult> {
      for (const auto &h : list) {
        if (!h->before_tool_call)
          continue;
        auto r = h->before_tool_call(ctx, st);
        if (r && r->block)
          return r;
      }
      return std::nullopt;
    };
  }

  // on_event — notify all observers in load order.
  if (std::ranges::any_of(list, [](const auto &h) { return !!h->on_event; })) {
    out->on_event = [list](const AgentEvent &event) {
      for (const auto &h : list) {
        if (h->on_event)
          h->on_event(event);
      }
    };
  }

  // prepare_context — first non-null replacement wins.
  if (std::ranges::any_of(
          list, [](const auto &h) { return !!h->prepare_context; })) {
    out->prepare_context =
        [list](const AgentContext &context, std::size_t estimated_tokens,
               std::stop_token stop_tok)
        -> std::optional<std::vector<Message>> {
      for (const auto &h : list) {
        if (!h->prepare_context)
          continue;
        if (auto result = h->prepare_context(context, estimated_tokens,
                                             stop_tok))
          return result;
      }
      return std::nullopt;
    };
  }

  // after_tool_call — run all; first non-null wins
  if (std::ranges::any_of(list,
                          [](const auto &h) { return !!h->after_tool_call; })) {
    out->after_tool_call =
        [list](
            const AfterToolCallContext &ctx,
            const std::stop_token &st) -> std::optional<AfterToolCallResult> {
      for (const auto &h : list) {
        if (!h->after_tool_call)
          continue;
        auto r = h->after_tool_call(ctx, st);
        if (r)
          return r;
      }
      return std::nullopt;
    };
  }

  // should_stop_after_turn — OR
  if (std::ranges::any_of(
          list, [](const auto &h) { return !!h->should_stop_after_turn; })) {
    out->should_stop_after_turn =
        [list](const Message &msg,
               const std::vector<ToolResultMessage> &results,
               const AgentContext &ctx) -> bool {
      return std::ranges::any_of(list, [&](const auto &h) {
        return h->should_stop_after_turn &&
               h->should_stop_after_turn(msg, results, ctx);
      });
    };
  }

  // on_command — first handled wins
  if (std::ranges::any_of(list,
                          [](const auto &h) { return !!h->on_command; })) {
    out->on_command =
        [list](std::string_view cmd, std::string_view args,
               const std::vector<Message> &transcript,
               const LuaContextSnapshot &context) -> LuaHooks::CommandResult {
      for (const auto &h : list) {
        if (!h->on_command)
          continue;
        auto r = h->on_command(cmd, args, transcript, context);
        if (r.handled)
          return r;
      }
      return {};
    };
  }

  // commands + registered_tools — union
  for (const auto &h : list) {
    out->commands.insert(out->commands.end(), h->commands.begin(),
                         h->commands.end());
    out->registered_tools.insert(out->registered_tools.end(),
                                 h->registered_tools.begin(),
                                 h->registered_tools.end());
  }

  // configure — forward to all
  out->configure = [list](const LuaHooks::AgentInfo &info) {
    for (const auto &h : list)
      if (h->configure)
        h->configure(info);
  };

  // prompt_line — last non-nil wins (override semantics)
  if (std::ranges::any_of(list,
                          [](const auto &h) { return !!h->prompt_line; })) {
    out->prompt_line =
        [list](std::size_t turn, std::string_view model_id,
               std::size_t tools_count, const TokenUsage &last,
               const TokenUsage &session) -> std::optional<std::string> {
      std::optional<std::string> result;
      for (const auto &h : list) {
        if (!h->prompt_line)
          continue;
        auto r = h->prompt_line(turn, model_id, tools_count, last, session);
        if (r)
          result = std::move(r);
      }
      return result;
    };
  }

  // status_line and tab_title — last non-nil wins (override semantics)
  if (std::ranges::any_of(list,
                          [](const auto &h) { return !!h->status_line; })) {
    out->status_line =
        [list](const LuaUiContext &context) -> std::optional<std::string> {
      std::optional<std::string> result;
      for (const auto &h : list) {
        if (!h->status_line)
          continue;
        auto r = h->status_line(context);
        if (r)
          result = std::move(r);
      }
      return result;
    };
  }
  if (std::ranges::any_of(list, [](const auto &h) { return !!h->tab_title; })) {
    out->tab_title =
        [list](const LuaUiContext &context) -> std::optional<std::string> {
      std::optional<std::string> result;
      for (const auto &h : list) {
        if (!h->tab_title)
          continue;
        auto r = h->tab_title(context);
        if (r)
          result = std::move(r);
      }
      return result;
    };
  }

  // complete — union of all results
  if (std::ranges::any_of(list, [](const auto &h) { return !!h->complete; })) {
    out->complete = [list](std::string_view partial,
                           const std::vector<Message> &transcript)
        -> std::vector<std::string> {
      std::vector<std::string> result;
      for (const auto &h : list) {
        if (!h->complete)
          continue;
        auto r = h->complete(partial, transcript);
        result.insert(result.end(), r.begin(), r.end());
      }
      return result;
    };
  }

  return out;
}

} // namespace pi::core
