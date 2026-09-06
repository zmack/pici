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
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "core/agent_runtime_identity.h"
#include "core/builtin_tools.h"
#include "core/event_json.h"
#include "core/event_types.h"
#include "core/lua_eval_tool.h"
#include "core/message_types.h"
#include "core/terminal.h"

namespace pi::core {
namespace {

// Shared recursion-depth cap for the Lua<->JSON conversion helpers below.
// The review that drove this cap found two real process crashes in code
// with no depth limit at all: a 200,000-deep nested Lua table (only ~14MB
// of Lua memory -- well under any memory cap) stack-overflows the process
// via ~200,000 C++ stack frames. 200 is deep enough for any normal
// tool/add-on payload while keeping the C++ call stack shallow.
constexpr int kLuaJsonMaxDepth = 200;

// Lua only guarantees LUA_MINSTACK (20) free stack slots to a freshly
// entered C function; it does NOT grow the stack automatically as a C
// function keeps pushing values across its own recursive calls. Each
// recursion level below holds a small, bounded number of values live on
// the Lua stack across its own recursive call (the table/key being built,
// the value fetched via lua_next/lua_rawgeti) -- empirically confirmed
// (via a standalone reproduction against this exact Lua runtime) that
// omitting this check corrupts the Lua stack and crashes the process via
// SIGSEGV at only ~35-40 levels of nesting, well before the depth cap
// above would ever engage. luaL_checkstack grows the stack on demand (or
// raises a clean, catchable Lua error if a hard limit is hit) -- calling
// it once per recursion level keeps arbitrarily deep (up to the cap)
// recursion safe.
constexpr int kLuaJsonStackHeadroom = 4;

void json_to_lua(lua_State *L, const nlohmann::json &j, int depth);

// json_to_lua has a depth cap but deliberately NOT a cycle guard: unlike
// the Lua-to-JSON direction below, its input is always an nlohmann::json
// tree built by parsing text (lua_json_decode) or by explicit construction
// elsewhere in this codebase -- never from a live object graph with
// back-references -- so it cannot contain a cycle. Adding cycle detection
// here would guard against an input shape this function can never receive.
// The depth cap alone still protects against a pathologically deep (but
// acyclic) JSON document.
// NOLINTNEXTLINE(misc-no-recursion)
void json_to_lua(lua_State *L, const nlohmann::json &j, int depth) {
  if (depth >= kLuaJsonMaxDepth) {
    // Past the cap: return a clean nil rather than recursing further.
    lua_pushnil(L);
    return;
  }
  // See kLuaJsonStackHeadroom's comment above: without this, a deeply
  // nested (but well under the depth cap) document can still corrupt the
  // Lua stack and crash the process, because Lua doesn't grow the stack
  // automatically as a C function recurses.
  luaL_checkstack(L, kLuaJsonStackHeadroom,
                  "pici: JSON document too deeply nested to decode");
  if (j.is_object()) {
    lua_newtable(L);
    for (const auto &[key, val] : j.items()) {
      lua_pushstring(L, key.c_str());
      json_to_lua(L, val, depth + 1);
      lua_rawset(L, -3);
    }
  } else if (j.is_array()) {
    lua_newtable(L);
    for (std::size_t i = 0; i < j.size(); ++i) {
      json_to_lua(L, j[i], depth + 1);
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

// Public entry point used throughout this file; starts the depth counter
// at 0. Behavior for any non-pathological input is unchanged from before
// the depth cap was added.
void json_to_lua(lua_State *L, const nlohmann::json &j) {
  json_to_lua(L, j, 0);
}

nlohmann::json lua_to_json(lua_State *L, int idx, int depth,
                           std::unordered_set<const void *> &visited_tables);

nlohmann::json
lua_table_to_json(lua_State *L, int idx, int depth, // NOLINT(misc-no-recursion)
                  std::unordered_set<const void *> &visited_tables) {
  if (idx < 0) {
    idx = lua_gettop(L) + idx + 1;
  }
  if (depth >= kLuaJsonMaxDepth) {
    // Depth cap: e.g. `local t={} for i=1,200000 do t={t} end` is only
    // ~14MB of Lua memory (well under any memory cap) but would otherwise
    // recurse ~200,000 C++ stack frames and crash the process with a stack
    // overflow. Returning null here is consistent with how this function
    // already maps other can't-represent-as-JSON values (e.g. functions,
    // userdata) to null in lua_to_json below.
    return nullptr;
  }
  // See kLuaJsonStackHeadroom's comment above: without this, a deeply
  // nested (but well under the depth cap) table can still corrupt the Lua
  // stack and crash the process, because Lua doesn't grow the stack
  // automatically as a C function recurses.
  luaL_checkstack(L, kLuaJsonStackHeadroom,
                  "pici: table too deeply nested to convert to JSON");
  // Cycle guard: a table's identity is stable for the lifetime of this
  // call via lua_topointer. `visited_tables` tracks tables currently on
  // the recursion path (ancestors), not every table ever seen -- the same
  // table legitimately appearing twice as unrelated sibling values (a DAG,
  // not a cycle) must not be flagged. Erasing on scope exit (below) keeps
  // the set scoped to "currently an ancestor."
  const void *table_ptr = lua_topointer(L, idx);
  const bool track_identity = table_ptr != nullptr;
  if (track_identity && !visited_tables.insert(table_ptr).second) {
    // This table is already an ancestor of itself on the current
    // recursion path (e.g. `local t={}; t.x=t`) -- without this check,
    // converting it would recurse forever and SIGSEGV. Return a clean
    // null instead.
    return nullptr;
  }
  struct VisitedTableGuard {
    std::unordered_set<const void *> &visited;
    const void *ptr;
    bool active;
    VisitedTableGuard(std::unordered_set<const void *> &visited_in,
                      const void *ptr_in, bool active_in)
        : visited(visited_in), ptr(ptr_in), active(active_in) {}
    ~VisitedTableGuard() {
      if (active)
        visited.erase(ptr);
    }
    VisitedTableGuard(const VisitedTableGuard &) = delete;
    VisitedTableGuard &operator=(const VisitedTableGuard &) = delete;
    VisitedTableGuard(VisitedTableGuard &&) = delete;
    VisitedTableGuard &operator=(VisitedTableGuard &&) = delete;
  } guard(visited_tables, table_ptr, track_identity);

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
      arr.push_back(lua_to_json(L, lua_gettop(L), depth + 1, visited_tables));
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
      obj[key] = lua_to_json(L, lua_gettop(L), depth + 1, visited_tables);
    }
    lua_pop(L, 1);
  }
  return obj;
}

// NOLINTNEXTLINE(misc-no-recursion)
nlohmann::json lua_to_json(lua_State *L, int idx, int depth,
                           std::unordered_set<const void *> &visited_tables) {
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
    return lua_table_to_json(L, idx, depth, visited_tables);
  default:
    return {nullptr};
  }
}

// Public entry point used throughout this file; starts the depth counter
// at 0 and owns the per-call visited-table set. Behavior for any
// non-pathological input is unchanged from before the depth/cycle guard
// was added.
nlohmann::json lua_to_json(lua_State *L, int idx) {
  std::unordered_set<const void *> visited_tables;
  return lua_to_json(L, idx, 0, visited_tables);
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

int lua_pici_truncate(lua_State *L) {
  std::size_t len = 0;
  const char *s = luaL_checklstring(L, 1, &len);
  auto out = truncate_tool_result(std::string_view(s, len));
  lua_pushlstring(L, out.data(), out.size());
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

// The following seven helpers were relocated here (pure move, no behavior
// change) from LuaHooksImpl's private static section: none of them read or
// write LuaHooksImpl instance state -- only impl_from(L) recovers the owning
// LuaHooksImpl*, and that stays a LuaHooksImpl static method. Un-coupling
// these from LuaHooksImpl lets a later eval-tool trampoline reuse them
// without needing a LuaHooksImpl instance at all.

// `domain` names the pici.<domain> table this binding belongs to (e.g.
// "agents", "session") -- used to build both the "not available" message
// and the generic error fallback so callers under pici.session.* don't
// misleadingly say "agent task error".
int push_agent_binding(
    lua_State *L,
    const std::function<nlohmann::json(const nlohmann::json &)> &fn,
    const nlohmann::json &args, std::string_view domain) {
  if (!fn) {
    lua_pushnil(L);
    const auto message = "pici." + std::string(domain) + " is not available";
    lua_pushstring(L, message.c_str());
    return 2;
  }
  try {
    auto result = fn(args.is_object() ? args : nlohmann::json::object());
    if (result.contains("error")) {
      lua_pushnil(L);
      const auto &error = result["error"];
      const auto fallback = std::string(domain) + " action error";
      const auto message = error.is_object() ? error.value("message", fallback)
                                             : error.get<std::string>();
      lua_pushstring(L, message.c_str());
      return 2;
    }
    json_to_lua(L, result);
    return 1;
  } catch (const std::exception &e) {
    lua_pushnil(L);
    lua_pushstring(L, e.what());
    return 2;
  }
}

nlohmann::json agent_args(lua_State *L) {
  if (lua_istable(L, 1))
    return lua_to_json(L, 1);
  nlohmann::json args = nlohmann::json::object();
  if (lua_isstring(L, 1) != 0)
    args["target"] = lua_tostring(L, 1);
  if (lua_isstring(L, 2) != 0)
    args["message"] = lua_tostring(L, 2);
  if (lua_isstring(L, 3) != 0)
    args["reason"] = lua_tostring(L, 3);
  return args;
}

std::stop_token mailbox_stop_token(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "pici.inline_stop_token");
  auto *token = static_cast<std::stop_token *>(lua_touserdata(L, -1));
  const auto result = token != nullptr ? *token : std::stop_token{};
  lua_pop(L, 1);
  return result;
}

std::optional<AgentRuntimeIdentity> mailbox_actor(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "pici.inline_actor");
  auto *actor =
      static_cast<std::optional<AgentRuntimeIdentity> *>(lua_touserdata(L, -1));
  const auto result = actor != nullptr ? *actor : std::nullopt;
  lua_pop(L, 1);
  return result;
}

ToolPresentationCallback *mailbox_presentation_callback(lua_State *L) {
  lua_getfield(L, LUA_REGISTRYINDEX, "pici.inline_presentation");
  auto *callback =
      static_cast<ToolPresentationCallback *>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return callback;
}

int push_mailbox_binding(lua_State *L,
                         const LuaHooks::MailboxBindings::Callback &fn,
                         const nlohmann::json &args) {
  if (!fn) {
    lua_pushnil(L);
    lua_pushstring(L, "pici.mailbox is not available");
    return 2;
  }
  try {
    const LuaHooks::MailboxBindings::InvocationContext context{
        mailbox_actor(L), mailbox_stop_token(L),
        mailbox_presentation_callback(L)};
    auto result =
        fn(args.is_object() ? args : nlohmann::json::object(), context);
    if (result.contains("error")) {
      lua_pushnil(L);
      const auto &error = result["error"];
      const auto message = error.is_object()
                               ? error.value("message", "mailbox error")
                               : error.get<std::string>();
      const auto code = error.is_object() ? error.value("code", "mailbox_error")
                                          : std::string("mailbox_error");
      const auto stable_error = code + ": " + message;
      lua_pushstring(L, stable_error.c_str());
      return 2;
    }
    json_to_lua(L, result);
    return 1;
  } catch (const std::exception &e) {
    lua_pushnil(L);
    lua_pushstring(L, e.what());
    return 2;
  }
}

nlohmann::json mailbox_args(lua_State *L) {
  return lua_istable(L, 1) ? lua_to_json(L, 1) : nlohmann::json::object();
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
  auto *callback =
      static_cast<ToolUpdateCallback *>(lua_touserdata(L, lua_upvalueindex(1)));
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
    (*callback)(std::make_shared<LuaToolResult>(std::move(content), is_error));
  } catch (...) {
    static_cast<void>(
        0); // Never let a C++ observer exception cross the Lua C ABI boundary.
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

  std::shared_ptr<ToolResult>
  execute(std::string_view, std::string_view args_json, std::stop_token,
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
          } else if constexpr (std::is_same_v<T, ContextCompactionMessage>) {
            lua_pushstring(L, "contextCompaction");
            lua_setfield(L, -2, "role");
            // Opaque server-encrypted content; never exposed to scripts.
            lua_pushstring(L, "(context compaction)");
            lua_setfield(L, -2, "content");
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

class LuaRegistryPointerGuard {
public:
  static int registry_type(lua_State *state, const char *key) {
    lua_getfield(state, LUA_REGISTRYINDEX, key);
    const auto type = lua_type(state, -1);
    lua_pop(state, 1);
    return type;
  }

  LuaRegistryPointerGuard(lua_State *state, const char *key, void *value)
      : state_(state), key_(key), previous_type_(registry_type(state, key)) {
    lua_getfield(state_, LUA_REGISTRYINDEX, key_);
    if (previous_type_ == LUA_TLIGHTUSERDATA)
      previous_value_ = lua_touserdata(state_, -1);
    lua_pop(state_, 1);
    lua_pushlightuserdata(state_, value);
    lua_setfield(state_, LUA_REGISTRYINDEX, key_);
  }

  ~LuaRegistryPointerGuard() {
    if (previous_type_ == LUA_TLIGHTUSERDATA) {
      lua_pushlightuserdata(state_, previous_value_);
    } else {
      lua_pushnil(state_);
    }
    lua_setfield(state_, LUA_REGISTRYINDEX, key_);
  }

  LuaRegistryPointerGuard(const LuaRegistryPointerGuard &) = delete;
  LuaRegistryPointerGuard &operator=(const LuaRegistryPointerGuard &) = delete;

private:
  lua_State *state_;
  const char *key_;
  int previous_type_{LUA_TNIL};
  void *previous_value_{nullptr};
};

class InlineLuaTool final : public ToolDefinition {
public:
  InlineLuaTool(std::shared_ptr<LuaHooksImpl> impl, int exec_ref,
                std::string name, std::string description,
                std::string schema_str, std::string source, bool child_safe)
      : impl_(std::move(impl)), exec_ref_(exec_ref), name_(std::move(name)),
        description_(std::move(description)), source_(std::move(source)),
        schema_(std::make_unique<LuaToolSchema>(std::move(schema_str))),
        child_safe_(child_safe) {}

  ~InlineLuaTool() override;
  InlineLuaTool(const InlineLuaTool &) = delete;
  InlineLuaTool &operator=(const InlineLuaTool &) = delete;
  InlineLuaTool(InlineLuaTool &&) = delete;
  InlineLuaTool &operator=(InlineLuaTool &&) = delete;

  std::string_view name() const override { return name_; }
  std::string_view description() const override { return description_; }
  std::string_view source_path() const override { return source_; }
  ToolSchema &schema() const override { return *schema_; }
  ToolCapabilities capabilities() const override {
    return {.child_safe = child_safe_};
  }

  std::shared_ptr<ToolResult>
  execute(std::string_view call_id, std::string_view args_json,
          std::stop_token stop_token = {},
          ToolUpdateCallback on_update = {}) const override {
    return execute(args_json,
                   ToolExecutionContext{.call_id = call_id,
                                        .stop_token = stop_token,
                                        .on_update = std::move(on_update)});
  }

  std::shared_ptr<ToolResult>
  execute(std::string_view args_json,
          ToolExecutionContext context) const override;

private:
  std::shared_ptr<LuaHooksImpl> impl_;
  int exec_ref_;
  std::string name_, description_, source_;
  std::unique_ptr<LuaToolSchema> schema_;
  bool child_safe_{false};
};

class LuaHooksImpl : public std::enable_shared_from_this<LuaHooksImpl> {
public:
  explicit LuaHooksImpl(const std::filesystem::path &path,
                        bool bundled_child_safe_tools)
      : L_(luaL_newstate()),
        bundled_child_safe_tools_(bundled_child_safe_tools) {
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
    lua_pushcfunction(L_, &lua_pici_truncate);
    lua_setfield(L_, -2, "truncate_tool_result");

    lua_newtable(L_);
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_agents_spawn, 1);
    lua_setfield(L_, -2, "spawn");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_agents_get, 1);
    lua_setfield(L_, -2, "get");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_agents_list, 1);
    lua_setfield(L_, -2, "list");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_agents_send, 1);
    lua_setfield(L_, -2, "send");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_agents_follow_up, 1);
    lua_setfield(L_, -2, "follow_up");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_agents_interrupt, 1);
    lua_setfield(L_, -2, "interrupt");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_agents_wait, 1);
    lua_setfield(L_, -2, "wait");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_agents_close, 1);
    lua_setfield(L_, -2, "close");
    lua_setfield(L_, -2, "agents");

    lua_newtable(L_);
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_session_request_compaction, 1);
    lua_setfield(L_, -2, "request_compaction");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_session_request_model_switch,
                     1);
    lua_setfield(L_, -2, "request_model_switch");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_session_request_fork, 1);
    lua_setfield(L_, -2, "request_fork");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_session_action_status, 1);
    lua_setfield(L_, -2, "action_status");
    lua_setfield(L_, -2, "session");

    lua_newtable(L_);
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_mailbox_self, 1);
    lua_setfield(L_, -2, "self");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_mailbox_list, 1);
    lua_setfield(L_, -2, "list");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_mailbox_send, 1);
    lua_setfield(L_, -2, "send");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_mailbox_request, 1);
    lua_setfield(L_, -2, "request");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_mailbox_reply, 1);
    lua_setfield(L_, -2, "reply");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_mailbox_inbox, 1);
    lua_setfield(L_, -2, "inbox");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_mailbox_ack, 1);
    lua_setfield(L_, -2, "ack");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_mailbox_wait, 1);
    lua_setfield(L_, -2, "wait");
    lua_pushlightuserdata(L_, this);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_pici_mailbox_status, 1);
    lua_setfield(L_, -2, "status");
    lua_setfield(L_, -2, "mailbox");
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
    format_tool_call_ref_ = extract("format_tool_call");
    format_tool_result_ref_ = extract("format_tool_result");

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
      if (format_tool_call_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, format_tool_call_ref_);
      if (format_tool_result_ref_ != LUA_NOREF)
        luaL_unref(L_, LUA_REGISTRYINDEX, format_tool_result_ref_);
      lua_close(L_);
    }
  }

  LuaHooksImpl(const LuaHooksImpl &) = delete;
  LuaHooksImpl &operator=(const LuaHooksImpl &) = delete;

  void set_source_path(std::string p) { source_path_ = std::move(p); }
  const std::string &source_path() const { return source_path_; }
  bool has_before() const { return before_ref_ != LUA_NOREF; }
  bool has_on_event() const { return on_event_ref_ != LUA_NOREF; }
  bool has_prepare_context() const { return prepare_context_ref_ != LUA_NOREF; }
  bool has_after() const { return after_ref_ != LUA_NOREF; }
  bool has_stop_after() const { return stop_after_ref_ != LUA_NOREF; }
  bool has_command() const { return command_ref_ != LUA_NOREF; }
  bool has_complete() const { return complete_ref_ != LUA_NOREF; }
  bool has_prompt_line() const { return prompt_line_ref_ != LUA_NOREF; }
  bool has_status_line() const { return status_line_ref_ != LUA_NOREF; }
  bool has_tab_title() const { return tab_title_ref_ != LUA_NOREF; }
  bool has_format_tool_call() const {
    return format_tool_call_ref_ != LUA_NOREF;
  }
  bool has_format_tool_result() const {
    return format_tool_result_ref_ != LUA_NOREF;
  }

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

  std::optional<std::string>
  call_format_tool_call(const LuaHooks::FormatToolCallContext &ctx) {
    std::scoped_lock lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, format_tool_call_ref_);
    lua_newtable(L_);
    lua_pushlstring(L_, ctx.tool_name.data(), ctx.tool_name.size());
    lua_setfield(L_, -2, "tool_name");
    lua_pushlstring(L_, ctx.call_id.data(), ctx.call_id.size());
    lua_setfield(L_, -2, "call_id");
    json_to_lua(L_, ctx.args);
    lua_setfield(L_, -2, "args");
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return std::nullopt;
    }
    std::optional<std::string> result;
    if (lua_isstring(L_, -1) != 0)
      result = std::string(lua_tostring(L_, -1));
    lua_pop(L_, 1);
    return result;
  }

  std::optional<std::string>
  call_format_tool_result(const LuaHooks::FormatToolResultContext &ctx) {
    std::scoped_lock lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, format_tool_result_ref_);
    lua_newtable(L_);
    lua_pushlstring(L_, ctx.tool_name.data(), ctx.tool_name.size());
    lua_setfield(L_, -2, "tool_name");
    lua_pushlstring(L_, ctx.call_id.data(), ctx.call_id.size());
    lua_setfield(L_, -2, "call_id");
    json_to_lua(L_, ctx.args);
    lua_setfield(L_, -2, "args");
    lua_pushlstring(L_, ctx.content.data(), ctx.content.size());
    lua_setfield(L_, -2, "content");
    lua_pushboolean(L_, ctx.is_error ? 1 : 0);
    lua_setfield(L_, -2, "is_error");
    if (lua_pcall(L_, 1, 1, 0) != LUA_OK) {
      lua_pop(L_, 1);
      return std::nullopt;
    }
    std::optional<std::string> result;
    if (lua_isstring(L_, -1) != 0)
      result = std::string(lua_tostring(L_, -1));
    lua_pop(L_, 1);
    return result;
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
                       const std::stop_token &stop_tok) {
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
          .block = true, .reason = "before_tool_call hook failed: " + error};
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

  static int lua_pici_mailbox_self(lua_State *L) {
    auto *impl = impl_from(L);
    return push_mailbox_binding(L, impl->mailbox_bindings_.self,
                                mailbox_args(L));
  }

  static int lua_pici_mailbox_list(lua_State *L) {
    auto *impl = impl_from(L);
    return push_mailbox_binding(L, impl->mailbox_bindings_.list,
                                mailbox_args(L));
  }

  static int lua_pici_mailbox_send(lua_State *L) {
    auto *impl = impl_from(L);
    return push_mailbox_binding(L, impl->mailbox_bindings_.send,
                                mailbox_args(L));
  }

  static int lua_pici_mailbox_request(lua_State *L) {
    auto *impl = impl_from(L);
    return push_mailbox_binding(L, impl->mailbox_bindings_.request,
                                mailbox_args(L));
  }

  static int lua_pici_mailbox_reply(lua_State *L) {
    auto *impl = impl_from(L);
    return push_mailbox_binding(L, impl->mailbox_bindings_.reply,
                                mailbox_args(L));
  }

  static int lua_pici_mailbox_inbox(lua_State *L) {
    auto *impl = impl_from(L);
    return push_mailbox_binding(L, impl->mailbox_bindings_.inbox,
                                mailbox_args(L));
  }

  static int lua_pici_mailbox_ack(lua_State *L) {
    auto *impl = impl_from(L);
    return push_mailbox_binding(L, impl->mailbox_bindings_.ack,
                                mailbox_args(L));
  }

  static int lua_pici_mailbox_wait(lua_State *L) {
    auto *impl = impl_from(L);
    return push_mailbox_binding(L, impl->mailbox_bindings_.wait,
                                mailbox_args(L));
  }

  static int lua_pici_mailbox_status(lua_State *L) {
    auto *impl = impl_from(L);
    return push_mailbox_binding(L, impl->mailbox_bindings_.status,
                                mailbox_args(L));
  }

  static int lua_pici_agents_spawn(lua_State *L) {
    auto *impl = impl_from(L);
    return push_agent_binding(L, impl->agent_bindings_.spawn, agent_args(L),
                              "agents");
  }

  static int lua_pici_agents_get(lua_State *L) {
    auto *impl = impl_from(L);
    return push_agent_binding(L, impl->agent_bindings_.get, agent_args(L),
                              "agents");
  }

  static int lua_pici_agents_list(lua_State *L) {
    auto *impl = impl_from(L);
    nlohmann::json args;
    if (lua_isstring(L, 1) != 0)
      args = nlohmann::json{{"path_prefix", lua_tostring(L, 1)}};
    else
      args = agent_args(L);
    return push_agent_binding(L, impl->agent_bindings_.list, args, "agents");
  }

  static int lua_pici_agents_send(lua_State *L) {
    auto *impl = impl_from(L);
    return push_agent_binding(L, impl->agent_bindings_.send_message,
                              agent_args(L), "agents");
  }

  static int lua_pici_agents_follow_up(lua_State *L) {
    auto *impl = impl_from(L);
    return push_agent_binding(L, impl->agent_bindings_.follow_up, agent_args(L),
                              "agents");
  }

  static int lua_pici_agents_interrupt(lua_State *L) {
    auto *impl = impl_from(L);
    return push_agent_binding(L, impl->agent_bindings_.interrupt, agent_args(L),
                              "agents");
  }

  static int lua_pici_agents_wait(lua_State *L) {
    auto *impl = impl_from(L);
    return push_agent_binding(L, impl->agent_bindings_.wait, agent_args(L),
                              "agents");
  }

  static int lua_pici_agents_close(lua_State *L) {
    auto *impl = impl_from(L);
    return push_agent_binding(L, impl->agent_bindings_.close, agent_args(L),
                              "agents");
  }

  static int lua_pici_session_request_compaction(lua_State *L) {
    auto *impl = impl_from(L);
    return push_agent_binding(L, impl->session_bindings_.request_compaction,
                              agent_args(L), "session");
  }

  static int lua_pici_session_request_model_switch(lua_State *L) {
    auto *impl = impl_from(L);
    return push_agent_binding(L, impl->session_bindings_.request_model_switch,
                              agent_args(L), "session");
  }

  static int lua_pici_session_request_fork(lua_State *L) {
    auto *impl = impl_from(L);
    return push_agent_binding(L, impl->session_bindings_.request_fork,
                              agent_args(L), "session");
  }

  static int lua_pici_session_action_status(lua_State *L) {
    auto *impl = impl_from(L);
    return push_agent_binding(L, impl->session_bindings_.action_status,
                              agent_args(L), "session");
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
        std::string s = (lua_isstring(L, -1) != 0) ? lua_tostring(L, -1) : "";
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

  void configure_info(const LuaHooks::AgentInfo &info) {
    std::scoped_lock lk(mutex_);
    run_agent_fn_ = info.run_agent;
    agent_bindings_ = info.agents;
    mailbox_bindings_ = info.mailbox;
    session_bindings_ = info.session;
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

  static int lua_tool_cancelled(lua_State *L) {
    auto *token =
        static_cast<std::stop_token *>(lua_touserdata(L, lua_upvalueindex(1)));
    lua_pushboolean(
        L, static_cast<int>(token != nullptr && token->stop_requested()));
    return 1;
  }

  static void lua_stop_hook(lua_State *L, lua_Debug *) {
    lua_getfield(L, LUA_REGISTRYINDEX, "pici.inline_stop_token");
    auto *token = static_cast<std::stop_token *>(lua_touserdata(L, -1));
    lua_pop(L, 1);
    if (token != nullptr && token->stop_requested())
      luaL_error(L, "Tool execution cancelled");
  }

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
      static_cast<void>(0); // Never let a C++ observer exception cross the Lua
                            // C ABI boundary.
    }
    return 0;
  }

  std::shared_ptr<ToolResult>
  execute_inline_tool(int ref, std::string_view args_json,
                      ToolExecutionContext context) {
    if (context.stop_token.stop_requested())
      return std::make_shared<LuaToolResult>("Tool execution cancelled", true);
    std::scoped_lock lk(mutex_);
    lua_rawgeti(L_, LUA_REGISTRYINDEX, ref);
    auto args = nlohmann::json::parse(args_json, nullptr, false);
    if (args.is_discarded() || !args.is_object())
      args = nlohmann::json::object();
    json_to_lua(L_, args);
    lua_newtable(L_);
    lua_pushlightuserdata(L_, &context.on_update);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_tool_update, 1);
    lua_setfield(L_, -2, "update");
    lua_pushlightuserdata(L_, &context.stop_token);
    lua_pushcclosure(L_, &LuaHooksImpl::lua_tool_cancelled, 1);
    lua_setfield(L_, -2, "cancelled");

    LuaRegistryPointerGuard stop_token_guard(L_, "pici.inline_stop_token",
                                             &context.stop_token);
    LuaRegistryPointerGuard actor_guard(L_, "pici.inline_actor",
                                        &context.actor);
    LuaRegistryPointerGuard presentation_guard(L_, "pici.inline_presentation",
                                               &context.on_presentation);
    const auto previous_hook = lua_gethook(L_);
    const auto previous_mask = lua_gethookmask(L_);
    const auto previous_count = lua_gethookcount(L_);
    lua_sethook(L_, &LuaHooksImpl::lua_stop_hook, LUA_MASKCOUNT, 1000);
    const auto call_status = lua_pcall(L_, 2, 1, 0);
    lua_sethook(L_, previous_hook, previous_mask, previous_count);
    if (context.stop_token.stop_requested()) {
      lua_pop(L_, 1);
      return std::make_shared<LuaToolResult>("Tool execution cancelled", true);
    }
    if (call_status != LUA_OK) {
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
    static constexpr std::array<std::string_view, 6> child_tools = {
        "agents_self",    "agents_list",  "agents_send",
        "agents_request", "agents_reply", "agents_inbox"};
    for (auto &spec : pending_tools_) {
      const bool child_safe =
          bundled_child_safe_tools_ &&
          std::ranges::contains(child_tools, std::string_view(spec.name));
      auto tool = std::make_shared<InlineLuaTool>(
          shared_from_this(), spec.exec_ref, std::move(spec.name),
          std::move(spec.description), std::move(spec.schema), source_path_,
          child_safe);
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
      std::string s = (lua_isstring(L, -1) != 0) ? lua_tostring(L, -1) : "";
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
  int format_tool_call_ref_{LUA_NOREF};
  int format_tool_result_ref_{LUA_NOREF};
  std::string source_path_;
  bool bundled_child_safe_tools_{false};
  std::vector<PendingTool> pending_tools_;
  std::vector<std::shared_ptr<const ToolDefinition>> inline_tools_;
  std::vector<LuaHooks::Command> commands_;
  LuaHooks::RunAgentFn run_agent_fn_;
  LuaHooks::AgentBindings agent_bindings_;
  LuaHooks::MailboxBindings mailbox_bindings_;
  LuaHooks::SessionBindings session_bindings_;
  nlohmann::json storage_{nlohmann::json::object()};
  std::filesystem::path storage_path_;
  mutable std::mutex mutex_;
};

InlineLuaTool::~InlineLuaTool() {
  if (impl_ && exec_ref_ != LUA_NOREF)
    impl_->unref_tool(exec_ref_);
}

std::shared_ptr<ToolResult>
InlineLuaTool::execute(std::string_view args_json,
                       ToolExecutionContext context) const {
  return impl_->execute_inline_tool(exec_ref_, args_json, std::move(context));
}

// ===========================================================================
// The `lua` built-in tool (plans/velvety-skipping-marble.md), Stage 2: the
// eval-state machinery -- allocator/memory cap, curated libraries, the
// __gc/setmetatable shim, the count-hook timeout, the pcall/xpcall shims,
// RAII state ownership, pici.scratch, and the error taxonomy. No
// pici.agents/session/mailbox trampolines are wired yet (Stage 3).
//
// Every one of the pieces below closes a specific, empirically-confirmed
// process-hang or crash bug found during this plan's adversarial review
// (see the plan's revision note and §3) -- none of it is speculative
// hardening, so none of it should be simplified away.
// ===========================================================================

constexpr std::size_t kEvalMemoryLimitBytes = 64 * 1024 * 1024;

// Reserved headroom before dispatching to a binding trampoline (Stage 3)
// or any other allocating call whose C++ locals aren't longjmp-safe (see
// lua_pici_truncate/json.encode, lua_tool.cpp above) -- checked via
// EvalMemory::has_headroom before the call is even attempted, so the
// allocating call itself becomes overwhelmingly unlikely to ever hit the
// cap. Nothing in Stage 2 calls has_headroom yet (there are no
// trampolines to gate); the mechanism is built now so Stage 3 only has to
// call it.
constexpr std::size_t kEvalBindingReserveBytes = 8 * 1024 * 1024;

constexpr int kEvalCountHookInstructionCount = 1000; // matches lua_stop_hook.

constexpr int kEvalDefaultTimeoutMs = 30000;
constexpr int kEvalMinTimeoutMs = 1;
constexpr int kEvalMaxTimeoutMs = 120000;

// Output-capture caps (plan §6 / design doc §6.1): the capture buffer is
// hard-stopped at kEvalOutputHardCapBytes during the run itself (a cheap
// safety valve against a script that prints gigabytes), then the whole
// tool-result text is run through the same truncate_head_ex every other
// built-in tool's output uses, once, after the run finishes.
constexpr std::size_t kEvalOutputHardCapBytes = 1024 * 1024;
constexpr std::size_t kEvalOutputMaxLines = 2000;
constexpr std::size_t kEvalOutputMaxBytes = 64 * 1024;

// pici.scratch caps (plan §4.4): a total cap, PLUS a separate per-key cap
// and key-count cap -- a single pathological key must not be able to
// consume the whole total budget on its own, and a flood of many small
// keys must not be able to grow unboundedly just because each one
// individually stays under the per-key cap.
constexpr std::size_t kScratchTotalBytesCap = 1024 * 1024;
constexpr std::size_t kScratchPerKeyBytesCap = 256 * 1024;
constexpr std::size_t kScratchMaxKeys = 256;

// ---------------------------------------------------------------------------
// Memory-capped allocator (plan §3 "Memory cap").
// ---------------------------------------------------------------------------

struct EvalMemory {
  std::size_t used{0};
  std::size_t limit{0};

  // Reserved-headroom check for a binding call or other allocating call
  // whose C++ locals aren't longjmp-safe across a possible LUA_ERRMEM --
  // see kEvalBindingReserveBytes above. Not called anywhere in Stage 2;
  // exists so Stage 3's trampolines can call it before dispatch.
  bool has_headroom(std::size_t reserve_bytes) const {
    return reserve_bytes <= limit && used <= limit - reserve_bytes;
  }
};

// Lua allocator function (lua_Alloc). `osize` is the previous block's size
// when `ptr` is non-null; when `ptr` is null, `osize` is instead a *type
// tag* for the (non-existent) previous block, not a size -- so treating it
// as a size in that branch would be wrong.
void *eval_alloc(void *ud, void *ptr, std::size_t osize, std::size_t nsize) {
  auto *mem = static_cast<EvalMemory *>(ud);
  const std::size_t old_size = (ptr == nullptr) ? 0 : osize;
  if (nsize == 0) {
    std::free(ptr);
    mem->used -= old_size;
    return nullptr;
  }
  if (nsize > old_size && mem->used + (nsize - old_size) > mem->limit) {
    // Returning nullptr here (rather than actually running out of system
    // memory) is what turns a script-controlled allocation loop into a
    // catchable Lua "not enough memory" error (LUA_ERRMEM) instead of an
    // OOM of the whole pici process.
    return nullptr;
  }
  void *out = std::realloc(ptr, nsize);
  if (out == nullptr)
    return nullptr;
  // When nsize < old_size (shrinking/realloc-down), `nsize - old_size`
  // underflows as an unsigned subtraction -- this is intentional, not a
  // bug: two's-complement wraparound makes `mem->used += (nsize -
  // old_size)` compute the same result as `mem->used -= (old_size -
  // nsize)` modulo 2^64, and mem->used never actually goes negative in
  // practice because it is always kept in sync with real outstanding
  // allocations.
  mem->used += nsize - old_size;
  return out;
}

// ---------------------------------------------------------------------------
// Count-hook run context, retrieved via lua_getextraspace (plan §3
// "EvalRun* retrieval must use lua_getextraspace, not a registry lookup" --
// a registry lookup interns a string via luaS_new, which allocates, which
// can itself raise LUA_ERRMEM from inside the hook under this capped
// allocator, silently misclassifying a timeout as a memory error).
//
// Every member here is trivially destructible on purpose: the hook body
// below may call luaL_error, which longjmps past any C++ destructors that
// would otherwise run -- so no non-trivially-destructible local may ever
// be live inside it, and this struct (accessed only through a pointer, but
// still worth keeping trivial in its own right) follows the same rule.
struct EvalRun {
  const std::stop_token *stop{nullptr};
  std::chrono::steady_clock::time_point deadline{};
  bool deadline_hit{false};
  bool cancelled{false};
};
static_assert(std::is_trivially_destructible_v<EvalRun>);

void eval_store_run(lua_State *L, EvalRun *run) {
  std::memcpy(lua_getextraspace(L), &run, sizeof(run));
}

EvalRun *eval_run_from(lua_State *L) {
  EvalRun *run = nullptr;
  std::memcpy(&run, lua_getextraspace(L), sizeof(run));
  return run;
}

// LUA_MASKCOUNT hook, fired every kEvalCountHookInstructionCount VM
// instructions -- matching the exact existing precedent at lua_stop_hook
// above (count 1000). No non-trivially-destructible C++ locals: only a
// pointer and a direct time comparison, per EvalRun's own doc comment.
void eval_count_hook(lua_State *L, lua_Debug * /*ar*/) {
  EvalRun *run = eval_run_from(L);
  if (run == nullptr)
    return;
  if (run->stop != nullptr && run->stop->stop_requested()) {
    run->cancelled = true;
    luaL_error(L, "pici: execution cancelled");
  }
  if (std::chrono::steady_clock::now() >= run->deadline) {
    run->deadline_hit = true;
    luaL_error(L, "pici: time budget exceeded");
  }
}

// ---------------------------------------------------------------------------
// pcall/xpcall shims (plan §3, "pcall can swallow the abort"). A budget
// error raised inside a user pcall would otherwise be caught by the
// script, the hook would fire again 1000 instructions later, and it would
// be caught again -- a livelock the tool never returns from. These shims
// re-raise once the run's deadline/cancellation flag is set, regardless of
// what the wrapped call did with its own error. Each re-raise unwinds
// exactly one pcall level (via lua_error's longjmp to the next enclosing
// protected call), and no new pcall level can be entered without passing
// back through one of these two shims -- so however deeply pcalls are
// nested, termination happens in at most "nesting depth" additional
// re-raises, not a livelock.
// ---------------------------------------------------------------------------

int eval_pcall(lua_State *L) {
  luaL_checkany(L, 1);
  const int nargs = lua_gettop(L) - 1;
  const int status = lua_pcall(L, nargs, LUA_MULTRET, 0);
  EvalRun *run = eval_run_from(L);
  if (status != LUA_OK && run != nullptr &&
      (run->deadline_hit || run->cancelled)) {
    return lua_error(L); // A budget/cancellation abort is not catchable.
  }
  lua_pushboolean(L, status == LUA_OK);
  lua_rotate(L, 1, 1); // Mirrors lbaselib.c's finishpcall stack shape.
  return lua_gettop(L);
}

int eval_xpcall(lua_State *L) {
  const int n = lua_gettop(L);
  luaL_checkany(L, 1);
  luaL_checktype(L, 2, LUA_TFUNCTION); // Message handler is mandatory.
  lua_pushvalue(L, 2);                 // Duplicate msgh onto the top.
  lua_insert(L, 1);                    // Move the duplicate to the bottom.
  lua_remove(L, 3);                    // Drop the now-redundant original.
  // Stack is now: [1]=msgh, [2]=f, [3..n]=original extra args (n-2 of them).
  const int status = lua_pcall(L, n - 2, LUA_MULTRET, 1);
  EvalRun *run = eval_run_from(L);
  if (status != LUA_OK && run != nullptr &&
      (run->deadline_hit || run->cancelled)) {
    return lua_error(L);
  }
  lua_remove(L, 1); // Drop msgh, leaving results (or the error object).
  lua_pushboolean(L, status == LUA_OK);
  lua_rotate(L, 1, 1);
  return lua_gettop(L);
}

// ---------------------------------------------------------------------------
// setmetatable shim (plan §3, the __gc shim). Lua only marks an object
// finalizable if __gc is present on its metatable *at setmetatable time* --
// a later rawset(mt, "__gc", f) is not honored -- so checking here, and
// only here, is a complete fix, not a partial one. Without this,
// setmetatable({}, {__gc = function() while true do end end}) hangs
// lua_close() forever: the count hook never fires inside a finalizer
// (empirically confirmed: zero hook fires across a 50-million-iteration
// finalizer loop), so no timeout/cancellation ever recovers it.
// ---------------------------------------------------------------------------

int eval_setmetatable(lua_State *L) {
  const int mt_type = lua_type(L, 2);
  luaL_checktype(L, 1, LUA_TTABLE);
  luaL_argexpected(L, mt_type == LUA_TNIL || mt_type == LUA_TTABLE, 2,
                   "nil or table");
  if (luaL_getmetafield(L, 1, "__metatable") != LUA_TNIL)
    return luaL_error(L, "cannot change a protected metatable");
  if (mt_type == LUA_TTABLE) {
    lua_getfield(L, 2, "__gc");
    const bool has_gc = lua_isnil(L, -1) == 0;
    lua_pop(L, 1);
    if (has_gc) {
      return luaL_error(
          L, "pici: setmetatable with a __gc finalizer is not allowed");
    }
  }
  lua_settop(L, 2);
  lua_setmetatable(L, 1);
  return 1;
}

// ---------------------------------------------------------------------------
// Output capture (plan §6, design doc §6.1): print/pici.log write into a
// per-call buffer instead of std::cerr, matching lua_pici_log's own
// tab-separated/newline-terminated formatting.
// ---------------------------------------------------------------------------

struct EvalOutput {
  std::string buffer;
  bool truncated{false};
};

void eval_append_output(EvalOutput &out, std::string_view text) {
  if (out.truncated)
    return;
  out.buffer.append(text);
  if (out.buffer.size() > kEvalOutputHardCapBytes) {
    out.truncated = true; // Stop growing; final rendering trims further.
  }
}

// Shared by both `print` and `pici.log` -- same formatting convention as
// lua_pici_log above (luaL_tolstring per argument, tab-separated,
// newline-terminated), just writing into an EvalOutput buffer (upvalue 1)
// instead of std::cerr. Writing to stderr mid-turn would corrupt the TUI
// viewport; capturing into the result keeps it self-contained.
int eval_capture_print(lua_State *L) {
  auto *out = static_cast<EvalOutput *>(lua_touserdata(L, lua_upvalueindex(1)));
  const int n = lua_gettop(L);
  std::string line;
  for (int i = 1; i <= n; ++i) {
    if (i > 1)
      line += '\t';
    line += luaL_tolstring(L, i, nullptr);
    lua_pop(L, 1);
  }
  line += '\n';
  if (out != nullptr)
    eval_append_output(*out, line);
  return 0;
}

// ---------------------------------------------------------------------------
// pici.scratch trampolines (plan §4.4). LuaEvalBindings owns the actual
// storage (session-length lifetime); these just marshal between Lua and
// the JSON the holder deals in. Marshaling MUST go through the
// Stage-1-updated lua_to_json (depth cap + cycle guard baked in) -- do not
// introduce a second conversion path here that could bypass that cap.
// ---------------------------------------------------------------------------

int eval_scratch_get(lua_State *L) {
  auto *bindings =
      static_cast<LuaEvalBindings *>(lua_touserdata(L, lua_upvalueindex(1)));
  const char *key = luaL_checkstring(L, 1);
  if (bindings == nullptr) {
    lua_pushnil(L);
    return 1;
  }
  auto value = bindings->scratch_get(key);
  if (!value) {
    lua_pushnil(L);
    return 1;
  }
  json_to_lua(L, *value);
  return 1;
}

int eval_scratch_set(lua_State *L) {
  auto *bindings =
      static_cast<LuaEvalBindings *>(lua_touserdata(L, lua_upvalueindex(1)));
  const char *key = luaL_checkstring(L, 1);
  luaL_checkany(L, 2);
  if (bindings == nullptr) {
    lua_pushnil(L);
    lua_pushstring(L, "pici.scratch is not available");
    return 2;
  }
  // Depth-capped/cycle-guarded conversion (Stage 1 fix) -- this is the only
  // path a Lua value takes on its way into scratch storage.
  const auto value = lua_to_json(L, 2);
  auto err = bindings->scratch_set(key, value);
  if (err) {
    lua_pushnil(L);
    lua_pushstring(L, err->c_str());
    return 2;
  }
  lua_pushboolean(L, 1);
  return 1;
}

// ---------------------------------------------------------------------------
// The curated environment (plan §3, "Curated libraries, not luaL_openlibs").
// ---------------------------------------------------------------------------

constexpr std::array<luaL_Reg, 5> kEvalLibs{{
    {LUA_GNAME, luaopen_base},
    {LUA_TABLIBNAME, luaopen_table},
    {LUA_STRLIBNAME, luaopen_string},
    {LUA_MATHLIBNAME, luaopen_math},
    {LUA_UTF8LIBNAME, luaopen_utf8},
}};

// Builds a curated `os` global exposing only time/clock/date -- no
// getenv (API keys), execute/remove/rename/tmpname (shell/filesystem), or
// exit (kills pici). luaL_requiref's glb=0 leaves the *real* os module
// off the stack as a global; only the hand-picked fields are copied onto
// a fresh table that becomes the actual global `os`.
void eval_build_curated_os(lua_State *L) {
  luaL_requiref(L, LUA_OSLIBNAME, luaopen_os, 0);
  lua_newtable(L);
  for (const char *name : {"time", "clock", "date"}) {
    lua_getfield(L, -2, name);
    lua_setfield(L, -2, name);
  }
  lua_setglobal(L, "os");
  lua_pop(L, 1); // Pop the real (uncurated) os module table.
}

void eval_build_pici_table(lua_State *L, EvalOutput *output,
                           LuaEvalBindings *bindings) {
  lua_newtable(L);

  lua_pushlightuserdata(L, output);
  lua_pushcclosure(L, &eval_capture_print, 1);
  lua_setfield(L, -2, "log");

  lua_pushcfunction(L, &lua_pici_truncate);
  lua_setfield(L, -2, "truncate_tool_result");

  lua_newtable(L); // pici.scratch
  lua_pushlightuserdata(L, bindings);
  lua_pushcclosure(L, &eval_scratch_get, 1);
  lua_setfield(L, -2, "get");
  lua_pushlightuserdata(L, bindings);
  lua_pushcclosure(L, &eval_scratch_set, 1);
  lua_setfield(L, -2, "set");
  lua_setfield(L, -2, "scratch");

  lua_setglobal(L, "pici");
}

// Builds the full run-mode environment: curated libraries, the
// setmetatable/__gc shim, the pcall/xpcall shims, print capture, the
// string pattern-matching removal, the curated os table, json, and pici
// (log/truncate_tool_result/scratch only in Stage 2 -- agents/session/
// mailbox are Stage 3). Never called for mode:"check" -- a script that is
// only compiled, never run, cannot observe or mutate any of this, which is
// the whole point of check mode.
void eval_build_environment(lua_State *L, EvalOutput *output,
                            LuaEvalBindings *bindings) {
  for (const auto &lib : kEvalLibs) {
    luaL_requiref(L, lib.name, lib.func, 1);
    lua_pop(L, 1);
  }

  // load/loadfile/dofile: bytecode/file-backed chunk loading, both defeat
  // the "t"-mode guarantee at compile time. collectgarbage: disables
  // nothing safety-relevant on its own, but its "count"/"generational"/etc
  // options are a needless surface for a throwaway state. warn: writes
  // straight to stderr -- the review found this was missed in an earlier
  // draft; nilled out alongside the others, not redirected.
  for (const char *name :
       {"load", "loadfile", "dofile", "collectgarbage", "warn"}) {
    lua_pushnil(L);
    lua_setglobal(L, name);
  }

  lua_pushcfunction(L, &eval_setmetatable);
  lua_setglobal(L, "setmetatable");
  lua_pushcfunction(L, &eval_pcall);
  lua_setglobal(L, "pcall");
  lua_pushcfunction(L, &eval_xpcall);
  lua_setglobal(L, "xpcall");

  lua_pushlightuserdata(L, output);
  lua_pushcclosure(L, &eval_capture_print, 1);
  lua_setglobal(L, "print");

  // string.dump (defeats "t"-mode the same way load does) plus
  // find/match/gmatch/gsub: Lua's pattern-matching engine can be driven
  // into catastrophic backtracking (measured 38+ seconds of unbounded CPU
  // from one call, zero count-hook fires the whole time -- the hook only
  // fires between VM instructions, and a C library call is one
  // instruction regardless of how long it runs internally) using well
  // under the 64MB memory cap. No budget or cap can close this; removing
  // the functions is the only clean fix. format/rep/byte/char/sub/upper/
  // lower/len/reverse all stay.
  lua_getglobal(L, "string");
  for (const char *name : {"dump", "find", "match", "gmatch", "gsub"}) {
    lua_pushnil(L);
    lua_setfield(L, -2, name);
  }
  lua_pop(L, 1);

  eval_build_curated_os(L);
  register_json_module(L);
  eval_build_pici_table(L, output, bindings);
}

// ---------------------------------------------------------------------------
// Compile-error line extraction ("script:<N>: ..." -> N), and the
// lua_pcall message handler that appends a traceback to a runtime error.
// ---------------------------------------------------------------------------

std::optional<int> eval_extract_line(const std::string &message) {
  constexpr std::string_view prefix = "script:";
  if (message.compare(0, prefix.size(), prefix) != 0)
    return std::nullopt;
  std::size_t pos = prefix.size();
  std::size_t end = pos;
  while (end < message.size() && message[end] >= '0' && message[end] <= '9')
    ++end;
  if (end == pos)
    return std::nullopt;
  try {
    return std::stoi(message.substr(pos, end - pos));
  } catch (const std::exception &) {
    return std::nullopt;
  }
}

int eval_message_handler(lua_State *L) {
  const char *msg = lua_tostring(L, 1);
  if (msg == nullptr) {
    if (luaL_callmeta(L, 1, "__tostring") != 0 && lua_isstring(L, -1) != 0)
      return 1;
    msg =
        lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
  }
  luaL_traceback(L, L, msg, 1);
  return 1;
}

// ---------------------------------------------------------------------------
// The `lua` tool itself.
// ---------------------------------------------------------------------------

constexpr std::string_view kLuaEvalSchema =
    R"json({"type":"object","properties":{"script":{"type":"string","description":"Lua 5.4 source to compile, and (unless mode is \"check\") execute in-process against pici's own API."},"mode":{"type":"string","enum":["run","check"],"description":"\"run\" (default) compiles then executes. \"check\" only compiles and reports syntax errors, with no side effects."},"timeout_ms":{"type":"integer","minimum":1,"maximum":120000,"description":"Wall-clock budget for execution, default 30000. Ignored in check mode."}},"required":["script"],"additionalProperties":false})json";

constexpr std::string_view kLuaEvalDescription =
    "Execute a Lua 5.4 snippet inside pici itself, against pici's live "
    "scripting API. Use it to drive subagents, mailbox messaging, and "
    "session-lifecycle actions in one step instead of many tool calls.\n"
    "\n"
    "Available: pici.agents.{spawn,get,list,send,follow_up,interrupt,wait,"
    "close}, "
    "pici.session.{request_compaction,request_model_switch,request_fork,"
    "action_status}, "
    "pici.mailbox.{self,list,send,request,reply,inbox,ack,wait,status}, "
    "pici.log(...), pici.truncate_tool_result(s), pici.scratch.{get,set}, "
    "pici.model, pici.cwd, pici.tools, json.encode/decode, and the "
    "string/table/math/utf8 libraries.\n"
    "\n"
    "Not available: io, os.*(except os.time/clock/date), package/require, "
    "debug, coroutine, load/loadfile/dofile, string.find/match/gmatch/gsub "
    "(pattern matching is not available in this sandbox -- use plain "
    "string operations), and the file/shell tools -- use bash/read/write "
    "for those.\n"
    "\n"
    "Every pici.* call returns either a result table, or nil plus an error "
    "string as a second value -- check it: `local t, err = "
    "pici.agents.spawn{...}`.\n"
    "\n"
    "Each call runs in a fresh state: locals, globals and functions do NOT "
    "survive to the next lua call. Anything you need later must be in the "
    "return value, or stored via pici.scratch.set(key, value) and read "
    "back with pici.scratch.get(key) from a later call. Subagents, "
    "mailbox messages and session requests DO persist -- they live in "
    "pici, not in the script -- so a task spawned in one call can be "
    "waited on by id in a later call.\n"
    "\n"
    "Return values are reported back to you; so is anything you print() "
    "or pici.log().\n"
    "\n"
    "mode:\"check\" compiles without running: use it to catch syntax "
    "errors before causing side effects. It validates syntax only -- it "
    "does not detect undefined variables, wrong argument shapes, or "
    "runtime errors.";

class LuaEvalTool final : public ToolDefinition {
public:
  explicit LuaEvalTool(std::shared_ptr<LuaEvalBindings> bindings)
      : bindings_(std::move(bindings)),
        schema_(std::make_unique<LuaToolSchema>(std::string(kLuaEvalSchema))) {}

  std::string_view name() const override { return "lua"; }
  std::string_view description() const override { return kLuaEvalDescription; }
  ToolSchema &schema() const override { return *schema_; }
  ToolCapabilities capabilities() const override {
    // Never child_safe: see the plan's §5.1 -- child_safe=false alone,
    // with no name-based special-casing, fully excludes this tool from
    // every child task.
    return {.child_safe = false};
  }
  ToolExecutionMode execution_mode() const override {
    // See the plan's §3: without this, two `lua` calls in one batch run
    // concurrently by default (ToolExecutionMode::parallel), turning the
    // 64MB memory cap into N*64MB and making pici.scratch's
    // get-then-set non-atomic as a read-modify-write.
    return ToolExecutionMode::sequential;
  }

  std::shared_ptr<ToolResult>
  execute(std::string_view call_id, std::string_view args_json,
          std::stop_token stop_tok,
          ToolUpdateCallback on_update) const override {
    return execute(args_json,
                   ToolExecutionContext{.call_id = call_id,
                                        .stop_token = stop_tok,
                                        .on_update = std::move(on_update)});
  }

  std::shared_ptr<ToolResult>
  execute(std::string_view args_json,
          ToolExecutionContext context) const override {
    if (context.stop_token.stop_requested())
      return std::make_shared<LuaToolResult>("Tool execution cancelled", true);

    auto parsed = nlohmann::json::parse(args_json, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object())
      parsed = nlohmann::json::object();

    const std::string script = parsed.value("script", std::string{});
    const bool check_only = parsed.value("mode", std::string{"run"}) == "check";
    int timeout_ms = parsed.value("timeout_ms", kEvalDefaultTimeoutMs);
    timeout_ms = std::clamp(timeout_ms, kEvalMinTimeoutMs, kEvalMaxTimeoutMs);

    // The bindings snapshot is declared *before* the Lua state (and hence
    // before the RAII closer that owns it) so that reverse-order C++
    // destruction tears the state down first, while anything the state
    // might still reach through a trampoline upvalue pointing into this
    // snapshot remains valid until that happens. Stage 2 wires no
    // trampolines that read from it yet (that's Stage 3), but the
    // declaration order is load-bearing once it does, so it is
    // established here rather than left for a later stage to get right.
    const LuaEvalBindings::Snapshot bindings_snapshot =
        bindings_ ? bindings_->snapshot() : LuaEvalBindings::Snapshot{};
    static_cast<void>(bindings_snapshot); // Unused until Stage 3.

    EvalMemory memory{.limit = kEvalMemoryLimitBytes};
    lua_State *raw_state = lua_newstate(&eval_alloc, &memory);
    if (raw_state == nullptr) {
      return std::make_shared<LuaToolResult>(
          "[lua error] failed to create Lua state", true);
    }

    struct EvalStateCloser {
      lua_State *L;
      explicit EvalStateCloser(lua_State *state) : L(state) {}
      ~EvalStateCloser() {
        if (L != nullptr)
          lua_close(L);
      }
      EvalStateCloser(const EvalStateCloser &) = delete;
      EvalStateCloser &operator=(const EvalStateCloser &) = delete;
      EvalStateCloser(EvalStateCloser &&) = delete;
      EvalStateCloser &operator=(EvalStateCloser &&) = delete;
    } state_closer{raw_state};
    lua_State *L = raw_state;

    EvalOutput output;
    EvalRun run;
    run.stop = &context.stop_token;
    run.deadline = std::chrono::steady_clock::now() +
                   std::chrono::milliseconds(timeout_ms);
    eval_store_run(L, &run);

    if (!check_only)
      eval_build_environment(L, &output, bindings_.get());

    const int load_status = eval_load(L, script);
    if (load_status != LUA_OK) {
      std::string message =
          lua_isstring(L, -1) != 0 ? lua_tostring(L, -1) : "compile error";
      lua_pop(L, 1);
      const auto line = eval_extract_line(message);
      nlohmann::json details{{"mode", check_only ? "check" : "run"},
                             {"error_kind", "compile"}};
      if (line)
        details["line"] = *line;
      return std::make_shared<LuaToolResult>("[lua compile error] " + message,
                                             true, details.dump());
    }

    if (check_only) {
      lua_pop(L, 1); // Discard the compiled function -- never executed.
      const auto lines = 1 + static_cast<std::size_t>(std::count(
                                 script.begin(), script.end(), '\n'));
      std::string ok_message =
          "[lua check ok] " + std::to_string(lines) + " lines, " +
          std::to_string(script.size()) +
          " bytes -- compiles cleanly.\n"
          "Syntax only: undefined globals, wrong argument shapes and "
          "runtime errors are not detected.";
      nlohmann::json details{{"mode", "check"}};
      return std::make_shared<LuaToolResult>(std::move(ok_message), false,
                                             details.dump());
    }

    // Message handler goes below the compiled chunk on the stack so its
    // absolute index can be passed as lua_pcall's msgh argument.
    lua_pushcfunction(L, &eval_message_handler);
    lua_insert(L, -2);
    constexpr int kMsghIndex = 1;

    lua_sethook(L, &eval_count_hook, LUA_MASKCOUNT,
                kEvalCountHookInstructionCount);
    const auto start = std::chrono::steady_clock::now();
    const int call_status = lua_pcall(L, 0, LUA_MULTRET, kMsghIndex);
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start)
            .count();
    lua_sethook(L, nullptr, 0, 0);
    lua_remove(L, kMsghIndex);

    // Cancellation/timeout take priority over whatever lua_pcall's status
    // says: they are deliberate aborts we drove, not a coincidental error
    // classification race against LUA_ERRMEM.
    if (run.cancelled || context.stop_token.stop_requested())
      return std::make_shared<LuaToolResult>("Tool execution cancelled", true);
    if (run.deadline_hit) {
      nlohmann::json details{{"mode", "run"},
                             {"error_kind", "timeout"},
                             {"timed_out", true},
                             {"duration_ms", elapsed_ms}};
      return std::make_shared<LuaToolResult>(
          "[lua timeout] script exceeded its " + std::to_string(timeout_ms) +
              " ms budget",
          true, details.dump());
    }

    if (call_status != LUA_OK) {
      nlohmann::json details{{"mode", "run"}, {"duration_ms", elapsed_ms}};
      if (call_status == LUA_ERRMEM) {
        details["error_kind"] = "memory";
        lua_settop(L, 0);
        return std::make_shared<LuaToolResult>(
            "[lua memory limit] script exceeded its 64 MB allowance", true,
            details.dump());
      }
      details["error_kind"] = "runtime";
      std::string content;
      if (lua_type(L, 1) == LUA_TSTRING) {
        content = "[lua error] " + std::string(lua_tostring(L, 1));
      } else {
        // Non-string error value (e.g. error({code=1})): rendered via the
        // depth-capped/cycle-guarded lua_to_json, same as everywhere else
        // a Lua value crosses into JSON in this file.
        content = "[lua error] " + lua_to_json(L, 1).dump();
      }
      lua_settop(L, 0);
      return std::make_shared<LuaToolResult>(std::move(content), true,
                                             details.dump());
    }

    // Success: render the -- output -- and -- return -- sections.
    const int nresults = lua_gettop(L);
    std::vector<std::string> rendered;
    rendered.reserve(static_cast<std::size_t>(nresults));
    for (int i = 1; i <= nresults; ++i) {
      if (lua_istable(L, i)) {
        rendered.push_back(lua_to_json(L, i).dump(2));
      } else {
        rendered.emplace_back(luaL_tolstring(L, i, nullptr));
        lua_pop(L, 1); // Pop the string luaL_tolstring pushed on top.
      }
    }
    lua_settop(L, 0);

    std::string return_section;
    for (const auto &r : rendered) {
      if (!return_section.empty())
        return_section += "\n";
      return_section += r;
    }

    std::string rendered_output = output.buffer;
    if (!rendered_output.empty()) {
      auto trimmed = truncate_head_ex(rendered_output, kEvalOutputMaxLines,
                                      kEvalOutputMaxBytes);
      rendered_output = std::move(trimmed.content);
    }

    std::string header = "[lua ok] " + std::to_string(elapsed_ms) + "ms";
    std::string body;
    if (!rendered_output.empty())
      body += "\n-- output --\n" + rendered_output;
    if (!return_section.empty())
      body += "\n-- return --\n" + return_section;
    if (body.empty())
      header += " (no output, no return value)";

    nlohmann::json details{{"mode", "run"}, {"duration_ms", elapsed_ms}};
    if (output.truncated)
      details["output_truncated"] = true;
    return std::make_shared<LuaToolResult>(header + body, false,
                                           details.dump());
  }

private:
  // luaL_loadbufferx with mode "t" -- refuses precompiled bytecode chunks
  // (a "\27Lua"-prefixed blob is a memory-safety hazard, not a syntax
  // error) -- and takes an explicit length, so an embedded NUL in the
  // script can't silently truncate it the way strlen-based loading would.
  // Chunk name "=script": the leading '=' suppresses Lua's
  // [string "..."] quoting, so errors read "script:3: ..." rather than
  // "[string "local x = ..."]:3: ...".
  static int eval_load(lua_State *L, const std::string &script) {
    return luaL_loadbufferx(L, script.data(), script.size(), "=script", "t");
  }

  std::shared_ptr<LuaEvalBindings> bindings_;
  std::unique_ptr<LuaToolSchema> schema_;
};

} // namespace

std::shared_ptr<const ToolDefinition>
load_lua_tool(const std::filesystem::path &path) {
  return std::make_shared<LuaTool>(path);
}

void LuaEvalBindings::store(const LuaHooks::AgentInfo &info) {
  std::scoped_lock lock(mutex_);
  current_.agents = info.agents;
  current_.session = info.session;
  current_.mailbox = info.mailbox;
  current_.model_id = info.model_id;
  current_.model_provider = info.model_provider;
  current_.model_api = info.model_api;
  current_.cwd = info.cwd;
  current_.tool_names = info.tool_names;
  current_.configured = true;
}

LuaEvalBindings::Snapshot LuaEvalBindings::snapshot() const {
  std::scoped_lock lock(mutex_);
  return current_;
}

void LuaEvalBindings::clear() {
  std::scoped_lock lock(mutex_);
  current_ = Snapshot{};
}

std::optional<nlohmann::json>
LuaEvalBindings::scratch_get(const std::string &key) const {
  std::scoped_lock lock(scratch_mutex_);
  auto it = scratch_.find(key);
  if (it == scratch_.end())
    return std::nullopt;
  return it->second.value;
}

std::optional<std::string>
LuaEvalBindings::scratch_set(const std::string &key,
                             const nlohmann::json &value) {
  const std::string dumped = value.dump();
  const std::size_t new_size = dumped.size();
  if (new_size > kScratchPerKeyBytesCap)
    return "pici: scratch limit exceeded (per-key size)";

  std::scoped_lock lock(scratch_mutex_);
  auto it = scratch_.find(key);
  const std::size_t old_size =
      (it != scratch_.end()) ? it->second.cached_size : 0;
  const std::size_t projected_total =
      scratch_total_bytes_ - old_size + new_size;
  if (projected_total > kScratchTotalBytesCap)
    return "pici: scratch limit exceeded (total size)";
  if (it == scratch_.end() && scratch_.size() >= kScratchMaxKeys)
    return "pici: scratch limit exceeded (key count)";

  scratch_total_bytes_ = projected_total;
  scratch_[key] = ScratchEntry{value, new_size};
  return std::nullopt;
}

std::shared_ptr<const ToolDefinition>
make_lua_eval_tool(std::shared_ptr<LuaEvalBindings> bindings) {
  return std::make_shared<LuaEvalTool>(std::move(bindings));
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

std::shared_ptr<LuaHooks> load_lua_hooks(const std::filesystem::path &path,
                                         bool bundled_child_safe_tools) {
  auto impl = std::make_shared<LuaHooksImpl>(path, bundled_child_safe_tools);
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
               std::stop_token
                   stop_tok) // NOLINT(performance-unnecessary-value-param)
    { return impl->call_prepare_context(context, estimated_tokens, stop_tok); };
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
  if (impl->has_format_tool_call()) {
    hooks->format_tool_call = [impl](const LuaHooks::FormatToolCallContext &ctx)
        -> std::optional<std::string> {
      return impl->call_format_tool_call(ctx);
    };
  }
  if (impl->has_format_tool_result()) {
    hooks->format_tool_result =
        [impl](const LuaHooks::FormatToolResultContext &ctx)
        -> std::optional<std::string> {
      return impl->call_format_tool_result(ctx);
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
  lua_pushcfunction(L, &lua_pici_truncate);
  lua_setfield(L, -2, "truncate_tool_result");
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
load_lua_hooks_dir(const std::filesystem::path &directory,
                   bool bundled_child_safe_tools) {
  std::vector<std::shared_ptr<LuaHooks>> list;
  std::error_code ec;
  for (const auto &entry : std::filesystem::directory_iterator(directory, ec)) {
    if (!entry.is_regular_file(ec) || entry.path().extension() != ".lua")
      continue;
    try {
      list.push_back(load_lua_hooks(entry.path(), bundled_child_safe_tools &&
                                                      entry.path().filename() ==
                                                          "mailbox.lua"));
    } catch (const std::exception &e) {
      std::cerr << "warning: skipping hooks file " << entry.path() << ": "
                << e.what() << "\n";
    }
  }
  return compose_hooks(std::move(list));
}

std::shared_ptr<LuaHooks>
tool_formatter_hooks_only(const std::shared_ptr<LuaHooks> &hooks) {
  if (!hooks)
    return nullptr;
  auto filtered = std::make_shared<LuaHooks>();
  filtered->source_path = hooks->source_path;
  filtered->format_tool_call = hooks->format_tool_call;
  filtered->format_tool_result = hooks->format_tool_result;
  return filtered;
}

namespace {

// Each compose_*() below wires one LuaHooks field on `out` to combine every
// add-on's implementation of that field, per the combination rule named in
// its comment. Split out of compose_hooks() purely to shrink that
// function's branch count; behavior is unchanged from the inline versions
// they replace.

void compose_before_tool_call(
    const std::vector<std::shared_ptr<LuaHooks>> &list, LuaHooks &out) {
  // before_tool_call — run all; first block wins
  if (!std::ranges::any_of(list,
                           [](const auto &h) { return !!h->before_tool_call; }))
    return;
  out.before_tool_call =
      [list](const BeforeToolCallContext &ctx,
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

void compose_on_event(const std::vector<std::shared_ptr<LuaHooks>> &list,
                      LuaHooks &out) {
  // on_event — notify all observers in load order.
  if (!std::ranges::any_of(list, [](const auto &h) { return !!h->on_event; }))
    return;
  out.on_event = [list](const AgentEvent &event) {
    for (const auto &h : list) {
      if (h->on_event)
        h->on_event(event);
    }
  };
}

void compose_prepare_context(const std::vector<std::shared_ptr<LuaHooks>> &list,
                             LuaHooks &out) {
  // prepare_context — first non-null replacement wins.
  if (!std::ranges::any_of(list,
                           [](const auto &h) { return !!h->prepare_context; }))
    return;
  out.prepare_context =
      [list](const AgentContext &context, std::size_t estimated_tokens,
             std::stop_token
                 stop_tok) // NOLINT(performance-unnecessary-value-param)
      -> std::optional<std::vector<Message>> {
    for (const auto &h : list) {
      if (!h->prepare_context)
        continue;
      if (auto result = h->prepare_context(context, estimated_tokens, stop_tok))
        return result;
    }
    return std::nullopt;
  };
}

void compose_after_tool_call(const std::vector<std::shared_ptr<LuaHooks>> &list,
                             LuaHooks &out) {
  // after_tool_call — run all; first non-null wins
  if (!std::ranges::any_of(list,
                           [](const auto &h) { return !!h->after_tool_call; }))
    return;
  out.after_tool_call =
      [list](const AfterToolCallContext &ctx,
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

void compose_should_stop_after_turn(
    const std::vector<std::shared_ptr<LuaHooks>> &list, LuaHooks &out) {
  // should_stop_after_turn — OR
  if (!std::ranges::any_of(
          list, [](const auto &h) { return !!h->should_stop_after_turn; }))
    return;
  out.should_stop_after_turn =
      [list](const Message &msg, const std::vector<ToolResultMessage> &results,
             const AgentContext &ctx) -> bool {
    return std::ranges::any_of(list, [&](const auto &h) {
      return h->should_stop_after_turn &&
             h->should_stop_after_turn(msg, results, ctx);
    });
  };
}

void compose_on_command(const std::vector<std::shared_ptr<LuaHooks>> &list,
                        LuaHooks &out) {
  // on_command — first handled wins
  if (!std::ranges::any_of(list, [](const auto &h) { return !!h->on_command; }))
    return;
  out.on_command =
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

void compose_commands_and_tools(
    const std::vector<std::shared_ptr<LuaHooks>> &list, LuaHooks &out) {
  // commands + registered_tools — union
  for (const auto &h : list) {
    out.commands.insert(out.commands.end(), h->commands.begin(),
                        h->commands.end());
    out.registered_tools.insert(out.registered_tools.end(),
                                h->registered_tools.begin(),
                                h->registered_tools.end());
  }
}

void compose_configure(const std::vector<std::shared_ptr<LuaHooks>> &list,
                       LuaHooks &out) {
  // configure — forward to all
  out.configure = [list](const LuaHooks::AgentInfo &info) {
    for (const auto &h : list)
      if (h->configure)
        h->configure(info);
  };
}

void compose_prompt_line(const std::vector<std::shared_ptr<LuaHooks>> &list,
                         LuaHooks &out) {
  // prompt_line — last non-nil wins (override semantics)
  if (!std::ranges::any_of(list,
                           [](const auto &h) { return !!h->prompt_line; }))
    return;
  out.prompt_line =
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

void compose_format_tool_call(
    const std::vector<std::shared_ptr<LuaHooks>> &list, LuaHooks &out) {
  // format_tool_call — last non-nil wins (override semantics)
  if (!std::ranges::any_of(list,
                           [](const auto &h) { return !!h->format_tool_call; }))
    return;
  out.format_tool_call = [list](const LuaHooks::FormatToolCallContext &ctx)
      -> std::optional<std::string> {
    std::optional<std::string> result;
    for (const auto &h : list) {
      if (!h->format_tool_call)
        continue;
      auto r = h->format_tool_call(ctx);
      if (r)
        result = std::move(r);
    }
    return result;
  };
}

void compose_format_tool_result(
    const std::vector<std::shared_ptr<LuaHooks>> &list, LuaHooks &out) {
  // format_tool_result — last non-nil wins (override semantics)
  if (!std::ranges::any_of(
          list, [](const auto &h) { return !!h->format_tool_result; }))
    return;
  out.format_tool_result = [list](const LuaHooks::FormatToolResultContext &ctx)
      -> std::optional<std::string> {
    std::optional<std::string> result;
    for (const auto &h : list) {
      if (!h->format_tool_result)
        continue;
      auto r = h->format_tool_result(ctx);
      if (r)
        result = std::move(r);
    }
    return result;
  };
}

void compose_status_line(const std::vector<std::shared_ptr<LuaHooks>> &list,
                         LuaHooks &out) {
  // status_line — last non-nil wins (override semantics)
  if (!std::ranges::any_of(list,
                           [](const auto &h) { return !!h->status_line; }))
    return;
  out.status_line =
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

void compose_tab_title(const std::vector<std::shared_ptr<LuaHooks>> &list,
                       LuaHooks &out) {
  // tab_title — last non-nil wins (override semantics)
  if (!std::ranges::any_of(list, [](const auto &h) { return !!h->tab_title; }))
    return;
  out.tab_title =
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

void compose_complete(const std::vector<std::shared_ptr<LuaHooks>> &list,
                      LuaHooks &out) {
  // complete — union of all results
  if (!std::ranges::any_of(list, [](const auto &h) { return !!h->complete; }))
    return;
  out.complete =
      [list](
          std::string_view partial,
          const std::vector<Message> &transcript) -> std::vector<std::string> {
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

} // namespace

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
  compose_before_tool_call(list, *out);
  compose_on_event(list, *out);
  compose_prepare_context(list, *out);
  compose_after_tool_call(list, *out);
  compose_should_stop_after_turn(list, *out);
  compose_on_command(list, *out);
  compose_commands_and_tools(list, *out);
  compose_configure(list, *out);
  compose_prompt_line(list, *out);
  compose_format_tool_call(list, *out);
  compose_format_tool_result(list, *out);
  compose_status_line(list, *out);
  compose_tab_title(list, *out);
  compose_complete(list, *out);
  return out;
}

} // namespace pi::core
