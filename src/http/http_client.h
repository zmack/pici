#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>

namespace pi::core {

class StreamDiagnostics;

class HttpClient {
public:
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

  static bool
  post_streaming(const std::string &url, const std::string &body,
                 std::function<void(const std::string &line)> on_line,
                 const std::map<std::string, std::string> &extra_headers = {},
                 const std::optional<std::string> &api_key = std::nullopt,
                 std::optional<std::uint32_t> timeout_ms = std::nullopt,
                 std::stop_token stop_tok = std::stop_token{},
                 std::shared_ptr<StreamDiagnostics> diagnostics = {});
};

} // namespace pi::core
