#pragma once

#include "core/auth_types.h"

#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>

namespace pi::auth {

enum class AuthAvailability {
  configured,
  not_required,
  missing,
  expired_or_refresh_needed,
};

// Provider-neutral authentication boundary. Implementations may use a
// credential store or an external login protocol, but they return only the
// request-scoped value needed by one provider request.
class AuthenticationAdapter {
public:
  virtual ~AuthenticationAdapter() = default;

  virtual std::optional<core::RequestAuth>
  resolve(std::string_view provider, std::string_view explicit_api_key,
          const std::stop_token &stop_tok) const = 0;

  virtual AuthAvailability availability(std::string_view provider) const = 0;
};

class AuthenticationAdapterCollection {
public:
  using Adapter = std::shared_ptr<AuthenticationAdapter>;

  void register_adapter(std::string adapter_id, Adapter adapter);
  bool has_adapter(std::string_view adapter_id) const;
  Adapter get_adapter(std::string_view adapter_id) const;

private:
  mutable std::mutex mutex_;
  std::map<std::string, Adapter> adapters_;
};

} // namespace pi::auth
