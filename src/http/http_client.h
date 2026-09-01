#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

#include "core/auth_types.h"

namespace pi::core {

class StreamDiagnostics;

class HttpClient {
public:
  using ResponseCallback =
      std::function<void(int, const std::map<std::string, std::string> &)>;
  struct Response {
    int status_code{0};
    std::string body;
    std::map<std::string, std::string> headers;
  };

  static std::optional<Response>
  post(const std::string &url, const std::string &body,
       const std::map<std::string, std::string> &extra_headers = {},
       const std::optional<std::string> &api_key = std::nullopt,
       std::optional<std::uint32_t> timeout_ms = std::nullopt,
       std::stop_token stop_tok = std::stop_token{});

  static std::optional<Response>
  post_authenticated(const std::string &url, const std::string &body,
                     const std::map<std::string, std::string> &extra_headers,
                     const std::optional<RequestAuth> &auth,
                     std::optional<std::uint32_t> timeout_ms = std::nullopt,
                     std::stop_token stop_tok = std::stop_token{});

  static std::optional<Response>
  get_authenticated(const std::string &url,
                    const std::map<std::string, std::string> &extra_headers,
                    const std::optional<RequestAuth> &auth,
                    std::optional<std::uint32_t> timeout_ms = std::nullopt,
                    std::stop_token stop_tok = std::stop_token{});

  static bool
  post_streaming(const std::string &url, const std::string &body,
                 std::function<void(const std::string &line)> on_line,
                 const std::map<std::string, std::string> &extra_headers = {},
                 const std::optional<std::string> &api_key = std::nullopt,
                 std::optional<std::uint32_t> timeout_ms = std::nullopt,
                 std::stop_token stop_tok = std::stop_token{},
                 std::shared_ptr<StreamDiagnostics> diagnostics = {});

  static bool post_streaming_authenticated(
      const std::string &url, const std::string &body,
      std::function<void(const std::string &line)> on_line,
      const std::map<std::string, std::string> &extra_headers,
      const std::optional<RequestAuth> &auth,
      std::optional<std::uint32_t> timeout_ms = std::nullopt,
      std::stop_token stop_tok = std::stop_token{},
      std::shared_ptr<StreamDiagnostics> diagnostics = {},
      const ResponseCallback &on_response = {});
};

} // namespace pi::core
