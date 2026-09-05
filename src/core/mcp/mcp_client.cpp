#include "core/mcp/mcp_client.h"
#include "core/mcp/mcp_tool.h"

#include <iostream>
#include <nlohmann/json-schema.hpp>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace pi::core::mcp {

void McpClient::initialize(std::chrono::milliseconds timeout) {
  const nlohmann::json params{
      {"protocolVersion", "2024-11-05"},
      // Phase 1 declares no sampling/roots/elicitation capability. This
      // makes a server less likely to send those request types, but does
      // NOT make it impossible (a non-compliant or eager server may send
      // one anyway) -- mcp_transport.cpp's reader loop replies to any
      // inbound server-initiated request with a stub "method not found"
      // regardless of what we declare here.
      {"capabilities", nlohmann::json::object()},
      {"clientInfo", {{"name", "pici"}, {"version", PI_CPP_VERSION}}},
  };

  nlohmann::json response;
  try {
    response =
        transport_.call(std::string(methods::kInitialize), params, timeout);
  } catch (const std::exception &err) {
    throw std::runtime_error(std::string("mcp initialize failed: ") +
                             err.what());
  }

  if (!response.is_object() || !response.contains("protocolVersion") ||
      !response.contains("capabilities")) {
    throw std::runtime_error(
        "mcp initialize failed: malformed handshake response (missing "
        "protocolVersion/capabilities)");
  }

  // Notification: no reply expected. If the transport is already dead this
  // throws TransportError, which is a std::runtime_error and propagates as
  // the same "initialize failed" family of error.
  transport_.notify(std::string(methods::kInitialized),
                    nlohmann::json::object());
  initialized_ = true;
}

namespace {

// Throwaway validator used purely to prove `input_schema` is a loadable
// JSON Schema before letting it construct an McpTool. A schema that throws
// here (e.g. an unresolved $ref) would otherwise reach
// agent_loop.cpp:268's unguarded tool->schema().validate_arguments() call
// at actual tool-call time -- failing the entire turn instead of just this
// one tool -- and would separately poison tool_validation.cpp's
// process-lifetime validator cache (see that file's get_or_create_validator
// fix). Discovery time is the only place this can be caught cheaply and
// per-tool.
bool schema_is_loadable(const nlohmann::json &input_schema) {
  try {
    nlohmann::json_schema::json_validator validator;
    validator.set_root_schema(input_schema);
    return true;
  } catch (const std::exception &) {
    return false;
  }
}

} // namespace

std::vector<std::shared_ptr<const ToolDefinition>>
McpClient::list_tools(std::chrono::milliseconds timeout) {
  const auto response =
      transport_.call(std::string(methods::kToolsList),
                      nlohmann::json::object(), timeout);
  if (!response.is_object() || !response.contains("tools") ||
      !response.at("tools").is_array()) {
    throw std::runtime_error(
        "mcp tools/list failed: malformed response (missing tools array) "
        "from server \"" +
        server_name_ + "\"");
  }

  std::vector<std::shared_ptr<const ToolDefinition>> tools;
  // Seeded with builtin names so a sanitized/qualified MCP tool name can
  // never silently shadow a builtin (find_tool()'s lookup is name-based,
  // agent_loop.cpp:222) -- extended with each survivor's own name below so
  // siblings from the SAME server that sanitize/truncate to a collision are
  // also caught, not just builtin collisions.
  std::unordered_set<std::string> taken_names;
  for (auto &name : builtin_tool_names())
    taken_names.insert(std::move(name));

  for (const auto &entry : response.at("tools")) {
    if (!entry.is_object() || !entry.contains("name") ||
        !entry.at("name").is_string()) {
      std::cerr << "mcp: server \"" << server_name_
                << "\" advertised a tool with no string \"name\"; skipping\n";
      continue;
    }
    const auto tool_name = entry.at("name").get<std::string>();
    const auto description = entry.value("description", std::string{});
    const nlohmann::json input_schema =
        entry.contains("inputSchema") ? entry.at("inputSchema")
                                      : nlohmann::json::object();

    if (!schema_is_loadable(input_schema)) {
      std::cerr << "mcp: server \"" << server_name_ << "\" tool \""
                << tool_name
                << "\" has a malformed inputSchema (not a loadable JSON "
                   "Schema); skipping this tool\n";
      continue;
    }

    const auto qualified_name = sanitize_mcp_tool_name(server_name_, tool_name);
    if (!claim_tool_name(qualified_name, server_name_, taken_names))
      continue;

    tools.push_back(std::make_shared<McpTool>(
        server_name_, tool_name, description, input_schema,
        shared_from_this()));
  }

  return tools;
}

nlohmann::json McpClient::call_tool(const std::string &tool_name,
                                    nlohmann::json arguments,
                                    std::chrono::milliseconds timeout) {
  const nlohmann::json params{
      {"name", tool_name},
      {"arguments", std::move(arguments)},
  };
  return transport_.call(std::string(methods::kToolsCall), params, timeout);
}

} // namespace pi::core::mcp
