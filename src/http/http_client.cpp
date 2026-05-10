#include "http/http_client.h"
#include "curl/easy.h"
#include "curl/system.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <optional>
#include <string>

#include <curl/curl.h>
#include <utility>

namespace pi::core {

namespace {

struct CurlHandle {
  CURL *handle{nullptr};
  CurlHandle() : handle(curl_easy_init()) {}
  ~CurlHandle() {
    if (handle != nullptr) {
      curl_easy_cleanup(handle);
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

int stop_token_progress(void *p, curl_off_t, curl_off_t, curl_off_t,
                        curl_off_t) {
  return static_cast<const std::stop_token *>(p)->stop_requested() ? 1 : 0;
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
  headers.append("Content-Type: application/json");
  headers.append("Accept: application/json");

  if (api_key) {
    headers.append("Authorization: Bearer " + *api_key);
  }

  for (const auto &[key, value] : extra_headers) {
    headers.append(key + ": " + value);
  }

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

  CURLcode res = curl_easy_perform(curl.handle);

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
    std::optional<std::uint32_t> timeout_ms, std::stop_token stop_tok) {

  struct WriteState {
    std::string buffer;
    std::function<void(const std::string &)> callback;
    std::optional<std::string> callback_error;
  };

  auto write_callback =
      +[](char *ptr, size_t size, size_t nmemb, void *data) -> size_t {
    auto *state = static_cast<WriteState *>(data);
    try {
      state->buffer.append(ptr, size * nmemb);
      size_t pos = 0;
      while ((pos = state->buffer.find('\n')) != std::string::npos) {
        auto line = state->buffer.substr(0, pos);
        state->buffer.erase(0, pos + 1);
        if (!line.empty()) {
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
  headers.append("Content-Type: application/json");
  headers.append("Accept: text/event-stream");

  if (api_key) {
    headers.append("Authorization: Bearer " + *api_key);
  }

  for (const auto &[key, value] : extra_headers) {
    headers.append(key + ": " + value);
  }

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

  WriteState state{.buffer = std::string{}, .callback = std::move(on_line)};
  curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &state);

  CURLcode res = curl_easy_perform(curl.handle);

  if (!state.buffer.empty()) {
    try {
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
    state.callback(std::string(R"({"error":{"message":")") +
                   *state.callback_error + "\"}}");
    return false;
  }
  if (res != CURLE_OK) {
    state.callback(std::string(R"({"error":{"message":")") +
                   curl_easy_strerror(res) + "\"}}");
    return false;
  }
  if (status < 200 || status >= 300) {
    state.callback(std::string(R"({"error":{"message":"HTTP status )") +
                   std::to_string(status) + "\"}}");
    return false;
  }

  return true;
}

} // namespace pi::core
