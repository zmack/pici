#include "core/message_types.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <numeric>
#include <source_location>
#include <stdexcept>
#include <string_view>

namespace pi::core {

// ─── StopReason ───────────────────────────────────────────────────────────

std::string_view stop_reason_to_string(StopReason reason) {
    switch (reason) {
        case StopReason::stop:
            return "stop";
        case StopReason::length:
            return "length";
        case StopReason::tool_use:
            return "toolUse";
        case StopReason::error:
            return "error";
        case StopReason::aborted:
            return "aborted";
    }
    return "unknown";
}

StopReason stop_reason_from_string(std::string_view s) {
    if (s == "stop") return StopReason::stop;
    if (s == "length") return StopReason::length;
    if (s == "toolUse") return StopReason::tool_use;
    if (s == "error") return StopReason::error;
    if (s == "aborted") return StopReason::aborted;
    return StopReason::error;
}

// ─── ThinkingLevel ─────────────────────────────────────────────────────────

std::string_view thinking_level_to_string(ThinkingLevel level) {
    switch (level) {
        case ThinkingLevel::off:
            return "off";
        case ThinkingLevel::minimal:
            return "minimal";
        case ThinkingLevel::low:
            return "low";
        case ThinkingLevel::medium:
            return "medium";
        case ThinkingLevel::high:
            return "high";
        case ThinkingLevel::xhigh:
            return "xhigh";
    }
    return "unknown";
}

ThinkingLevel thinking_level_from_string(std::string_view s) {
    if (s == "off") return ThinkingLevel::off;
    if (s == "minimal") return ThinkingLevel::minimal;
    if (s == "low") return ThinkingLevel::low;
    if (s == "medium") return ThinkingLevel::medium;
    if (s == "high") return ThinkingLevel::high;
    if (s == "xhigh") return ThinkingLevel::xhigh;
    return ThinkingLevel::off;
}

// ─── ToolExecutionMode ─────────────────────────────────────────────────────

std::ostream& operator<<(std::ostream& os, StopReason reason) {
    os << stop_reason_to_string(reason);
    return os;
}

std::ostream& operator<<(std::ostream& os, ThinkingLevel level) {
    os << thinking_level_to_string(level);
    return os;
}

std::ostream& operator<<(std::ostream& os, ToolExecutionMode mode) {
    switch (mode) {
        case ToolExecutionMode::sequential:
            os << "sequential";
            break;
        case ToolExecutionMode::parallel:
            os << "parallel";
            break;
    }
    return os;
}

// ─── Minimal JSON ──────────────────────────────────────────────────────────

namespace json {

// ── Simple JSON value type ─────────────────────────────────────────────

enum class JsonKind {
    null_t,
    bool_t,
    int64_t,
    double_t,
    string_t,
    array_t,
    object_t,
};

struct JsonValue;
using JsonObject = std::vector<std::pair<std::string, JsonValue>>;

struct JsonValue {
    JsonKind kind{JsonKind::null_t};
    bool bval{false};
    std::int64_t ival{0};
    double dval{0.0};
    std::string sval;
    std::vector<JsonValue> arr;
    JsonObject obj;

    JsonValue() = default;
    JsonValue(std::nullptr_t) : kind(JsonKind::null_t) {}
    JsonValue(bool v) : kind(JsonKind::bool_t), bval(v) {}
    JsonValue(std::int64_t v) : kind(JsonKind::int64_t), ival(v) {}
    JsonValue(std::uint64_t v) : kind(JsonKind::int64_t), ival(static_cast<std::int64_t>(v)) {}
    JsonValue(double v) : kind(JsonKind::double_t), dval(v) {}
    JsonValue(std::string_view s) : kind(JsonKind::string_t), sval(s) {}
    JsonValue(const char* s) : kind(JsonKind::string_t), sval(s) {}

    // Assignment from string
    JsonValue& operator=(std::string_view s) {
        sval = s;
        kind = JsonKind::string_t;
        return *this;
    }
    JsonValue& operator=(const std::string& s) {
        sval = s;
        kind = JsonKind::string_t;
        return *this;
    }

    // Array access
    JsonValue& operator[](std::size_t i) {
        return arr.at(i);
    }
    const JsonValue& operator[](std::size_t i) const {
        return arr.at(i);
    }

    // Object access
    JsonValue& operator[](std::string_view key) {
        for (auto& [k, v] : obj) {
            if (k == key) return v;
        }
        obj.emplace_back(key, JsonValue{});
        return obj.back().second;
    }

    const JsonValue& operator[](std::string_view key) const {
        for (const auto& [k, v] : obj) {
            if (k == key) return v;
        }
        static JsonValue null_val;
        return null_val;
    }

    template<typename T>
    JsonValue& set(std::string_view key, T val) {
        for (auto& [k, v] : obj) {
            if (k == key) { v = val; return v; }
        }
        obj.emplace_back(key, JsonValue{});
        obj.back().second = val;
        return obj.back().second;
    }

    bool has(std::string_view key) const {
        for (const auto& [k, v] : obj) {
            if (k == key) return true;
        }
        return false;
    }

    template<typename T>
    T get(std::string_view key, T default_val) const {
        for (const auto& [k, v] : obj) {
            if (k == key) {
                if constexpr (std::is_same_v<T, bool>) return v.bval;
                if constexpr (std::is_same_v<T, std::int64_t>) return v.ival;
                if constexpr (std::is_same_v<T, std::uint64_t>) return static_cast<std::uint64_t>(v.ival);
                if constexpr (std::is_same_v<T, double>) return v.dval;
                if constexpr (std::is_same_v<T, std::string>) return v.sval;
                return default_val;
            }
        }
        return default_val;
    }

    template<typename T>
    T get(std::string_view key) const {
        return get<T>(key, T{});
    }
};

// ── JSON parser ─────────────────────────────────────────────────────────

class JsonParser {
public:
    explicit JsonParser(std::string_view input) : input_(input), pos_(0) {}

    JsonValue parse() {
        skip_ws();
        auto result = parse_value();
        skip_ws();
        return result;
    }

private:
    std::string_view input_;
    std::size_t pos_;

    char peek() const { return pos_ < input_.size() ? input_[pos_] : '\0'; }
    char next() { return input_[pos_++]; }

    void skip_ws() {
        while (pos_ < input_.size() &&
               (input_[pos_] == ' ' || input_[pos_] == '\t' ||
                input_[pos_] == '\n' || input_[pos_] == '\r')) {
            pos_++;
        }
    }

    JsonValue parse_value() {
        skip_ws();
        char c = peek();
        if (c == '"') {
            std::string s = parse_string();
            return JsonValue(std::string_view(s));
        }
        if (c == '{') return parse_object();
        if (c == '[') return parse_array();
        if (match("true")) return JsonValue(true);
        if (match("false")) return JsonValue(false);
        if (match("null")) return JsonValue(nullptr);
        return parse_number();
    }

    std::string parse_string() {
        next(); // skip opening "
        std::string result;
        while (pos_ < input_.size()) {
            char c = next();
            if (c == '"') return result;
            if (c == '\\') {
                char esc = next();
                switch (esc) {
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    case '/': result += '/'; break;
                    case 'n': result += '\n'; break;
                    case 't': result += '\t'; break;
                    case 'r': result += '\r'; break;
                    case 'b': result += '\b'; break;
                    case 'f': result += '\f'; break;
                    case 'u': {
                        std::string hex;
                        for (int i = 0; i < 4 && pos_ < input_.size(); i++) {
                            hex += next();
                        }
                        result += "?";
                        break;
                    }
                    default: result += esc; break;
                }
            } else {
                result += c;
            }
        }
        throw std::runtime_error("Unterminated string in JSON");
    }

    JsonValue parse_array() {
        next(); // skip [
        JsonValue arr;
        arr.kind = JsonKind::array_t;
        skip_ws();
        if (peek() == ']') { next(); return arr; }
        while (true) {
            arr.arr.push_back(parse_value());
            skip_ws();
            if (peek() == ',') { next(); continue; }
            if (peek() == ']') { next(); break; }
            throw std::runtime_error("Expected ',' or ']' in JSON array");
        }
        return arr;
    }

    JsonValue parse_object() {
        next(); // skip {
        JsonValue obj;
        obj.kind = JsonKind::object_t;
        skip_ws();
        if (peek() == '}') { next(); return obj; }
        while (true) {
            skip_ws();
            std::string key = parse_string();
            skip_ws();
            if (peek() != ':') throw std::runtime_error("Expected ':' in JSON object");
            next();
            auto val = parse_value();
            obj.obj.emplace_back(std::move(key), std::move(val));
            skip_ws();
            if (peek() == ',') { next(); continue; }
            if (peek() == '}') { next(); break; }
            throw std::runtime_error("Expected ',' or '}' in JSON object");
        }
        return obj;
    }

    JsonValue parse_number() {
        std::size_t start = pos_;
        bool is_float = false;
        if (peek() == '-') next();
        while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) next();
        if (pos_ < input_.size() && input_[pos_] == '.') {
            is_float = true;
            next();
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) next();
        }
        if (pos_ < input_.size() && (input_[pos_] == 'e' || input_[pos_] == 'E')) {
            is_float = true;
            next();
            if (pos_ < input_.size() && (input_[pos_] == '+' || input_[pos_] == '-')) next();
            while (pos_ < input_.size() && std::isdigit(static_cast<unsigned char>(input_[pos_]))) next();
        }
        std::string num_str = std::string(input_.substr(start, pos_ - start));
        if (is_float) {
            return JsonValue(std::stod(num_str));
        }
        return JsonValue(static_cast<std::int64_t>(std::stoll(num_str)));
    }

    bool match(std::string_view s) {
        if (input_.substr(pos_, s.size()) == s) {
            pos_ += s.size();
            return true;
        }
        return false;
    }
};

JsonValue parse(std::string_view s) {
    return JsonParser(s).parse();
}

// ── JSON serializer ─────────────────────────────────────────────────────

static std::string indent_str(int n) {
    return std::string(n * 2, ' ');
}

std::string serialize(const JsonValue& v, int indent = 0, int current_indent = 0) {
    switch (v.kind) {
        case JsonKind::null_t:
            return "null";
        case JsonKind::bool_t:
            return v.bval ? "true" : "false";
        case JsonKind::int64_t:
            return std::to_string(v.ival);
        case JsonKind::double_t: {
            char buf[64];
            int len = std::snprintf(buf, sizeof(buf), "%.17g", v.dval);
            return std::string(buf, len);
        }
        case JsonKind::string_t: {
            std::string result = "\"";
            for (char c : v.sval) {
                switch (c) {
                    case '"': result += "\\\""; break;
                    case '\\': result += "\\\\"; break;
                    case '\n': result += "\\n"; break;
                    case '\t': result += "\\t"; break;
                    case '\r': result += "\\r"; break;
                    case '\b': result += "\\b"; break;
                    case '\f': result += "\\f"; break;
                    default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                            char buf[8];
                            std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                            result += buf;
                        } else {
                            result += c;
                        }
                        break;
                }
            }
            result += "\"";
            return result;
        }
        case JsonKind::array_t: {
            if (v.arr.empty()) return "[]";
            std::string result = "[\n";
            for (std::size_t i = 0; i < v.arr.size(); i++) {
                result += indent_str(current_indent + 1) + serialize(v.arr[i], indent, current_indent + 1);
                if (i + 1 < v.arr.size()) result += ",";
                result += "\n";
            }
            result += indent_str(current_indent) + "]";
            return result;
        }
        case JsonKind::object_t: {
            if (v.obj.empty()) return "{}";
            std::string result = "{\n";
            for (std::size_t i = 0; i < v.obj.size(); i++) {
                auto& [key, val] = v.obj[i];
                result += indent_str(current_indent + 1) + "\"" + key + "\": " + serialize(val, indent, current_indent + 1);
                if (i + 1 < v.obj.size()) result += ",";
                result += "\n";
            }
            result += indent_str(current_indent) + "}";
            return result;
        }
    }
    return "null";
}

// ── Message to/from JSON ─────────────────────────────────────────────────

static JsonValue content_block_to_json(const ContentBlock& block) {
    return std::visit(
        []<typename T>(const T& value) -> JsonValue {
            if constexpr (std::same_as<T, TextContent>) {
                JsonValue j;
                j.set<std::string_view>("type", "text");
                j.set<std::string_view>("text", value.text);
                if (value.text_signature) {
                    j.set<std::string_view>("textSignature", *value.text_signature);
                }
                return j;
            } else if constexpr (std::same_as<T, ThinkingContent>) {
                JsonValue j;
                j.set<std::string_view>("type", "thinking");
                j.set<std::string_view>("thinking", value.thinking);
                if (value.thinking_signature) {
                    j.set<std::string_view>("thinkingSignature", *value.thinking_signature);
                }
                j.set<bool>("redacted", value.redacted);
                return j;
            } else if constexpr (std::same_as<T, ImageContent>) {
                JsonValue j;
                j.set<std::string_view>("type", "image");
                j.set<std::string_view>("data", value.data);
                j.set<std::string_view>("mimeType", value.mime_type);
                return j;
            } else if constexpr (std::same_as<T, ToolCall>) {
                JsonValue j;
                j.set<std::string_view>("type", "toolCall");
                j.set<std::string_view>("id", value.id);
                j.set<std::string_view>("name", value.name);
                JsonValue args;
                for (const auto& [k, v] : value.arguments) {
                    args.set<std::string_view>(k, v);
                }
                j.set<JsonValue>("arguments", args);
                return j;
            }
            return JsonValue{};
        },
        block);
}

static std::optional<ContentBlock> json_to_content_block(const JsonValue& j) {
    auto type = j.get<std::string>("type");
    if (type == "text") {
        TextContent tc;
        tc.text = j.get<std::string>("text", "");
        if (j.has("textSignature")) {
            tc.text_signature = j.get<std::string>("textSignature");
        }
        return tc;
    }
    if (type == "thinking") {
        ThinkingContent tc;
        tc.thinking = j.get<std::string>("thinking", "");
        if (j.has("thinkingSignature")) {
            tc.thinking_signature = j.get<std::string>("thinkingSignature");
        }
        tc.redacted = j.get<bool>("redacted", false);
        return tc;
    }
    if (type == "image") {
        ImageContent ic;
        ic.data = j.get<std::string>("data", "");
        ic.mime_type = j.get<std::string>("mimeType", "");
        return ic;
    }
    if (type == "toolCall") {
        ToolCall tc;
        tc.id = j.get<std::string>("id", "");
        tc.name = j.get<std::string>("name", "");
        if (j.has("arguments")) {
            for (const auto& [k, v] : j["arguments"].obj) {
                tc.arguments[k] = v.sval;
            }
        }
        return tc;
    }
    return std::nullopt;
}

static JsonValue message_to_json(const Message& msg) {
    return std::visit(
        []<typename T>(const T& m) -> JsonValue {
            if constexpr (std::same_as<T, UserMessage>) {
                JsonValue j;
                j.set<std::string_view>("role", "user");
                j.set<std::int64_t>("timestamp", m.timestamp);
                JsonValue arr;
                for (const auto& cb : m.content) {
                    arr.arr.push_back(content_block_to_json(cb));
                }
                j.set<JsonValue>("content", arr);
                return j;
            } else if constexpr (std::same_as<T, AssistantMessage>) {
                JsonValue j;
                j.set<std::string_view>("role", "assistant");
                j.set<std::string_view>("api", m.api);
                j.set<std::string_view>("provider", m.provider);
                j.set<std::string_view>("model", m.model);
                j.set<std::string_view>("stopReason", stop_reason_to_string(m.stop_reason));
                j.set<std::int64_t>("timestamp", m.timestamp);

                {
                    JsonValue arr;
                    for (const auto& cb : m.content) {
                        arr.arr.push_back(content_block_to_json(cb));
                    }
                    j.set<JsonValue>("content", arr);
                }
                {
                    JsonValue usage;
                    usage.set<std::int64_t>("input", static_cast<std::int64_t>(m.usage.input));
                    usage.set<std::int64_t>("output", static_cast<std::int64_t>(m.usage.output));
                    usage.set<std::int64_t>("cacheRead", static_cast<std::int64_t>(m.usage.cache_read));
                    usage.set<std::int64_t>("cacheWrite", static_cast<std::int64_t>(m.usage.cache_write));
                    usage.set<std::int64_t>("totalTokens", static_cast<std::int64_t>(m.usage.total_tokens));
                    JsonValue cost;
                    cost.set<double>("input", m.usage.cost.input);
                    cost.set<double>("output", m.usage.cost.output);
                    cost.set<double>("cacheRead", m.usage.cost.cache_read);
                    cost.set<double>("cacheWrite", m.usage.cost.cache_write);
                    cost.set<double>("total", m.usage.cost.total);
                    usage.set<JsonValue>("cost", cost);
                    j.set<JsonValue>("usage", usage);
                }
                if (!m.response_model.empty()) {
                    j.set<std::string_view>("responseModel", m.response_model);
                }
                if (m.response_id.has_value()) {
                    j.set<std::string_view>("responseId", *m.response_id);
                }
                if (m.error_message.has_value()) {
                    j.set<std::string_view>("errorMessage", *m.error_message);
                }
                return j;
            } else if constexpr (std::same_as<T, ToolResultMessage>) {
                JsonValue j;
                j.set<std::string_view>("role", "toolResult");
                j.set<std::string_view>("toolCallId", m.tool_call_id);
                j.set<std::string_view>("toolName", m.tool_name);
                j.set<bool>("isError", m.is_error);
                j.set<std::int64_t>("timestamp", m.timestamp);
                {
                    JsonValue arr;
                    for (const auto& cb : m.content) {
                        arr.arr.push_back(content_block_to_json(cb));
                    }
                    j.set<JsonValue>("content", arr);
                }
                if (m.details.has_value()) {
                    j.set<std::string_view>("details", *m.details);
                }
                return j;
            }
            return JsonValue{};
        },
        msg);
}

std::string to_json(const TokenUsage& usage) {
    JsonValue j;
    j.set<std::int64_t>("input", static_cast<std::int64_t>(usage.input));
    j.set<std::int64_t>("output", static_cast<std::int64_t>(usage.output));
    j.set<std::int64_t>("cacheRead", static_cast<std::int64_t>(usage.cache_read));
    j.set<std::int64_t>("cacheWrite", static_cast<std::int64_t>(usage.cache_write));
    j.set<std::int64_t>("totalTokens", static_cast<std::int64_t>(usage.total_tokens));
    return serialize(j);
}

std::string to_json(const Message& msg) {
    return serialize(message_to_json(msg), 2);
}

std::string to_json(const Model& model) {
    JsonValue j;
    j.set<std::string_view>("id", model.id);
    j.set<std::string_view>("name", model.name);
    j.set<std::string_view>("api", model.api);
    j.set<std::string_view>("provider", model.provider);
    j.set<std::string_view>("baseUrl", model.base_url);
    j.set<bool>("reasoning", model.reasoning);
    {
        JsonValue arr;
        for (const auto& cap : model.input_capabilities) {
            arr.arr.push_back(JsonValue(cap));
        }
        j.set<JsonValue>("input", arr);
    }
    j.set<std::int64_t>("contextWindow", static_cast<std::int64_t>(model.context_window));
    j.set<std::int64_t>("maxTokens", static_cast<std::int64_t>(model.max_tokens));
    return serialize(j, 2);
}

static Message from_json_message(const JsonValue& j) {
    auto role = j.get<std::string>("role");
    if (role == "user") {
        UserMessage msg;
        msg.timestamp = j.get<std::int64_t>("timestamp", 0LL);
        if (j.has("content")) {
            for (const auto& cb : j["content"].arr) {
                auto block = json_to_content_block(cb);
                if (block) msg.content.push_back(*block);
            }
        }
        return msg;
    }
    if (role == "assistant") {
        AssistantMessage msg;
        msg.api = j.get<std::string>("api", "");
        msg.provider = j.get<std::string>("provider", "");
        msg.model = j.get<std::string>("model", "");
        msg.stop_reason = stop_reason_from_string(j.get<std::string>("stopReason", ""));
        msg.timestamp = j.get<std::int64_t>("timestamp", 0LL);
        if (j.has("responseModel")) {
            msg.response_model = j.get<std::string>("responseModel");
        }
        if (j.has("responseId")) {
            msg.response_id = j.get<std::string>("responseId");
        }
        if (j.has("errorMessage")) {
            msg.error_message = j.get<std::string>("errorMessage");
        }
        if (j.has("content")) {
            for (const auto& cb : j["content"].arr) {
                auto block = json_to_content_block(cb);
                if (block) msg.content.push_back(*block);
            }
        }
        if (j.has("usage")) {
            const auto& u = j["usage"];
            msg.usage.input = static_cast<std::uint64_t>(u.get<std::int64_t>("input", 0));
            msg.usage.output = static_cast<std::uint64_t>(u.get<std::int64_t>("output", 0));
            msg.usage.cache_read = static_cast<std::uint64_t>(u.get<std::int64_t>("cacheRead", 0));
            msg.usage.cache_write = static_cast<std::uint64_t>(u.get<std::int64_t>("cacheWrite", 0));
            msg.usage.total_tokens = static_cast<std::uint64_t>(u.get<std::int64_t>("totalTokens", 0));
            if (j["usage"].has("cost")) {
                const auto& c = j["usage"]["cost"];
                msg.usage.cost.input = c.get<double>("input", 0.0);
                msg.usage.cost.output = c.get<double>("output", 0.0);
                msg.usage.cost.cache_read = c.get<double>("cacheRead", 0.0);
                msg.usage.cost.cache_write = c.get<double>("cacheWrite", 0.0);
                msg.usage.cost.total = c.get<double>("total", 0.0);
            }
        }
        return msg;
    }
    if (role == "toolResult") {
        ToolResultMessage msg;
        msg.tool_call_id = j.get<std::string>("toolCallId", "");
        msg.tool_name = j.get<std::string>("toolName", "");
        msg.is_error = j.get<bool>("isError", false);
        msg.timestamp = j.get<std::int64_t>("timestamp", 0LL);
        if (j.has("content")) {
            for (const auto& cb : j["content"].arr) {
                auto block = json_to_content_block(cb);
                if (block) msg.content.push_back(*block);
            }
        }
        if (j.has("details")) {
            msg.details = j.get<std::string>("details");
        }
        return msg;
    }
    throw std::runtime_error("Unknown message role: " + role);
}

std::optional<Message> from_json(const std::string& s) {
    try {
        auto j = parse(s);
        return from_json_message(j);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<Message> from_json(std::string_view s) {
    return from_json(std::string(s));
}

} // namespace json

} // namespace pi::core
