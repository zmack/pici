#pragma once

#include <cctype>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace pi::core {

enum class AuthKind {
  none,
  api_key,
  oauth,
};

// RequestAuth contains only the already-resolved authentication material for
// one request. It must never be serialized into sessions or diagnostic events.
inline bool same_header_name(std::string_view left, std::string_view right) {
  if (left.size() != right.size())
    return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i])))
      return false;
  }
  return true;
}

inline void merge_headers_case_insensitive(
    std::map<std::string, std::string> &target,
    const std::map<std::string, std::string> &source) {
  for (const auto &[key, value] : source) {
    for (auto it = target.begin(); it != target.end(); ++it) {
      if (same_header_name(it->first, key)) {
        target.erase(it);
        break;
      }
    }
    target[key] = value;
  }
}

struct RequestAuth {
  AuthKind kind{AuthKind::none};
  std::optional<std::string> bearer_token;
  std::map<std::string, std::string> headers;
  std::string source;
};

} // namespace pi::core
