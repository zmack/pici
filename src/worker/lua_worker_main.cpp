// A disposable OS child, never linked to Pici's provider/session machinery.
#include <nlohmann/json.hpp>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
int luaopen_lfs(lua_State *L);
}

#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
using Json = nlohmann::json;
constexpr std::size_t kMaxFrame = 1024UL * 1024;
void *json_null_marker() {
  static char marker{};
  return &marker;
}

void limit(int resource, rlim_t value) {
  const rlimit bound{value, value};
  if (setrlimit(resource, &bound) != 0)
    throw std::runtime_error("cannot install worker resource limit");
}

void mark_json_array(lua_State *L, int index) {
  index = lua_absindex(L, index);
  if (luaL_newmetatable(L, "pici.json.array") != 0) {
    lua_pushliteral(L, "array");
    lua_setfield(L, -2, "__jsontype");
  }
  lua_setmetatable(L, index);
}

bool is_json_array(lua_State *L, int index) {
  if (lua_getmetatable(L, index) == 0)
    return false;
  lua_pushliteral(L, "__jsontype");
  lua_rawget(L, -2); // Never invoke user metamethods during serialization.
  std::size_t size = 0;
  const char *type = lua_tolstring(L, -1, &size);
  const bool array = type != nullptr && std::string_view(type, size) == "array";
  lua_pop(L, 2);
  return array;
}

int json_array(lua_State *L) {
  if (lua_gettop(L) == 0)
    lua_newtable(L);
  luaL_checktype(L, 1, LUA_TTABLE);
  lua_settop(L, 1);
  mark_json_array(L, 1);
  return 1;
}

// Traversals are bounded to 32 levels, including cycles, with Lua stack checks.
// NOLINTNEXTLINE(misc-no-recursion)
Json lua_value_to_json(lua_State *L, int index, int depth = 0);
// NOLINTNEXTLINE(misc-no-recursion)
Json lua_table_to_json(lua_State *L, int index, int depth) {
  std::size_t count = 0;
  bool array = true;
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    ++count;
    if (count > 4096)
      throw std::runtime_error("too many JSON table entries");
    array =
        array && lua_isinteger(L, -2) != 0 && lua_tointeger(L, -2) > 0 &&
        static_cast<std::size_t>(lua_tointeger(L, -2)) <= lua_rawlen(L, index);
    lua_pop(L, 1);
  }
  const bool marked_array = is_json_array(L, index);
  if (marked_array && (!array || count != lua_rawlen(L, index)))
    throw std::runtime_error(
        "JSON arrays must contain only dense integer keys");
  if (array && (count > 0 || marked_array) && count == lua_rawlen(L, index)) {
    Json result = Json::array();
    for (std::size_t i = 1; i <= count; ++i) {
      lua_rawgeti(L, index, static_cast<lua_Integer>(i));
      result.push_back(lua_value_to_json(L, -1, depth + 1));
      lua_pop(L, 1);
    }
    return result;
  }
  Json result = Json::object();
  lua_pushnil(L);
  while (lua_next(L, index) != 0) {
    if (lua_type(L, -2) != LUA_TSTRING)
      throw std::runtime_error(
          "JSON object keys must be strings; arrays must be dense");
    std::size_t size = 0;
    const char *key = lua_tolstring(L, -2, &size);
    result[std::string(key, size)] = lua_value_to_json(L, -1, depth + 1);
    lua_pop(L, 1);
  }
  return result;
}

// NOLINTNEXTLINE(misc-no-recursion)
Json lua_value_to_json(lua_State *L, int index, int depth) {
  if (depth > 32 || lua_checkstack(L, 8) == 0)
    throw std::runtime_error("JSON nesting exceeds 32 (or cyclic value)");
  index = lua_absindex(L, index);
  switch (lua_type(L, index)) {
  case LUA_TNIL:
    return nullptr;
  case LUA_TBOOLEAN:
    return lua_toboolean(L, index) != 0;
  case LUA_TNUMBER:
    if (lua_isinteger(L, index) != 0)
      return lua_tointeger(L, index);
    if (!std::isfinite(lua_tonumber(L, index)))
      throw std::runtime_error("non-finite JSON number");
    return lua_tonumber(L, index);
  case LUA_TSTRING: {
    std::size_t size = 0;
    const auto *text = lua_tolstring(L, index, &size);
    if (size > kMaxFrame)
      throw std::runtime_error("JSON string too large");
    return std::string(text, size);
  }
  case LUA_TLIGHTUSERDATA:
    if (lua_touserdata(L, index) == json_null_marker())
      return nullptr;
    break;
  case LUA_TTABLE:
    return lua_table_to_json(L, index, depth);
  default:
    break;
  }
  throw std::runtime_error("value is not JSON-compatible");
}

// Depth-capped JSON trees; no graph references can arrive here.
// NOLINTNEXTLINE(misc-no-recursion)
void push_json_value(lua_State *L, const Json &value, int depth = 0) {
  if (depth > 32 || lua_checkstack(L, 8) == 0)
    throw std::runtime_error("JSON nesting exceeds 32");
  if (value.is_null())
    lua_pushlightuserdata(L, json_null_marker());
  else if (value.is_boolean())
    lua_pushboolean(L, value.get<bool>() ? 1 : 0);
  else if (value.is_number_unsigned() &&
           value.get<std::uint64_t>() >
               static_cast<std::uint64_t>(
                   std::numeric_limits<lua_Integer>::max()))
    throw std::runtime_error("JSON integer exceeds Lua integer range");
  else if (value.is_number_integer())
    lua_pushinteger(L, value.get<lua_Integer>());
  else if (value.is_number())
    lua_pushnumber(L, value.get<double>());
  else if (value.is_string()) {
    const auto &text = value.get_ref<const std::string &>();
    lua_pushlstring(L, text.data(), text.size());
  } else {
    lua_newtable(L);
    if (value.is_array())
      mark_json_array(L, -1);
    lua_Integer position = 1;
    for (auto it = value.begin(); it != value.end(); ++it) {
      push_json_value(L, it.value(), depth + 1);
      if (value.is_array())
        lua_rawseti(L, -2, position++);
      else {
        lua_pushlstring(L, it.key().data(), it.key().size());
        lua_insert(L, -2);
        lua_rawset(L, -3);
      }
    }
  }
}

int encode(lua_State *L) {
  try {
    const auto text = lua_value_to_json(L, 1).dump();
    if (text.size() > kMaxFrame)
      throw std::runtime_error("encoded JSON exceeds 1 MiB");
    lua_pushlstring(L, text.data(), text.size());
    return 1;
  } catch (const std::exception &error) {
    lua_pushstring(L, error.what());
  }
  return lua_error(L);
}

int decode(lua_State *L) {
  std::size_t size = 0;
  const char *text = luaL_checklstring(L, 1, &size);
  try {
    if (size > kMaxFrame)
      throw std::runtime_error("JSON input exceeds 1 MiB");
    const auto parsed =
        Json::parse(std::string_view(text, size),
                    [](int depth, Json::parse_event_t, Json &) {
                      if (depth > 32)
                        throw std::runtime_error("JSON nesting exceeds 32");
                      return true;
                    });
    push_json_value(L, parsed);
    return 1;
  } catch (const std::exception &error) {
    lua_pushstring(L, error.what());
  }
  return lua_error(L);
}

int require_module(lua_State *L) {
  const char *name = luaL_checkstring(L, 1);
  lua_getfield(L, lua_upvalueindex(1), name);
  if (lua_isnil(L, -1))
    return luaL_error(L, "module '%s' was not declared in requires", name);
  return 1;
}

void environment(lua_State *L, const Json &libraries) {
  const std::array<luaL_Reg, 6> core{{{LUA_GNAME, luaopen_base},
                                      {LUA_TABLIBNAME, luaopen_table},
                                      {LUA_STRLIBNAME, luaopen_string},
                                      {LUA_MATHLIBNAME, luaopen_math},
                                      {LUA_UTF8LIBNAME, luaopen_utf8},
                                      {LUA_COLIBNAME, luaopen_coroutine}}};
  for (const auto &entry : core) {
    luaL_requiref(L, entry.name, entry.func, 1);
    lua_pop(L, 1);
  }
  for (const char *name : {"dofile", "loadfile", "load", "warn"}) {
    lua_pushnil(L);
    lua_setglobal(L, name);
  }
  lua_newtable(L); // inaccessible approved-module table held by require closure
  for (const char *name : {"string", "table", "math", "utf8", "coroutine"}) {
    lua_getglobal(L, name);
    lua_setfield(L, -2, name);
  }
  for (const auto &entry : libraries) {
    const auto name = entry.get<std::string>();
    constexpr std::array<luaL_Reg, 3> optional{
        {{"io", luaopen_io}, {"os", luaopen_os}, {"lfs", luaopen_lfs}}};
    const auto *const module = std::ranges::find_if(
        optional, [&](const auto &entry) { return name == entry.name; });
    if (module == optional.end())
      throw std::runtime_error("unsupported library");
    luaL_requiref(L, name.c_str(), module->func, 1);
    lua_setfield(L, -2, name.c_str());
  }
  lua_newtable(L);
  lua_pushcfunction(L, encode);
  lua_setfield(L, -2, "encode");
  lua_pushcfunction(L, decode);
  lua_setfield(L, -2, "decode");
  lua_pushcfunction(L, json_array);
  lua_setfield(L, -2, "array");
  lua_pushlightuserdata(L, json_null_marker());
  lua_setfield(L, -2, "null");
  lua_pushvalue(L, -1);
  lua_setglobal(L, "json");
  lua_setfield(L, -2, "json");
  lua_pushcclosure(L, require_module, 1);
  lua_setglobal(L, "require");
}

Json run(const Json &request) {
  lua_State *L = luaL_newstate();
  if (L == nullptr)
    throw std::runtime_error("cannot create Lua state");
  const std::unique_ptr<lua_State, decltype(&lua_close)> closer(L, lua_close);
  const auto source = request.at("script").get<std::string>();
  if (source.size() > 65536)
    throw std::runtime_error("source exceeds 64 KiB");
  const bool check = request.value("mode", "run") == "check";
  if (luaL_loadbufferx(L, source.data(), source.size(), "=script", "t") !=
      LUA_OK)
    return {{"ok", false}, {"error", lua_tostring(L, -1)}};
  if (check)
    return {{"ok", true}, {"values", Json::array()}, {"checked", true}};
  environment(L, request.at("requires"));
  if (lua_pcall(L, 0, LUA_MULTRET, 0) != LUA_OK) {
    const char *error = lua_tostring(L, -1);
    return {{"ok", false},
            {"error", error != nullptr ? error : "non-string Lua error"}};
  }
  Json values = Json::array();
  const int count = lua_gettop(L);
  for (int i = 1; i <= count; ++i)
    values.push_back(lua_value_to_json(L, i));
  return {{"ok", true}, {"values", std::move(values)}};
}
} // namespace

int main() {
  Json result;
  try {
    limit(RLIMIT_CORE, 0);
    limit(RLIMIT_FSIZE, 256UL * 1024 * 1024);
    limit(RLIMIT_NOFILE, 128);
    limit(RLIMIT_CPU, 120);
    limit(RLIMIT_NPROC, 256);
    std::string input;
    char ch{};
    while (std::cin.get(ch)) {
      if (input.size() >= 128UL * 1024)
        throw std::runtime_error("worker request exceeds 128 KiB");
      input.push_back(ch);
    }
    const auto request = Json::parse(input);
    const auto address_space_mb =
        request.at("address_space_mb").get<std::uint64_t>();
    if (address_space_mb > 65536 ||
        (address_space_mb != 0 && address_space_mb < 64))
      throw std::runtime_error("invalid address-space limit");
    if (address_space_mb != 0)
      limit(RLIMIT_AS, address_space_mb * 1024 * 1024);
    result = run(request);
  } catch (const std::exception &error) {
    result = {{"ok", false}, {"error", error.what()}};
  }
  std::string frame;
  try {
    result["runtime"] = {{"lua", LUA_RELEASE}, {"lfs", "1.8.0"}};
    frame = result.dump();
    if (frame.size() > kMaxFrame)
      frame = R"({"ok":false,"error":"result exceeds 1 MiB"})";
  } catch (...) {
    frame = R"({"ok":false,"error":"result is not valid JSON/UTF-8"})";
  }
  std::ofstream output("/pici/result", std::ios::binary | std::ios::trunc);
  output.write(frame.data(), static_cast<std::streamsize>(frame.size()));
  return output ? 0 : 1;
}
