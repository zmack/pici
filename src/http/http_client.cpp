#ifdef PI_CPP_HAVE_HTTP

#include "http/http_client.h"

#include <mutex>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace pi::core {

namespace json = nlohmann;

namespace {

struct CurlHandle {
    CURL* handle{curl_easy_init()};
    ~CurlHandle() {
        if (handle) {
            curl_easy_cleanup(handle);
        }
    }

    CurlHandle(const CurlHandle&) = delete;
    CurlHandle& operator=(const CurlHandle&) = delete;
};

struct CurlHeaders {
    curl_slist* list{nullptr};
    ~CurlHeaders() { curl_slist_free_all(list); }

    void append(const std::string& header) {
        list = curl_slist_append(list, header.c_str());
    }

    CurlHeaders(const CurlHeaders&) = delete;
    CurlHeaders& operator=(const CurlHeaders&) = delete;
};

} // namespace

// ─── OpenAICompatibleClient ───────────────────────────────────────────────

OpenAICompatibleClient::OpenAICompatibleClient(
    std::string base_url, std::string model_id, bool streaming)
    : base_url_(std::move(base_url)), model_id_(std::move(model_id)),
      streaming_(streaming) {}

std::shared_ptr<AssistantMessage> OpenAICompatibleClient::stream(
    const Model& model,
    const AgentContext& context,
    ThinkingLevel thinking_level,
    StreamCallback emit,
    const std::optional<std::string>& api_key,
    std::stop_token stop_tok) {
    (void)thinking_level; // Would need proper mapping

    auto request = build_request_json(context, thinking_level, model);

    auto result = std::make_shared<AssistantMessage>();
    result->api = model.api;
    result->provider = model.provider;
    result->model = model.id;
    result->stop_reason = StopReason::stop;
    result->timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();

    auto emit_partial = [result, &emit](const std::string& content_type,
                                        const std::string& content) {
        (void)content_type;
        TextContent text;
        text.text = content;
        result->content.push_back(std::move(text));
        emit(MessageUpdateEvent(*result, content));
    };

    // Use streaming HTTP
    HttpClient::post_streaming(
        base_url_ + "/v1/chat/completions",
        request,
        [result, &emit, &emit_partial](const std::string& line) {
            if (line.empty() || line.find("data: [DONE]") != std::string::npos) {
                return;
            }
            if (line.find("data: ") != 0) return;

            std::string data = line.substr(6);
            try {
                auto j = json::parse(data);
                auto delta = j.value("choices", json::array()).empty()
                                 ? json::object()
                                 : j["choices"][0].value("delta", json::object());
                auto content = delta.value("content", std::string{});
                if (!content.empty()) {
                    emit_partial("text", content);
                }
            } catch (...) {
                // Ignore parse errors
            }
        },
        {}, api_key, stop_tok);

    result->stop_reason = StopReason::stop;
    return result;
}

std::string OpenAICompatibleClient::build_request_json(
    const AgentContext& context,
    ThinkingLevel thinking_level,
    const Model& model) const {
    (void)model;
    (void)thinking_level;

    json::array_t messages;
    if (!context.system_prompt.empty()) {
        messages.push_back(
            json{{"role", "system"}, {"content", context.system_prompt}});
    }

    for (const auto& msg : context.messages) {
        std::string role;
        std::string content;
        std::visit(
            [&](const auto& value) {
                using T = std::remove_cvref_t<decltype(value)>;
                if constexpr (std::same_as<T, UserMessage>) {
                    role = "user";
                } else if constexpr (std::same_as<T, AssistantMessage>) {
                    role = "assistant";
                } else if constexpr (std::same_as<T, ToolResultMessage>) {
                    role = "tool";
                }
                for (const auto& block : value.content) {
                    if (const auto* text = std::get_if<TextContent>(&block)) {
                        content += text->text;
                    }
                }
            },
            msg);
        if (!role.empty()) {
            messages.push_back(json{{"role", role}, {"content", content}});
        }
    }

    json::array_t tools_arr;
    if (!context.tools.empty()) {
        for (const auto& t : context.tools) {
            tools_arr.push_back(t->schema().serialize());
        }
    }

    return json{{"model", model_id_},
                {"messages", messages},
                {"stream", streaming_},
                {"tools", tools_arr}}
        .dump();
}

void OpenAICompatibleClient::process_sse_chunk(
    const std::string& line, AssistantMessage& partial, StreamCallback emit) {
    // SSE processing for OpenAI-compatible APIs
    // In a full impl, parse deltas and emit proper events
}

// ─── HttpClient ───────────────────────────────────────────────────────────

HttpClient::Response HttpClient::post(
    const std::string& url,
    const std::string& body,
    const std::map<std::string, std::string>& extra_headers,
    const std::optional<std::string>& api_key,
    std::stop_token stop_tok) {
    (void)stop_tok;

    CurlHandle curl;
    if (!curl.handle) {
        return std::nullopt;
    }
    Response result;

    CurlHeaders headers;
    headers.append("Content-Type: application/json");
    headers.append("Accept: application/json");

    if (api_key) {
        headers.append("Authorization: Bearer " + *api_key);
    }

    for (const auto& [key, value] : extra_headers) {
        headers.append(key + ": " + value);
    }

    curl_easy_setopt(curl.handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.handle, CURLOPT_POST, 1L);
    curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION,
                     [](char* ptr, size_t size, size_t nmemb, void* data) {
                         auto* resp = static_cast<HttpClient::Response*>(data);
                         resp->body.append(ptr, size * nmemb);
                         return size * nmemb;
                     });
    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &result);

    CURLcode res = curl_easy_perform(curl.handle);

    if (res != CURLE_OK) {
        return std::nullopt;
    }

    long status = 0;
    curl_easy_getinfo(curl.handle, CURLINFO_RESPONSE_CODE, &status);

    result.status_code = static_cast<int>(status);
    return result;
}

std::optional<std::string> HttpClient::post_streaming(
    const std::string& url,
    const std::string& body,
    std::function<void(const std::string& line)> on_line,
    const std::map<std::string, std::string>& extra_headers,
    const std::optional<std::string>& api_key,
    std::stop_token stop_tok) {
    (void)stop_tok;

    auto write_callback = [](char* ptr, size_t size, size_t nmemb,
                              void* data) -> size_t {
        auto* state = static_cast<std::pair<std::string*, std::function<void(const std::string&)>>*>(data);
        auto* buf = state->first;
        auto* callback = &state->second;

        buf->append(ptr, size * nmemb);

        // Process complete lines
        size_t pos;
        while ((pos = buf->find('\n')) != std::string::npos) {
            auto line = buf->substr(0, pos);
            buf->erase(0, pos + 1);
            if (!line.empty()) {
                (*callback)(line);
            }
        }

        return size * nmemb;
    };

    CurlHandle curl;
    if (!curl.handle) return std::nullopt;

    CurlHeaders headers;
    headers.append("Content-Type: application/json");
    headers.append("Accept: text/event-stream");

    if (api_key) {
        headers.append("Authorization: Bearer " + *api_key);
    }

    for (const auto& [key, value] : extra_headers) {
        headers.append(key + ": " + value);
    }

    curl_easy_setopt(curl.handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.handle, CURLOPT_POST, 1L);
    curl_easy_setopt(curl.handle, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl.handle, CURLOPT_HTTPHEADER, headers.list);
    curl_easy_setopt(curl.handle, CURLOPT_WRITEFUNCTION, write_callback);

    std::string buffer;
    std::pair<std::string, std::function<void(const std::string&)>> state{&buffer, on_line};
    curl_easy_setopt(curl.handle, CURLOPT_WRITEDATA, &state);

    CURLcode res = curl_easy_perform(curl.handle);

    // Process remaining buffer
    if (!buffer.empty()) {
        on_line(buffer);
    }

    return res == CURLE_OK ? std::string{} : std::string{"HTTP request failed"};
}

} // namespace pi::core

#endif // PI_CPP_HAVE_HTTP
