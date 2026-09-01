#include "http/http_client.h"
#include "core/auth_types.h"
#include "core/stream_diagnostics.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <curl/curl.h>       // NOLINT(misc-include-cleaner)
#include <nlohmann/json.hpp> // NOLINT(misc-include-cleaner)
#include <utility>

namespace pi::core {

namespace {

constexpr std::size_t kMaxStreamingErrorBody =
    static_cast<std::size_t>(64U * 1024U);

struct CurlHandle {
  CURL *handle{nullptr};
  CurlHandle() : handle(curl_easy_init()) {} // NOLINT(misc-include-cleaner)
  ~CurlHandle() {
    if (handle != nullptr) {
      curl_easy_cleanup(handle); // NOLINT(misc-include-cleaner)
    }
  }

  CurlHandle(const CurlHandle &) = delete;
  CurlHandle &operator=(const CurlHandle &) = delete;
};

struct CurlHeaders {
  curl_slist *list{nullptr};
  CurlHeaders() = default;
  ~CurlHeaders() { curl_slist_free_all(list); }

  void append(const std::string &header) {
    list = curl_slist_append(list, header.c_str());
  }

  CurlHeaders(const CurlHeaders &) = delete;
  CurlHeaders &operator=(const CurlHeaders &) = delete;
};

int stop_token_progress(void *p, curl_off_t, // NOLINT(misc-include-cleaner)
                        curl_off_t, curl_off_t, curl_off_t) {
  return static_cast<const std::stop_token *>(p)->stop_requested() ? 1 : 0;
}

std::string error_line(const std::string &message) {
  return nlohmann::json{
      {"error", {{"message", message}}},
  }
      .dump();
}

bool same_header_name(std::string_view left, std::string_view right) {
  if (left.size() != right.size())
    return false;
  for (std::size_t i = 0; i < left.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(left[i])) !=
        std::tolower(static_cast<unsigned char>(right[i])))
      return false;
  }
  return true;
}

void append_authenticated_headers(
    CurlHeaders &headers,
    const std::map<std::string, std::string> &extra_headers,
    const std::optional<RequestAuth> &auth, std::string_view default_accept) {
  std::vector<std::pair<std::string, std::string>> merged;
  auto set_header = [&](std::string key, std::string value) {
    for (auto &existing : merged) {
      if (same_header_name(existing.first, key)) {
        existing.first = std::move(key);
        existing.second = std::move(value);
        return;
      }
    }
    merged.emplace_back(std::move(key), std::move(value));
  };

  set_header("Content-Type", "application/json");
  set_header("Accept", std::string(default_accept));
  for (const auto &[key, value] : extra_headers)
    set_header(key, value);
  if (auth) {
    for (const auto &[key, value] : auth->headers)
      set_header(key, value);
    if (auth->bearer_token)
      set_header("Authorization", "Bearer " + *auth->bearer_token);
  }
  for (const auto &[key, value] : merged) {
    std::string header = key;
    header += ": ";
    header += value;
    headers.append(header);
  }
}

} // namespace

std::optional<HttpClient::Response>
HttpClient::post(const std::string &url, const std::string &body,
                 const std::map<std::string, std::string> &extra_headers,
                 const std::optional<std::string> &api_key,
                 std::optional<std::uint32_t> timeout_ms,
                 std::stop_token stop_tok) {

  CurlHandle curl;
  if (curl.handle == nullptr)
    return std::nullopt;

  Response result;

  CurlHeaders headers;

  if (api_key) {
    std::string auth = "Authorization: Bearer ";
    auth += *api_key;
    headers.append(auth);
  }

  for (const auto &[key, value] : extra_headers) {
    std::string header;
    header.reserve(key.size() + value.size() + 2);
    header += key;
    header += ": ";
    header += value;
    headers.append(header);
  }

  // Defaults for any headers the caller didn't set.
  if (!extra_headers.contains("Content-Type"))
    headers.append("Content-Type: application/json");
  if (!extra_headers.contains("Accept"))
    headers.append("Accept: application/json");

  curl_easy_setopt(curl.handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_POST, 1L);
  curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers.list);
  curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT_MS,
                   (long)timeout_ms.value_or(600000));

  curl_easy_setopt(curl.handle, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.handle, CURLOPT_XFERINFOFUNCTION, stop_token_progress);
  curl_easy_setopt(curl.handle, CURLOPT_XFERINFODATA,
                   const_cast<std::stop_token *>(&stop_tok));

  curl_easy_setopt(
      curl.handle, CURLOPT_WRITEFUNCTION,
      +[](char *ptr, size_t size, size_t nmemb, void *data) -> size_t {
        auto *resp = static_cast<HttpClient::Response *>(data);
        resp->body.append(ptr, size * nmemb);
        return size * nmemb;
      });
  curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &result);

  CURLcode res = curl_easy_perform(curl.handle); // NOLINT(misc-include-cleaner)

  if (res != CURLE_OK)
    return std::nullopt;

  long status = 0;
  curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &status);
  result.status_code = static_cast<int>(status);

  return result;
}

bool HttpClient::post_streaming(
    const std::string &url, const std::string &body,
    std::function<void(const std::string &line)> on_line,
    const std::map<std::string, std::string> &extra_headers,
    const std::optional<std::string> &api_key,
    std::optional<std::uint32_t> timeout_ms, std::stop_token stop_tok,
    std::shared_ptr<StreamDiagnostics> diagnostics) {

  struct WriteState {
    std::string buffer;
    std::string raw_body;
    std::function<void(const std::string &)> callback;
    std::optional<std::string> callback_error;
    std::shared_ptr<StreamDiagnostics> diagnostics;
  };

  auto write_callback =
      +[](char *ptr, size_t size, size_t nmemb, void *data) -> size_t {
    auto *state = static_cast<WriteState *>(data);
    try {
      if (state->diagnostics)
        state->diagnostics->record_transport_chunk(size * nmemb);
      if (state->raw_body.size() < kMaxStreamingErrorBody) {
        const auto remaining = kMaxStreamingErrorBody - state->raw_body.size();
        state->raw_body.append(ptr, std::min(size * nmemb, remaining));
      }
      state->buffer.append(ptr, size * nmemb);
      size_t pos = 0;
      while ((pos = state->buffer.find('\n')) != std::string::npos) {
        auto line = state->buffer.substr(0, pos);
        state->buffer.erase(0, pos + 1);
        if (!line.empty()) {
          if (state->diagnostics)
            state->diagnostics->record_transport_line(line);
          state->callback(line);
        }
      }
    } catch (const std::exception &e) {
      state->callback_error = e.what();
      return static_cast<size_t>(0);
    } catch (...) {
      state->callback_error = "stream write callback failed";
      return static_cast<size_t>(0);
    }

    return size * nmemb;
  };

  CurlHandle curl;
  if (curl.handle == nullptr)
    return false;

  CurlHeaders headers;

  if (api_key) {
    std::string auth = "Authorization: Bearer ";
    auth += *api_key;
    headers.append(auth);
  }

  for (const auto &[key, value] : extra_headers) {
    std::string header;
    header.reserve(key.size() + value.size() + 2);
    header += key;
    header += ": ";
    header += value;
    headers.append(header);
  }

  // Defaults for any headers the caller didn't set.
  if (!extra_headers.contains("Content-Type"))
    headers.append("Content-Type: application/json");
  if (!extra_headers.contains("Accept"))
    headers.append("Accept: text/event-stream");

  curl_easy_setopt(curl.handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_POST, 1L);
  curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers.list);
  curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT_MS,
                   (long)timeout_ms.value_or(600000));

  curl_easy_setopt(curl.handle, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.handle, CURLOPT_XFERINFOFUNCTION, stop_token_progress);
  curl_easy_setopt(curl.handle, CURLOPT_XFERINFODATA,
                   const_cast<std::stop_token *>(&stop_tok));

  curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, write_callback);

  WriteState state{.buffer = std::string{},
                   .callback = std::move(on_line),
                   .diagnostics = std::move(diagnostics)};
  curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &state);

  CURLcode res = curl_easy_perform(curl.handle);

  if (!state.buffer.empty()) {
    try {
      if (state.diagnostics)
        state.diagnostics->record_transport_line(state.buffer);
      state.callback(state.buffer);
    } catch (const std::exception &e) {
      state.callback_error = e.what();
    } catch (...) {
      state.callback_error = "stream callback failed";
    }
  }

  long status = 0;
  curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &status);

  if (state.callback_error) {
    auto line = error_line(*state.callback_error);
    if (state.diagnostics)
      state.diagnostics->record_transport_line(line);
    state.callback(line);
    return false;
  }
  if (res != CURLE_OK) {
    auto line = error_line(curl_easy_strerror(res));
    if (state.diagnostics)
      state.diagnostics->record_transport_line(line);
    state.callback(line);
    return false;
  }
  if (status < 200 || status >= 300) {
    std::string msg = "HTTP status " + std::to_string(status);
    if (!state.raw_body.empty())
      msg += ": " + state.raw_body;
    auto line = error_line(msg);
    if (state.diagnostics)
      state.diagnostics->record_transport_line(line);
    state.callback(line);
    return false;
  }

  return true;
}

std::optional<HttpClient::Response> HttpClient::post_authenticated(
    const std::string &url, const std::string &body,
    const std::map<std::string, std::string> &extra_headers,
    const std::optional<RequestAuth> &auth,
    std::optional<std::uint32_t> timeout_ms, std::stop_token stop_tok) {
  CurlHandle curl;
  if (curl.handle == nullptr)
    return std::nullopt;

  Response result;
  CurlHeaders headers;
  append_authenticated_headers(headers, extra_headers, auth,
                               "application/json");

  curl_easy_setopt(curl.handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_POST, 1L);
  curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers.list);
  curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT_MS,
                   static_cast<long>(timeout_ms.value_or(600000)));
  curl_easy_setopt(curl.handle, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.handle, CURLOPT_XFERINFOFUNCTION, stop_token_progress);
  curl_easy_setopt(curl.handle, CURLOPT_XFERINFODATA,
                   const_cast<std::stop_token *>(&stop_tok));
  curl_easy_setopt(
      curl.handle, CURLOPT_WRITEFUNCTION,
      +[](char *ptr, size_t size, size_t nmemb, void *data) -> size_t {
        auto *resp = static_cast<HttpClient::Response *>(data);
        resp->body.append(ptr, size * nmemb);
        return size * nmemb;
      });
  curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &result);

  if (curl_easy_perform(curl.handle) != CURLE_OK)
    return std::nullopt;

  long status = 0;
  curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &status);
  result.status_code = static_cast<int>(status);
  return result;
}

std::optional<HttpClient::Response> HttpClient::get_authenticated(
    const std::string &url,
    const std::map<std::string, std::string> &extra_headers,
    const std::optional<RequestAuth> &auth,
    std::optional<std::uint32_t> timeout_ms, std::stop_token stop_tok) {
  CurlHandle curl;
  if (curl.handle == nullptr)
    return std::nullopt;

  Response result;
  CurlHeaders headers;
  append_authenticated_headers(headers, extra_headers, auth,
                               "application/json");

  curl_easy_setopt(curl.handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers.list);
  curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT_MS,
                   static_cast<long>(timeout_ms.value_or(600000)));
  curl_easy_setopt(curl.handle, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.handle, CURLOPT_XFERINFOFUNCTION, stop_token_progress);
  curl_easy_setopt(curl.handle, CURLOPT_XFERINFODATA,
                   const_cast<std::stop_token *>(&stop_tok));
  curl_easy_setopt(
      curl.handle, CURLOPT_WRITEFUNCTION,
      +[](char *ptr, size_t size, size_t nmemb, void *data) -> size_t {
        auto *resp = static_cast<HttpClient::Response *>(data);
        resp->body.append(ptr, size * nmemb);
        return size * nmemb;
      });
  curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &result);

  if (curl_easy_perform(curl.handle) != CURLE_OK)
    return std::nullopt;

  long status = 0;
  curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &status);
  result.status_code = static_cast<int>(status);
  return result;
}

bool HttpClient::post_streaming_authenticated(
    const std::string &url, const std::string &body,
    std::function<void(const std::string &line)> on_line,
    const std::map<std::string, std::string> &extra_headers,
    const std::optional<RequestAuth> &auth,
    std::optional<std::uint32_t> timeout_ms, std::stop_token stop_tok,
    std::shared_ptr<StreamDiagnostics> diagnostics,
    const ResponseCallback &on_response) {
  struct WriteState {
    std::string buffer;
    std::string raw_body;
    std::function<void(const std::string &)> callback;
    std::optional<std::string> callback_error;
    std::shared_ptr<StreamDiagnostics> diagnostics;
    std::map<std::string, std::string> headers;
  };

  auto write_callback =
      +[](char *ptr, size_t size, size_t nmemb, void *data) -> size_t {
    auto *state = static_cast<WriteState *>(data);
    try {
      if (state->diagnostics)
        state->diagnostics->record_transport_chunk(size * nmemb);
      if (state->raw_body.size() < kMaxStreamingErrorBody) {
        const auto remaining = kMaxStreamingErrorBody - state->raw_body.size();
        state->raw_body.append(ptr, std::min(size * nmemb, remaining));
      }
      state->buffer.append(ptr, size * nmemb);
      size_t pos = 0;
      while ((pos = state->buffer.find('\n')) != std::string::npos) {
        auto line = state->buffer.substr(0, pos);
        state->buffer.erase(0, pos + 1);
        if (!line.empty()) {
          if (state->diagnostics)
            state->diagnostics->record_transport_line(line);
          state->callback(line);
        }
      }
    } catch (const std::exception &e) {
      state->callback_error = e.what();
      return static_cast<size_t>(0);
    } catch (...) {
      state->callback_error = "stream write callback failed";
      return static_cast<size_t>(0);
    }
    return size * nmemb;
  };

  auto header_callback =
      +[](char *ptr, size_t size, size_t nmemb, void *data) -> size_t {
    auto *state = static_cast<WriteState *>(data);
    const std::string_view line(ptr, size * nmemb);
    const auto colon = line.find(':');
    if (colon == std::string_view::npos)
      return size * nmemb;
    auto key = std::string(line.substr(0, colon));
    auto value = std::string(line.substr(colon + 1));
    while (!value.empty() &&
           (std::isspace(static_cast<unsigned char>(value.front())) != 0))
      value.erase(value.begin());
    while (!value.empty() &&
           (std::isspace(static_cast<unsigned char>(value.back())) != 0))
      value.pop_back();
    state->headers[std::move(key)] = std::move(value);
    return size * nmemb;
  };

  CurlHandle curl;
  if (curl.handle == nullptr)
    return false;

  CurlHeaders headers;
  append_authenticated_headers(headers, extra_headers, auth,
                               "text/event-stream");

  curl_easy_setopt(curl.handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_POST, 1L);
  curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDS, body.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers.list);
  curl_easy_setopt(curl.handle, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT_MS,
                   static_cast<long>(timeout_ms.value_or(600000)));
  curl_easy_setopt(curl.handle, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl.handle, CURLOPT_XFERINFOFUNCTION, stop_token_progress);
  curl_easy_setopt(curl.handle, CURLOPT_XFERINFODATA,
                   const_cast<std::stop_token *>(&stop_tok));
  curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, write_callback);
  curl_easy_setopt(curl.handle, CURLOPT_HEADERFUNCTION, header_callback);

  WriteState state{.callback = std::move(on_line),
                   .diagnostics = std::move(diagnostics)};
  curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &state);
  curl_easy_setopt(curl.handle, CURLOPT_HEADERDATA, &state);

  CURLcode res = curl_easy_perform(curl.handle);
  if (!state.buffer.empty()) {
    try {
      if (state.diagnostics)
        state.diagnostics->record_transport_line(state.buffer);
      state.callback(state.buffer);
    } catch (const std::exception &e) {
      state.callback_error = e.what();
    } catch (...) {
      state.callback_error = "stream callback failed";
    }
  }

  long status = 0;
  curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &status);
  if (on_response)
    on_response(static_cast<int>(status), state.headers);
  if (state.callback_error) {
    state.callback(error_line(*state.callback_error));
    return false;
  }
  if (res != CURLE_OK) {
    state.callback(error_line(curl_easy_strerror(res)));
    return false;
  }
  if (status < 200 || status >= 300) {
    std::string msg = "HTTP status " + std::to_string(status);
    if (!state.raw_body.empty())
      msg += ": " + state.raw_body;
    state.callback(error_line(msg));
    return false;
  }
  return true;
}

} // namespace pi::core
