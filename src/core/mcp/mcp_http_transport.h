#pragma once

// McpHttpTransport: Adapter speaking MCP's streamable-HTTP transport. Each
// request is one POST of a JSON-RPC message; the server answers with either
// a single application/json body or a text/event-stream whose events carry
// the response (and possibly notifications / server requests first). One
// libcurl easy handle per exchange, so concurrent callers never share
// transfer state; the only shared state is the negotiated session id and
// protocol version.
//
// Not implemented (follow-up phases): the optional standalone GET event
// stream (so `notifications/tools/list_changed` is only observed when it
// arrives inside a POST reply stream), SSE resumption (Last-Event-ID), and
// server-scoped OAuth.
//
// Credentials: the bearer token and configured headers live only in this
// object. They are never part of an error message, and URLs are reported in
// redacted form (scheme://host[:port]/path, no userinfo/query/fragment).
// Redirects are not followed so an Authorization header cannot leak to
// another origin.

#include "core/mcp/mcp_transport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pi::core::mcp {

// Hard cap on the bytes accepted for one reply (JSON body or whole SSE
// stream). A longer reply aborts the exchange with a TransportError.
inline constexpr std::size_t kDefaultMcpHttpMaxResponseBytes = 8U * 1024 * 1024;

struct McpHttpConfig {
  std::string url;
  // Extra non-secret request headers. CR/LF in names or values is rejected
  // at construction.
  std::map<std::string, std::string> headers;
  // Sent as `Authorization: Bearer <token>` when set. Held in memory only.
  std::optional<std::string> bearer_token;
  std::size_t max_response_bytes{kDefaultMcpHttpMaxResponseBytes};
};

class McpHttpTransport final : public McpTransport {
public:
  // Throws std::runtime_error for a non-http(s) URL, a URL with userinfo, or
  // a header containing CR/LF. Does not touch the network.
  explicit McpHttpTransport(McpHttpConfig config);
  // Best-effort DELETE to terminate the session, if one was assigned.
  ~McpHttpTransport() override;

  McpHttpTransport(const McpHttpTransport &) = delete;
  McpHttpTransport &operator=(const McpHttpTransport &) = delete;
  McpHttpTransport(McpHttpTransport &&) = delete;
  McpHttpTransport &operator=(McpHttpTransport &&) = delete;

  using McpTransport::call;
  nlohmann::json call(const std::string &method, nlohmann::json params,
                      std::chrono::milliseconds timeout,
                      std::stop_token stop_tok) override;
  void notify(const std::string &method, nlohmann::json params) override;
  void set_notification_callback(NotificationCallback cb) override;

  // False after the server reported the session gone (HTTP 404 on a request
  // that carried a session id) or after close.
  bool is_alive() const override { return alive_.load(); }

  // Redacted endpoint, safe for logs and error messages.
  const std::string &display_url() const { return display_url_; }

private:
  struct Exchange;

  Exchange post(const nlohmann::json &message,
                std::chrono::milliseconds timeout,
                const std::stop_token &stop_tok, std::optional<RequestId> want,
                const std::string &method);
  void send_server_request_errors(const std::vector<nlohmann::json> &ids);

  McpHttpConfig config_;
  std::string display_url_;
  std::atomic<std::int64_t> next_id_{1};
  std::atomic<bool> alive_{true};

  mutable std::mutex state_mutex_;
  std::string session_id_;
  std::string protocol_version_;
  NotificationCallback notification_callback_;
};

} // namespace pi::core::mcp
