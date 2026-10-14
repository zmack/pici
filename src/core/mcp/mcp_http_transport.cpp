#include "core/mcp/mcp_http_transport.h"

#include <algorithm>
#include <cctype>
#include <curl/curl.h>
#include <string_view>

namespace pi::core::mcp {

namespace {

constexpr auto kConnectTimeout = std::chrono::milliseconds(10000);
constexpr auto kAuxTimeout = std::chrono::milliseconds(3000);

std::string lower(std::string_view text) {
  std::string out(text);
  std::ranges::transform(out, out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

std::string_view trim(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())))
    text.remove_prefix(1);
  while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
    text.remove_suffix(1);
  return text;
}

bool has_line_break(std::string_view text) {
  return text.find_first_of("\r\n") != std::string_view::npos;
}

// scheme://host[:port]/path with userinfo, query and fragment stripped.
// Throws for anything that is not http(s) or that embeds credentials.
std::string redact_url(const std::string &url) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string::npos)
    throw std::runtime_error("mcp http: url must be http:// or https://");
  const auto scheme = lower(url.substr(0, scheme_end));
  if (scheme != "http" && scheme != "https")
    throw std::runtime_error("mcp http: url must be http:// or https://");
  const auto authority_start = scheme_end + 3;
  const auto authority_end = url.find_first_of("/?#", authority_start);
  const auto authority =
      url.substr(authority_start, authority_end == std::string::npos
                                      ? std::string::npos
                                      : authority_end - authority_start);
  if (authority.empty())
    throw std::runtime_error("mcp http: url has no host");
  if (authority.find('@') != std::string::npos)
    throw std::runtime_error(
        "mcp http: credentials in the url are not allowed; use "
        "bearer_token_env or headers");
  std::string path;
  if (authority_end != std::string::npos && url[authority_end] == '/') {
    const auto path_end = url.find_first_of("?#", authority_end);
    path = url.substr(authority_end, path_end == std::string::npos
                                         ? std::string::npos
                                         : path_end - authority_end);
  }
  return scheme + "://" + authority + path;
}

// Incremental text/event-stream decoder. Yields the concatenated `data`
// field of every dispatched `message` event.
class SseParser {
public:
  template <typename Sink> void feed(std::string_view chunk, Sink &&sink) {
    buffer_.append(chunk);
    std::size_t start = 0;
    for (;;) {
      const auto nl = buffer_.find('\n', start);
      if (nl == std::string::npos)
        break;
      auto line = std::string_view(buffer_).substr(start, nl - start);
      if (!line.empty() && line.back() == '\r')
        line.remove_suffix(1);
      handle_line(line, sink);
      start = nl + 1;
    }
    buffer_.erase(0, start);
  }

private:
  template <typename Sink> void handle_line(std::string_view line, Sink &sink) {
    if (line.empty()) {
      if (has_data_ && (event_.empty() || event_ == "message"))
        sink(data_);
      data_.clear();
      event_.clear();
      has_data_ = false;
      return;
    }
    if (line.front() == ':')
      return;
    const auto colon = line.find(':');
    const auto field = line.substr(0, colon);
    auto value = colon == std::string_view::npos ? std::string_view{}
                                                 : line.substr(colon + 1);
    if (!value.empty() && value.front() == ' ')
      value.remove_prefix(1);
    if (field == "data") {
      if (has_data_)
        data_ += '\n';
      data_.append(value);
      has_data_ = true;
    } else if (field == "event") {
      event_ = std::string(value);
    }
  }

  std::string buffer_;
  std::string data_;
  std::string event_;
  bool has_data_{false};
};

struct CurlHeaders {
  curl_slist *list{nullptr};
  CurlHeaders() = default;
  CurlHeaders(const CurlHeaders &) = delete;
  CurlHeaders &operator=(const CurlHeaders &) = delete;
  ~CurlHeaders() { curl_slist_free_all(list); }
  void add(const std::string &line) {
    auto *next = curl_slist_append(list, line.c_str());
    if (next == nullptr)
      throw TransportError("mcp http: out of memory building headers");
    list = next;
  }
};

struct CurlHandle {
  CURL *handle{curl_easy_init()};
  CurlHandle() = default;
  CurlHandle(const CurlHandle &) = delete;
  CurlHandle &operator=(const CurlHandle &) = delete;
  ~CurlHandle() {
    if (handle != nullptr)
      curl_easy_cleanup(handle);
  }
};

} // namespace

struct McpHttpTransport::Exchange {
  long status{0};
  std::string session_id;
  std::optional<nlohmann::json> response;
  std::vector<nlohmann::json> server_request_ids;
  std::vector<std::pair<std::string, nlohmann::json>> notifications;
  std::string content_type;
  std::string body; // non-SSE bodies only
  std::string protocol_error;
  std::size_t received{0};
  bool oversized{false};
  bool cancelled{false};
  bool done{false};
  bool sse{false};
  SseParser sse_parser;
  std::optional<RequestId> want;
  std::size_t max_bytes{0};
  const std::stop_token *stop{nullptr};

  void on_message(const nlohmann::json &msg) {
    if (msg.is_array()) {
      for (const auto &item : msg)
        on_message(item);
      return;
    }
    const auto parsed = parse_inbound_message(msg);
    if (!parsed)
      return;
    if (const auto *response_msg = std::get_if<JsonRpcResponse>(&*parsed)) {
      if (want && response_msg->id == *want) {
        response = msg;
        done = true;
      }
    } else if (const auto *note = std::get_if<JsonRpcNotification>(&*parsed)) {
      notifications.emplace_back(note->method, note->params);
    } else if (const auto *req = std::get_if<InboundServerRequest>(&*parsed)) {
      server_request_ids.push_back(req->id);
    }
  }

  void on_sse_data(const std::string &data) {
    auto parsed = nlohmann::json::parse(data, nullptr, false);
    if (parsed.is_discarded()) {
      protocol_error = "malformed JSON in event stream";
      done = true;
      return;
    }
    on_message(parsed);
  }
};

McpHttpTransport::McpHttpTransport(McpHttpConfig config)
    : config_(std::move(config)) {
  display_url_ = redact_url(config_.url);
  if (has_line_break(config_.url))
    throw std::runtime_error("mcp http: url contains a line break");
  for (const auto &[name, value] : config_.headers) {
    if (name.empty() || has_line_break(name) || has_line_break(value) ||
        name.find(':') != std::string::npos)
      throw std::runtime_error("mcp http: invalid header \"" + name + "\"");
  }
  if (config_.bearer_token && has_line_break(*config_.bearer_token))
    throw std::runtime_error("mcp http: invalid bearer token");
  curl_global_init(CURL_GLOBAL_DEFAULT);
}

McpHttpTransport::~McpHttpTransport() {
  std::string session;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session = session_id_;
  }
  if (session.empty() || !alive_.load())
    return;
  CurlHandle curl;
  if (curl.handle == nullptr)
    return;
  CurlHeaders headers;
  try {
    headers.add("Mcp-Session-Id: " + session);
    if (config_.bearer_token)
      headers.add("Authorization: Bearer " + *config_.bearer_token);
    for (const auto &[name, value] : config_.headers)
      headers.add(name + ": " + value);
  } catch (const std::exception &) {
    return;
  }
  curl_easy_setopt(curl.handle, CURLOPT_URL, config_.url.c_str());
  curl_easy_setopt(curl.handle, CURLOPT_CUSTOMREQUEST, "DELETE");
  curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers.list);
  curl_easy_setopt(curl.handle, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(curl.handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl.handle, CURLOPT_TIMEOUT_MS,
                   static_cast<long>(kAuxTimeout.count()));
  curl_easy_setopt(
      curl.handle, CURLOPT_WRITEFUNCTION,
      +[](char *, std::size_t size, std::size_t n, void *) {
        return size * n;
      });
  curl_easy_perform(curl.handle);
}

McpHttpTransport::Exchange McpHttpTransport::post(
    const nlohmann::json &message, std::chrono::milliseconds timeout,
    const std::stop_token &stop_tok, std::optional<RequestId> want,
    const std::string &method) {
  CurlHandle curl;
  if (curl.handle == nullptr)
    throw TransportError("mcp http: failed to create request handle");

  std::string session;
  std::string protocol;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    session = session_id_;
    protocol = protocol_version_;
  }

  CurlHeaders headers;
  headers.add("Content-Type: application/json");
  headers.add("Accept: application/json, text/event-stream");
  for (const auto &[name, value] : config_.headers)
    headers.add(name + ": " + value);
  if (config_.bearer_token)
    headers.add("Authorization: Bearer " + *config_.bearer_token);
  if (!session.empty())
    headers.add("Mcp-Session-Id: " + session);
  if (!protocol.empty())
    headers.add("MCP-Protocol-Version: " + protocol);

  Exchange ex;
  ex.want = want;
  ex.max_bytes = config_.max_response_bytes;
  ex.stop = &stop_tok;

  const std::string payload = message.dump();
  auto *h = curl.handle;
  curl_easy_setopt(h, CURLOPT_URL, config_.url.c_str());
  curl_easy_setopt(h, CURLOPT_POST, 1L);
  curl_easy_setopt(h, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(h, CURLOPT_POSTFIELDSIZE_LARGE,
                   static_cast<curl_off_t>(payload.size()));
  curl_easy_setopt(h, CURLOPT_HTTPHEADER, headers.list);
  curl_easy_setopt(h, CURLOPT_PROTOCOLS_STR, "http,https");
  curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 0L);
  curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(h, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout.count()));
  curl_easy_setopt(
      h, CURLOPT_CONNECTTIMEOUT_MS,
      static_cast<long>(std::min(timeout, kConnectTimeout).count()));

  curl_easy_setopt(
      h, CURLOPT_HEADERFUNCTION,
      +[](char *buffer, std::size_t size, std::size_t n, void *user) {
        auto &exchange = *static_cast<Exchange *>(user);
        const std::string_view line(buffer, size * n);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos)
          return size * n;
        const auto name = lower(trim(line.substr(0, colon)));
        const auto value = std::string(trim(line.substr(colon + 1)));
        if (name == "content-type") {
          exchange.content_type = lower(value);
          exchange.sse = exchange.content_type.starts_with("text/event-stream");
        } else if (name == "mcp-session-id") {
          exchange.session_id = value;
        }
        return size * n;
      });
  curl_easy_setopt(h, CURLOPT_HEADERDATA, &ex);

  curl_easy_setopt(
      h, CURLOPT_WRITEFUNCTION,
      +[](char *data, std::size_t size, std::size_t n, void *user) {
        auto &exchange = *static_cast<Exchange *>(user);
        const std::size_t len = size * n;
        exchange.received += len;
        if (exchange.received > exchange.max_bytes) {
          exchange.oversized = true;
          return std::size_t{0};
        }
        if (exchange.sse) {
          exchange.sse_parser.feed(std::string_view(data, len),
                                   [&](const std::string &event_data) {
                                     if (!exchange.done)
                                       exchange.on_sse_data(event_data);
                                   });
          // Stop reading once the awaited response arrived: a server that
          // keeps the stream open must not hold the caller hostage.
          if (exchange.done)
            return std::size_t{0};
        } else {
          exchange.body.append(data, len);
        }
        return len;
      });
  curl_easy_setopt(h, CURLOPT_WRITEDATA, &ex);

  curl_easy_setopt(
      h, CURLOPT_XFERINFOFUNCTION,
      +[](void *user, curl_off_t, curl_off_t, curl_off_t, curl_off_t) -> int {
        auto &exchange = *static_cast<Exchange *>(user);
        if (exchange.stop != nullptr && exchange.stop->stop_requested()) {
          exchange.cancelled = true;
          return 1;
        }
        return 0;
      });
  curl_easy_setopt(h, CURLOPT_XFERINFODATA, &ex);
  curl_easy_setopt(h, CURLOPT_NOPROGRESS, 0L);

  const CURLcode code = curl_easy_perform(h);
  curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &ex.status);

  if (ex.oversized)
    throw TransportError("mcp http response from " + display_url_ +
                         " exceeds " + std::to_string(ex.max_bytes) + " bytes");
  if (ex.cancelled) {
    if (want) {
      try {
        notify(std::string(methods::kCancelled),
               nlohmann::json{{"requestId", *want},
                              {"reason", "client cancelled the tool call"}});
      } catch (...) { // NOLINT(bugprone-empty-catch)
      }
    }
    throw TransportError("mcp call cancelled: " + method);
  }
  // CURLE_WRITE_ERROR with `done` is our own early stop, not a failure.
  const bool early_stop = code == CURLE_WRITE_ERROR && ex.done;
  if (code == CURLE_OPERATION_TIMEDOUT)
    throw TransportError("mcp call timed out: " + method);
  if (code != CURLE_OK && !early_stop)
    throw TransportError("mcp http request to " + display_url_ +
                         " failed: " + curl_easy_strerror(code));
  return ex;
}

nlohmann::json McpHttpTransport::call(const std::string &method,
                                      nlohmann::json params,
                                      std::chrono::milliseconds timeout,
                                      std::stop_token stop_tok) {
  if (!alive_.load())
    throw TransportError("mcp http session is closed");

  const auto id = next_id_.fetch_add(1);
  const nlohmann::json request{
      {"jsonrpc", "2.0"},
      {"id", id},
      {"method", method},
      {"params", std::move(params)},
  };

  auto ex = post(request, timeout, stop_tok, id, method);

  if (ex.status == 404) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!session_id_.empty()) {
      alive_.store(false);
      throw TransportError("mcp http session expired (server " + display_url_ +
                           " no longer recognizes it)");
    }
  }
  if (ex.status == 401 || ex.status == 403)
    throw TransportError("mcp http server " + display_url_ +
                         " rejected the request as unauthorized (HTTP " +
                         std::to_string(ex.status) + ")");
  if (ex.status < 200 || ex.status >= 300)
    throw TransportError("mcp http server " + display_url_ + " returned HTTP " +
                         std::to_string(ex.status) + " for " + method);

  if (!ex.sse && !ex.body.empty()) {
    auto parsed = nlohmann::json::parse(ex.body, nullptr, false);
    if (parsed.is_discarded())
      throw TransportError("mcp http response to " + method +
                           " is not valid JSON");
    ex.on_message(parsed);
  }
  if (!ex.protocol_error.empty())
    throw TransportError("mcp http response to " + method + ": " +
                         ex.protocol_error);

  NotificationCallback cb;
  {
    std::lock_guard<std::mutex> lock(state_mutex_);
    cb = notification_callback_;
    if (method == methods::kInitialize && !ex.session_id.empty())
      session_id_ = ex.session_id;
  }
  if (cb) {
    for (const auto &[note_method, note_params] : ex.notifications)
      cb(note_method, note_params);
  }
  send_server_request_errors(ex.server_request_ids);

  if (!ex.response)
    throw TransportError("mcp http response to " + method +
                         " did not contain a matching JSON-RPC response");

  const auto &response = *ex.response;
  if (response.contains("error")) {
    const auto &error = response.at("error");
    throw TransportError(
        "mcp error calling " + method + ": " +
        (error.is_object()
             ? error.value("message", std::string{"unknown error"})
             : std::string{"unknown error"}));
  }
  nlohmann::json result = response.contains("result")
                              ? response.at("result")
                              : nlohmann::json::object();
  if (method == methods::kInitialize && result.is_object() &&
      result.contains("protocolVersion") &&
      result.at("protocolVersion").is_string()) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    protocol_version_ = result.at("protocolVersion").get<std::string>();
  }
  return result;
}

void McpHttpTransport::notify(const std::string &method,
                              nlohmann::json params) {
  if (!alive_.load())
    throw TransportError("mcp http session is closed");
  const nlohmann::json message{
      {"jsonrpc", "2.0"},
      {"method", method},
      {"params", std::move(params)},
  };
  const auto ex =
      post(message, kAuxTimeout * 2, std::stop_token{}, std::nullopt, method);
  if (ex.status == 404) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!session_id_.empty())
      alive_.store(false);
  }
  if (ex.status < 200 || ex.status >= 300)
    throw TransportError("mcp http server " + display_url_ + " returned HTTP " +
                         std::to_string(ex.status) + " for notification " +
                         method);
}

void McpHttpTransport::send_server_request_errors(
    const std::vector<nlohmann::json> &ids) {
  for (const auto &id : ids) {
    const nlohmann::json reply{
        {"jsonrpc", "2.0"},
        {"id", id},
        {"error", {{"code", -32601}, {"message", "method not found"}}},
    };
    try {
      post(reply, kAuxTimeout, std::stop_token{}, std::nullopt, "reply");
    } catch (const std::exception &) { // NOLINT(bugprone-empty-catch)
    }
  }
}

void McpHttpTransport::set_notification_callback(NotificationCallback cb) {
  std::lock_guard<std::mutex> lock(state_mutex_);
  notification_callback_ = std::move(cb);
}

} // namespace pi::core::mcp
