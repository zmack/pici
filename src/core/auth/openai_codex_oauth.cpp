#include "core/auth/openai_codex_oauth.h"

#include "http/http_client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <future>
#include <httplib.h>
#include <initializer_list>
#include <limits>
#include <nlohmann/json.hpp>
#include <openssl/sha.h>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pi::auth {

namespace {

using json = nlohmann::json;
using core::HttpClient;

constexpr std::string_view kAlphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
constexpr std::string_view kJwtClaimPath = "https://api.openai.com/auth";

std::string percent_encode(std::string_view value) {
  constexpr std::array hex = {'0', '1', '2', '3', '4', '5', '6', '7',
                              '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
  std::string result;
  for (const unsigned char c : value) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' ||
        c == '~') {
      result.push_back(static_cast<char>(c));
    } else {
      result.push_back('%');
      result.push_back(hex[c >> 4]);
      result.push_back(hex[c & 0x0f]);
    }
  }
  return result;
}

std::string
form_body(std::initializer_list<std::pair<std::string_view, std::string_view>>
              fields) {
  std::string result;
  bool first = true;
  for (const auto &[key, value] : fields) {
    if (!first)
      result.push_back('&');
    first = false;
    result += percent_encode(key);
    result.push_back('=');
    result += percent_encode(value);
  }
  return result;
}

std::vector<std::byte> secure_random_bytes(std::size_t count) {
  const int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    throw std::runtime_error(
        "unable to open the operating-system random source");

  std::vector<std::byte> bytes(count);
  std::size_t offset = 0;
  while (offset < count) {
    const auto read_count = ::read(fd, bytes.data() + offset, count - offset);
    if (read_count < 0 && errno == EINTR)
      continue;
    if (read_count <= 0) {
      ::close(fd);
      throw std::runtime_error(
          "operating-system random source returned too few bytes");
    }
    offset += static_cast<std::size_t>(read_count);
  }
  ::close(fd);
  return bytes;
}

std::string random_alphabet_string(std::size_t count) {
  const auto bytes = secure_random_bytes(count);
  std::string result;
  result.reserve(count);
  for (const auto byte : bytes)
    result.push_back(
        kAlphabet[std::to_integer<unsigned char>(byte) % kAlphabet.size()]);
  return result;
}

std::string build_authorization_url(const OpenAICodexOAuthEndpoints &endpoints,
                                    std::string_view state,
                                    std::string_view challenge) {
  std::string url = endpoints.authorize_url;
  url += "?response_type=code&client_id=";
  url += percent_encode(endpoints.client_id);
  url += "&redirect_uri=";
  url += percent_encode(endpoints.redirect_uri);
  url += "&scope=";
  url += percent_encode(endpoints.scope);
  url += "&code_challenge=";
  url += percent_encode(challenge);
  url += "&code_challenge_method=S256&state=";
  url += percent_encode(state);
  url += "&id_token_add_organizations=true&codex_cli_simplified_flow=true";
  url += "&originator=pi";
  return url;
}

std::string safe_oauth_error(const HttpClient::Response &response,
                             std::string_view operation) {
  std::string message = "OpenAI Codex " + std::string(operation) +
                        " failed (HTTP " +
                        std::to_string(response.status_code) + ")";
  const auto parsed = json::parse(response.body, nullptr, false);
  if (parsed.is_object()) {
    if (auto it = parsed.find("error"); it != parsed.end()) {
      if (it->is_string())
        message += ": " + it->get<std::string>();
      else if (it->is_object() && it->contains("error_description") &&
               (*it)["error_description"].is_string())
        message += ": " + (*it)["error_description"].get<std::string>();
    }
  }
  if (message.size() > 512)
    message.resize(512);
  return message;
}

OAuthCredential parse_token_response(const HttpClient::Response &response,
                                     std::string_view operation) {
  if (response.status_code < 200 || response.status_code >= 300)
    throw std::runtime_error(safe_oauth_error(response, operation));
  const auto parsed = json::parse(response.body, nullptr, false);
  if (!parsed.is_object() || !parsed.contains("access_token") ||
      !parsed["access_token"].is_string() ||
      !parsed.contains("refresh_token") ||
      !parsed["refresh_token"].is_string() || !parsed.contains("expires_in") ||
      !parsed["expires_in"].is_number()) {
    throw std::runtime_error("OpenAI Codex " + std::string(operation) +
                             " response was missing token fields");
  }
  const auto access = parsed["access_token"].get<std::string>();
  const auto refresh = parsed["refresh_token"].get<std::string>();
  const auto expires_in = parsed["expires_in"].get<double>();
  if (access.empty() || refresh.empty() || !std::isfinite(expires_in) ||
      expires_in <= 0 || expires_in > 31'536'000) {
    throw std::runtime_error("OpenAI Codex " + std::string(operation) +
                             " response contained invalid token fields");
  }
  const auto account_id = extract_chatgpt_account_id(access);
  if (!account_id)
    throw std::runtime_error(
        "OpenAI Codex token did not contain a ChatGPT account ID");
  const auto expires_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      static_cast<std::int64_t>(expires_in * 1000.0);
  return OAuthCredential{.access_token = access,
                         .refresh_token = refresh,
                         .expires_at_ms = expires_ms,
                         .account_id = *account_id};
}

bool wait_interruptibly(std::chrono::milliseconds duration,
                        std::stop_token stop_tok) {
  constexpr auto slice = std::chrono::milliseconds(100);
  while (duration > std::chrono::milliseconds::zero()) {
    if (stop_tok.stop_requested())
      return false;
    const auto wait = std::min(duration, slice);
    std::this_thread::sleep_for(wait);
    duration -= wait;
  }
  return !stop_tok.stop_requested();
}

double parse_poll_interval(const json &value) {
  if (value.is_number())
    return value.get<double>();
  if (value.is_string()) {
    try {
      std::size_t parsed = 0;
      const auto text = value.get<std::string>();
      const auto interval = std::stod(text, &parsed);
      if (parsed == text.size())
        return interval;
    } catch (const std::exception &) {
      return std::numeric_limits<double>::quiet_NaN();
    }
  }
  return std::numeric_limits<double>::quiet_NaN();
}

} // namespace

std::string base64url_encode(std::string_view bytes) {
  std::string result;
  result.reserve((bytes.size() * 4 + 2) / 3);
  std::uint32_t accumulator = 0;
  int bits = 0;
  for (const unsigned char byte : bytes) {
    accumulator = (accumulator << 8) | byte;
    bits += 8;
    while (bits >= 6) {
      bits -= 6;
      result.push_back(kAlphabet[(accumulator >> bits) & 0x3f]);
    }
  }
  if (bits > 0)
    result.push_back(kAlphabet[(accumulator << (6 - bits)) & 0x3f]);
  return result;
}

std::optional<std::string> base64url_decode(std::string_view encoded) {
  std::array<int, 256> values{};
  values.fill(-1);
  for (std::size_t i = 0; i < kAlphabet.size(); ++i)
    values[static_cast<unsigned char>(kAlphabet[i])] = static_cast<int>(i);

  std::string result;
  result.reserve(encoded.size() * 3 / 4);
  std::uint32_t accumulator = 0;
  int bits = 0;
  for (const unsigned char byte : encoded) {
    if (values[byte] < 0)
      return std::nullopt;
    accumulator = (accumulator << 6) | static_cast<unsigned>(values[byte]);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      result.push_back(static_cast<char>((accumulator >> bits) & 0xff));
    }
  }
  if (bits >= 6 || (bits > 0 && (accumulator & ((1U << bits) - 1U)) != 0))
    return std::nullopt;
  return result;
}

PkcePair generate_pkce() {
  const auto random = random_alphabet_string(64);
  return PkcePair{.verifier = random, .challenge = pkce_challenge(random)};
}

std::string pkce_challenge(std::string_view verifier) {
  std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
  SHA256(reinterpret_cast<const unsigned char *>(verifier.data()),
         verifier.size(), digest.data());
  std::string digest_bytes(reinterpret_cast<const char *>(digest.data()),
                           digest.size());
  return base64url_encode(digest_bytes);
}

std::string generate_oauth_state() {
  const auto bytes = secure_random_bytes(16);
  return base64url_encode(std::string_view(
      reinterpret_cast<const char *>(bytes.data()), bytes.size()));
}

std::optional<std::string> extract_chatgpt_account_id(std::string_view jwt) {
  const auto first = jwt.find('.');
  if (first == std::string_view::npos || first == 0)
    return std::nullopt;
  const auto second = jwt.find('.', first + 1);
  if (second == std::string_view::npos || second == first + 1 ||
      second + 1 >= jwt.size() ||
      jwt.find('.', second + 1) != std::string_view::npos)
    return std::nullopt;
  auto payload = base64url_decode(jwt.substr(first + 1, second - first - 1));
  if (!payload)
    return std::nullopt;
  const auto parsed = json::parse(*payload, nullptr, false);
  if (!parsed.is_object())
    return std::nullopt;
  auto auth = parsed.find(std::string(kJwtClaimPath));
  if (auth == parsed.end() || !auth->is_object())
    return std::nullopt;
  auto account = auth->find("chatgpt_account_id");
  if (account == auth->end() || !account->is_string() || account->empty())
    return std::nullopt;
  return account->get<std::string>();
}

OpenAICodexOAuth::OpenAICodexOAuth(CredentialStore store,
                                   OpenAICodexOAuthEndpoints endpoints)
    : store_(std::move(store)), endpoints_(std::move(endpoints)) {}

OAuthCredential OpenAICodexOAuth::exchange_code(
    std::string_view code, std::string_view verifier,
    std::string_view redirect_uri, std::stop_token stop_tok) const {
  if (stop_tok.stop_requested())
    throw std::runtime_error("OpenAI Codex login was cancelled");
  const auto body = form_body({{"grant_type", "authorization_code"},
                               {"client_id", endpoints_.client_id},
                               {"code", code},
                               {"code_verifier", verifier},
                               {"redirect_uri", redirect_uri}});
  auto response =
      HttpClient::post(endpoints_.token_url, body,
                       {{"Content-Type", "application/x-www-form-urlencoded"},
                        {"Accept", "application/json"}},
                       std::nullopt, 60'000, stop_tok);
  if (!response)
    throw std::runtime_error("OpenAI Codex authorization exchange failed");
  return parse_token_response(*response, "authorization exchange");
}

OAuthCredential
OpenAICodexOAuth::login_browser(const OpenAICodexLoginOptions &options,
                                std::stop_token stop_tok) const {
  const auto pkce = generate_pkce();
  const auto state = generate_oauth_state();
  std::promise<std::string> code_promise;
  auto code_future = code_promise.get_future();
  std::atomic<bool> completed{false};

  httplib::Server server;
  server.Get("/auth/callback", [&](const httplib::Request &request,
                                   httplib::Response &response) {
    if (!request.has_param("state") ||
        request.get_param_value("state") != state ||
        !request.has_param("code")) {
      response.status = 400;
      response.set_content("Authentication callback rejected.", "text/plain");
      return;
    }
    if (!completed.exchange(true)) {
      code_promise.set_value(request.get_param_value("code"));
      response.status = 200;
      response.set_content(
          "OpenAI authentication completed. You can close this window.",
          "text/plain");
      server.stop();
    }
  });

  if (!server.bind_to_port("127.0.0.1", 1455))
    throw std::runtime_error(
        "cannot bind OpenAI callback port 1455; use --device instead");

  std::thread listener([&server] { server.listen_after_bind(); });
  if (options.notify)
    options.notify(build_authorization_url(endpoints_, state, pkce.challenge));

  const auto deadline = std::chrono::steady_clock::now() + options.timeout;
  while (code_future.wait_for(std::chrono::milliseconds(100)) !=
             std::future_status::ready &&
         std::chrono::steady_clock::now() < deadline &&
         !stop_tok.stop_requested()) {
  }
  server.stop();
  if (listener.joinable())
    listener.join();
  if (stop_tok.stop_requested())
    throw std::runtime_error("OpenAI Codex login was cancelled");
  if (code_future.wait_for(std::chrono::milliseconds(0)) !=
      std::future_status::ready)
    throw std::runtime_error("OpenAI Codex browser login timed out");
  return exchange_code(code_future.get(), pkce.verifier,
                       endpoints_.redirect_uri, stop_tok);
}

OAuthCredential
OpenAICodexOAuth::login_device(const OpenAICodexLoginOptions &options,
                               std::stop_token stop_tok) const {
  auto response = HttpClient::post(
      endpoints_.device_user_code_url,
      json{{"client_id", endpoints_.client_id}}.dump(),
      {{"Content-Type", "application/json"}, {"Accept", "application/json"}},
      std::nullopt, 60'000, stop_tok);
  if (!response)
    throw std::runtime_error("OpenAI Codex device-code request failed");
  if (response->status_code < 200 || response->status_code >= 300)
    throw std::runtime_error(
        safe_oauth_error(*response, "device-code request"));
  const auto parsed = json::parse(response->body, nullptr, false);
  if (!parsed.is_object() || !parsed.contains("device_auth_id") ||
      !parsed["device_auth_id"].is_string() || !parsed.contains("user_code") ||
      !parsed["user_code"].is_string())
    throw std::runtime_error("OpenAI Codex device-code response was malformed");
  const auto device_id = parsed["device_auth_id"].get<std::string>();
  const auto user_code = parsed["user_code"].get<std::string>();
  auto interval = parsed.contains("interval")
                      ? parse_poll_interval(parsed["interval"])
                      : 5.0;
  if (device_id.empty() || user_code.empty() || !std::isfinite(interval) ||
      interval < 0)
    throw std::runtime_error("OpenAI Codex device-code response was invalid");
  if (options.notify)
    options.notify("Open this URL and enter the code: " +
                   endpoints_.device_verification_uri + " " + user_code);

  const auto deadline = std::chrono::steady_clock::now() + options.timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (!wait_interruptibly(
            std::chrono::milliseconds(static_cast<int>(interval * 1000)),
            stop_tok))
      throw std::runtime_error("OpenAI Codex device login was cancelled");
    auto poll = HttpClient::post(
        endpoints_.device_token_url,
        json{{"device_auth_id", device_id}, {"user_code", user_code}}.dump(),
        {{"Content-Type", "application/json"}, {"Accept", "application/json"}},
        std::nullopt, 60'000, stop_tok);
    if (!poll)
      throw std::runtime_error("OpenAI Codex device-code polling failed");
    if (poll->status_code >= 200 && poll->status_code < 300) {
      const auto token = json::parse(poll->body, nullptr, false);
      if (!token.is_object() || !token.contains("authorization_code") ||
          !token["authorization_code"].is_string() ||
          !token.contains("code_verifier") ||
          !token["code_verifier"].is_string())
        throw std::runtime_error(
            "OpenAI Codex device token response was malformed");
      return exchange_code(token["authorization_code"].get<std::string>(),
                           token["code_verifier"].get<std::string>(),
                           endpoints_.device_redirect_uri, stop_tok);
    }
    const auto error = json::parse(poll->body, nullptr, false);
    std::string error_code;
    if (error.is_object() && error.contains("error")) {
      if (error["error"].is_string())
        error_code = error["error"].get<std::string>();
      else if (error["error"].is_object())
        error_code = error["error"].value("code", "");
    }
    if (error_code == "slow_down") {
      interval = std::min(interval * 2.0, 60.0);
      continue;
    }
    if (poll->status_code == 403 || poll->status_code == 404 ||
        error_code == "deviceauth_authorization_pending")
      continue;
    throw std::runtime_error(safe_oauth_error(*poll, "device-code polling"));
  }
  throw std::runtime_error("OpenAI Codex device login timed out");
}

OAuthCredential OpenAICodexOAuth::login(const OpenAICodexLoginOptions &options,
                                        std::stop_token stop_tok) const {
  if (options.mode == OpenAICodexLoginMode::device)
    return login_device(options, stop_tok);
  return login_browser(options, stop_tok);
}

OAuthCredential OpenAICodexOAuth::refresh(const OAuthCredential &credential,
                                          std::stop_token stop_tok) const {
  const auto body = form_body({{"grant_type", "refresh_token"},
                               {"refresh_token", credential.refresh_token},
                               {"client_id", endpoints_.client_id}});
  auto response =
      HttpClient::post(endpoints_.token_url, body,
                       {{"Content-Type", "application/x-www-form-urlencoded"},
                        {"Accept", "application/json"}},
                       std::nullopt, 60'000, stop_tok);
  if (!response)
    throw std::runtime_error("OpenAI Codex token refresh failed");
  return parse_token_response(*response, "token refresh");
}

std::optional<core::RequestAuth>
OpenAICodexOAuth::resolve(std::stop_token stop_tok) const {
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto current = store_.read_oauth("openai-codex");
  if (!current)
    return std::nullopt;

  const auto refresh_after = now + (5LL * 60LL * 1000LL);
  auto credential = current;
  if (current->expires_at_ms <= refresh_after) {
    credential = store_.modify_oauth(
        "openai-codex",
        [&](const std::optional<OAuthCredential> &locked_current)
            -> std::optional<OAuthCredential> {
          if (!locked_current)
            return std::nullopt;
          if (locked_current->expires_at_ms > refresh_after)
            return locked_current;
          return refresh(*locked_current, stop_tok);
        },
        stop_tok);
  }
  if (!credential)
    return std::nullopt;
  return core::RequestAuth{
      .kind = core::AuthKind::oauth,
      .bearer_token = credential->access_token,
      .headers = {{"chatgpt-account-id", credential->account_id}},
      .source = "pici auth"};
}

} // namespace pi::auth
