#include "core/tool_validation.h"

#include <cmath>
#include <mutex>
#include <nlohmann/json-schema.hpp>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace pi::core {

namespace {

using njson = nlohmann::json;
using nlohmann::json_schema::json_validator;
using nlohmann::json_schema::error_handler;

// ─── Coercion (mirrors validation.ts::coerceWithJsonSchema) ───────────────

static njson coerce_with_schema(const njson& value, const njson& schema);

static njson coerce_primitive_by_type(const njson& value, const std::string& type) {
    if (type == "number") {
        if (value.is_null()) return 0;
        if (value.is_string()) {
            const auto& s = value.get<std::string>();
            if (!s.empty()) {
                try {
                    double d = std::stod(s);
                    if (std::isfinite(d)) return d;
                } catch (...) {}
            }
        }
        if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
        return value;
    }
    if (type == "integer") {
        if (value.is_null()) return 0;
        if (value.is_string()) {
            const auto& s = value.get<std::string>();
            if (!s.empty()) {
                try {
                    double d = std::stod(s);
                    if (std::isfinite(d) && d == std::floor(d)) {
                        return static_cast<std::int64_t>(d);
                    }
                } catch (...) {}
            }
        }
        if (value.is_boolean()) return value.get<bool>() ? 1 : 0;
        return value;
    }
    if (type == "boolean") {
        if (value.is_null()) return false;
        if (value.is_string()) {
            const auto& s = value.get<std::string>();
            if (s == "true") return true;
            if (s == "false") return false;
        }
        if (value.is_number()) {
            double d = value.get<double>();
            if (d == 1.0) return true;
            if (d == 0.0) return false;
        }
        return value;
    }
    if (type == "string") {
        if (value.is_null()) return "";
        if (value.is_number()) return value.dump();
        if (value.is_boolean()) {
            return value.get<bool>() ? std::string("true") : std::string("false");
        }
        return value;
    }
    if (type == "null") {
        if (value.is_string() && value.get<std::string>().empty()) return nullptr;
        if (value.is_number() && value.get<double>() == 0.0) return nullptr;
        if (value.is_boolean() && !value.get<bool>()) return nullptr;
        return value;
    }
    return value;
}

static void apply_object_coercion(njson& obj, const njson& schema) {
    if (!obj.is_object()) return;

    if (schema.contains("properties") && schema["properties"].is_object()) {
        for (auto& [key, prop_schema] : schema["properties"].items()) {
            if (obj.contains(key)) {
                obj[key] = coerce_with_schema(obj[key], prop_schema);
            }
        }
    }

    if (schema.contains("additionalProperties") &&
        schema["additionalProperties"].is_object()) {
        const auto& add_schema = schema["additionalProperties"];
        std::vector<std::string> defined_keys;
        if (schema.contains("properties") && schema["properties"].is_object()) {
            for (auto& [k, _] : schema["properties"].items()) {
                defined_keys.push_back(k);
            }
        }
        for (auto& [key, val] : obj.items()) {
            bool is_defined = false;
            for (const auto& dk : defined_keys) {
                if (dk == key) { is_defined = true; break; }
            }
            if (!is_defined) {
                obj[key] = coerce_with_schema(val, add_schema);
            }
        }
    }
}

static void apply_array_coercion(njson& arr, const njson& schema) {
    if (!arr.is_array()) return;
    if (!schema.contains("items")) return;

    const auto& items = schema["items"];
    if (items.is_array()) {
        for (std::size_t i = 0; i < arr.size(); ++i) {
            if (i < items.size()) {
                arr[i] = coerce_with_schema(arr[i], items[i]);
            }
        }
    } else if (items.is_object()) {
        for (auto& elem : arr) {
            elem = coerce_with_schema(elem, items);
        }
    }
}

static bool matches_json_type(const njson& value, const std::string& type) {
    if (type == "number") return value.is_number();
    if (type == "integer") {
        if (value.is_number_integer()) return true;
        if (value.is_number_float()) {
            double d = value.get<double>();
            return d == std::floor(d);
        }
        return false;
    }
    if (type == "boolean") return value.is_boolean();
    if (type == "string") return value.is_string();
    if (type == "null") return value.is_null();
    if (type == "array") return value.is_array();
    if (type == "object") return value.is_object();
    return false;
}

static njson coerce_with_schema(const njson& value, const njson& schema) {
    if (!schema.is_object()) return value;

    njson next = value;

    if (schema.contains("allOf") && schema["allOf"].is_array()) {
        for (const auto& nested : schema["allOf"]) {
            next = coerce_with_schema(next, nested);
        }
    }

    auto try_union = [&](const njson& schemas) -> njson {
        for (const auto& sub : schemas) {
            njson candidate = coerce_with_schema(next, sub);
            if (sub.contains("type")) {
                const auto& t = sub["type"];
                if (t.is_string() && matches_json_type(candidate, t.get<std::string>())) {
                    return candidate;
                }
            } else {
                return candidate;
            }
        }
        return next;
    };

    if (schema.contains("anyOf") && schema["anyOf"].is_array()) {
        next = try_union(schema["anyOf"]);
    }
    if (schema.contains("oneOf") && schema["oneOf"].is_array()) {
        next = try_union(schema["oneOf"]);
    }

    std::vector<std::string> schema_types;
    if (schema.contains("type")) {
        const auto& t = schema["type"];
        if (t.is_string()) {
            schema_types.push_back(t.get<std::string>());
        } else if (t.is_array()) {
            for (const auto& st : t) {
                if (st.is_string()) schema_types.push_back(st.get<std::string>());
            }
        }
    }

    if (!schema_types.empty()) {
        bool matches_union = schema_types.size() > 1 &&
            std::any_of(schema_types.begin(), schema_types.end(),
                        [&](const auto& st) { return matches_json_type(next, st); });

        if (!matches_union) {
            for (const auto& st : schema_types) {
                njson candidate = coerce_primitive_by_type(next, st);
                if (candidate != next) {
                    next = candidate;
                    break;
                }
            }
        }
    }

    bool is_object_type = std::find(schema_types.begin(), schema_types.end(), "object") != schema_types.end();
    if (is_object_type && next.is_object()) {
        apply_object_coercion(next, schema);
    }

    bool is_array_type = std::find(schema_types.begin(), schema_types.end(), "array") != schema_types.end();
    if (is_array_type && next.is_array()) {
        apply_array_coercion(next, schema);
    }

    return next;
}

// ─── Validator cache ───────────────────────────────────────────────────────

static std::mutex g_cache_mutex;
static std::unordered_map<std::string, json_validator> g_validator_cache;

static json_validator& get_or_create_validator(
    const std::string& schema_str, const njson& schema_json) {
    // g_cache_mutex must be held by caller
    auto it = g_validator_cache.find(schema_str);
    if (it != g_validator_cache.end()) {
        return it->second;
    }
    auto& v = g_validator_cache.emplace(schema_str, json_validator{}).first->second;
    v.set_root_schema(schema_json);
    return v;
}

// ─── Error collector ───────────────────────────────────────────────────────

struct ValidationError {
    std::string path;
    std::string message;
};

static std::string path_from_pointer(const nlohmann::json::json_pointer& ptr) {
    std::string p = ptr.to_string();
    if (p.empty() || p == "/") return "root";
    if (!p.empty() && p[0] == '/') p = p.substr(1);
    for (char& c : p) {
        if (c == '/') c = '.';
    }
    return p;
}

class CollectingErrorHandler : public error_handler {
public:
    std::vector<ValidationError> errors;

    void error(const nlohmann::json::json_pointer& ptr,
               const njson& /*instance*/,
               const std::string& message) override {
        errors.push_back({path_from_pointer(ptr), message});
    }
};

} // anonymous namespace

// ─── ToolValidator::validate ───────────────────────────────────────────────

std::optional<std::string> ToolValidator::validate(
    std::string_view tool_name,
    std::string_view schema_json_str,
    ToolArguments& arguments) {

    ToolArguments original = arguments;

    njson schema_json;
    try {
        schema_json = njson::parse(schema_json_str);
    } catch (const std::exception& e) {
        return std::string("Invalid schema for tool \"") + std::string(tool_name) +
               "\": " + e.what();
    }

    // Apply coercion in place
    njson coerced = coerce_with_schema(arguments, schema_json);
    arguments = coerced;

    CollectingErrorHandler err_handler;
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        json_validator& validator = get_or_create_validator(
            std::string(schema_json_str), schema_json);
        try {
            validator.validate(arguments, err_handler);
        } catch (const std::exception& e) {
            err_handler.errors.push_back({"root", e.what()});
        }
    }

    if (err_handler.errors.empty()) return std::nullopt;

    std::ostringstream oss;
    oss << "Validation failed for tool \"" << tool_name << "\":\n";
    for (const auto& err : err_handler.errors) {
        oss << "  - " << err.path << ": " << err.message << "\n";
    }
    oss << "\nReceived arguments:\n";
    oss << original.dump(2);

    return oss.str();
}

// ─── validate_tool_arguments ───────────────────────────────────────────────

std::optional<std::string> validate_tool_arguments(
    const ToolDefinition& tool,
    const ToolCall& tool_call,
    ToolArguments& mutable_arguments) {
    return ToolValidator::validate(
        tool.name(),
        tool.schema().serialize(),
        mutable_arguments);
}

} // namespace pi::core
