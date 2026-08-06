#pragma once

#include <map>
#include <optional>
#include <string>

namespace pi::core {

enum class AuthKind {
  none,
  api_key,
  oauth,
};

// RequestAuth contains only the already-resolved authentication material for
// one request. It must never be serialized into sessions or diagnostic events.
struct RequestAuth {
  AuthKind kind{AuthKind::none};
  std::optional<std::string> bearer_token;
  std::map<std::string, std::string> headers;
  std::string source;
};

} // namespace pi::core
