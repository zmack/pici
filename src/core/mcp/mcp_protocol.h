#pragma once

// JSON-RPC 2.0 envelope types and MCP method-name constants shared by
// McpStdioTransport and McpClient. Header-only: everything here is either a
// plain struct or a small inline function.
//
// The discriminator below (parse_inbound_message) encodes a protocol rule
// that is easy to get subtly wrong: MCP servers can send *requests* to the
// client (sampling/createMessage, roots/list, elicitation/create), and those
// requests carry both an `id` and a `method`. The server mints its own ids
// independently of the client's own outgoing request-id counter, so the two
// id spaces can and do collide. Discriminating on `id` presence alone --
// "has id => it's a response" -- would wrongly treat a server-initiated
// request as a reply to one of our own pending calls whenever the two
// numbers happened to coincide, fulfilling the wrong promise with garbage.
// The correct rule checks `method` presence FIRST:
//
//   method + id present  -> inbound request (the server is calling us)
//   method, no id         -> notification
//   no method, id present -> response to one of OUR OWN outstanding requests
//
// See mcp_transport.h's reader-thread handling and
// test/test_mcp_transport.cpp's
// HandlesServerInitiatedRequestWithMethodAndNotFoundReply for the regression
// proof.

#include <cstdint>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace pi::core::mcp {

namespace methods {

inline constexpr std::string_view kInitialize = "initialize";
inline constexpr std::string_view kInitialized = "notifications/initialized";
inline constexpr std::string_view kToolsList = "tools/list";
inline constexpr std::string_view kToolsCall = "tools/call";
inline constexpr std::string_view kCancelled = "notifications/cancelled";

} // namespace methods

// Ids we mint for our own outgoing requests. Always non-negative, monotonic
// integers (see McpStdioTransport::call) -- never string ids, which lets the
// discriminator below reject any inbound "response" whose id isn't an
// integer as unrecognized rather than mis-binding it.
using RequestId = std::int64_t;

// A request WE send to the server (tools/call, initialize, ...).
struct JsonRpcRequest {
  RequestId id{};
  std::string method;
  nlohmann::json params = nlohmann::json::object();
};

// A reply to one of OUR OWN outstanding requests.
struct JsonRpcResponse {
  RequestId id{};
  std::optional<nlohmann::json> result;
  std::optional<nlohmann::json> error;
};

// A notification (no id, no reply expected) in either direction.
struct JsonRpcNotification {
  std::string method;
  nlohmann::json params = nlohmann::json::object();
};

// A request the SERVER sends TO us (sampling/createMessage, roots/list,
// elicitation/create, ...). Deliberately a distinct type from
// JsonRpcRequest: this `id` lives in the server's own id space, which is
// independent of (and may numerically collide with) the ids we mint for our
// own outgoing JsonRpcRequests. Stored as raw json rather than RequestId
// because the spec allows either a number or a string id here, and we must
// echo it back verbatim in our stub reply regardless of its type.
struct InboundServerRequest {
  nlohmann::json id;
  std::string method;
  nlohmann::json params = nlohmann::json::object();
};

using InboundMessage =
    std::variant<JsonRpcResponse, JsonRpcNotification, InboundServerRequest>;

// Discriminates one already-parsed inbound JSON object per the rule
// described in the file comment above. Returns nullopt for anything that
// isn't a JSON object, or whose shape doesn't match any of the three cases
// (e.g. a "response" whose id isn't an integer, which cannot be a reply to
// any request we sent) -- callers should treat nullopt as "drop this line",
// not as fatal.
inline std::optional<InboundMessage>
parse_inbound_message(const nlohmann::json &msg) {
  if (!msg.is_object())
    return std::nullopt;

  const bool has_method =
      msg.contains("method") && msg.at("method").is_string();
  const bool has_id = msg.contains("id") && !msg.at("id").is_null();

  // method + id present -> inbound request. Checked FIRST, deliberately,
  // regardless of what `id` looks like -- this is the fix: a server request
  // must never fall through to being treated as a response just because its
  // id happens to look like one of ours.
  if (has_method && has_id) {
    InboundServerRequest req;
    req.id = msg.at("id");
    req.method = msg.at("method").get<std::string>();
    if (msg.contains("params"))
      req.params = msg.at("params");
    return InboundMessage{std::move(req)};
  }

  // method present, no id -> notification.
  if (has_method) {
    JsonRpcNotification note;
    note.method = msg.at("method").get<std::string>();
    if (msg.contains("params"))
      note.params = msg.at("params");
    return InboundMessage{std::move(note)};
  }

  // no method, id present -> a response to one of our own requests. Our own
  // ids are always emitted as JSON integers (RequestId), so an id of any
  // other shape here cannot be a reply to anything we sent.
  if (has_id) {
    if (!msg.at("id").is_number_integer())
      return std::nullopt;
    JsonRpcResponse resp;
    resp.id = msg.at("id").get<RequestId>();
    if (msg.contains("result"))
      resp.result = msg.at("result");
    if (msg.contains("error"))
      resp.error = msg.at("error");
    return InboundMessage{std::move(resp)};
  }

  return std::nullopt;
}

} // namespace pi::core::mcp
