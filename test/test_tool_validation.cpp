#include <functional>
#include <iostream>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>

#include "core/message_types.h"
#include "core/tool_validation.h"

using namespace pi::core;

// ─── Simple test harness (matches test_core.cpp style) ────────────────────

namespace tests {

int passed{0};
int failed{0};
int total{0};
int current_failed{0};

bool CHECK_impl(bool cond, bool expected,
                std::string_view expr,
                std::source_location loc = std::source_location::current()) {
    if (cond != expected) {
        current_failed++;
        std::cerr << "  FAIL " << loc.file_name() << ":" << loc.line()
                  << " - " << expr << " (expected " << expected << ", got " << cond << ")\n";
        return false;
    }
    return true;
}

#define CHECK(cond) \
    (::tests::CHECK_impl(static_cast<bool>(cond), true, #cond, \
                         std::source_location::current()))

#define CHECK_EQ(a, b) \
    (::tests::CHECK_impl((a) == (b), true, #a " == " #b, \
                         std::source_location::current()))

void register_test(std::string name, std::function<void()> fn) {
    total++;
    current_failed = 0;
    fn();
    if (current_failed == 0) {
        passed++;
        std::cout << "  PASS " << name << "\n";
    } else {
        failed++;
        std::cout << "  FAIL " << name << "\n";
    }
}

void print_summary() {
    std::cout << "\n========================================\n";
    std::cout << "  Tests: " << total << " total, "
              << passed << " passed, "
              << failed << " failed\n";
    std::cout << "========================================\n";
}

} // namespace tests

// ─── Test 1: Required field missing ───────────────────────────────────────

void test_required_field_missing() {
    tests::register_test("ToolValidator: required field missing", []() {
        std::string schema = R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"]})";
        ToolArguments args = nlohmann::json::object();
        auto result = ToolValidator::validate("test_tool", schema, args);
        CHECK(result.has_value());
        CHECK(result->find("x") != std::string::npos);
        CHECK(result->find("Validation failed for tool") != std::string::npos);
    });
}

// ─── Test 2: Type mismatch (no coercion possible) ─────────────────────────

void test_type_mismatch() {
    tests::register_test("ToolValidator: type mismatch after failed coercion", []() {
        std::string schema = R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})";
        ToolArguments args = {{"n", "abc"}};
        auto result = ToolValidator::validate("my_tool", schema, args);
        CHECK(result.has_value());
        CHECK(result->find("Validation failed") != std::string::npos);
    });
}

// ─── Test 3: String→number coercion ──────────────────────────────────────

void test_string_to_number_coercion() {
    tests::register_test("ToolValidator: string->integer coercion", []() {
        std::string schema = R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})";
        ToolArguments args = {{"n", "42"}};
        auto result = ToolValidator::validate("my_tool", schema, args);
        CHECK(!result.has_value());
        CHECK(args["n"].is_number_integer());
        CHECK_EQ(args["n"].get<int>(), 42);
    });
}

// ─── Test 4: String→boolean coercion ─────────────────────────────────────

void test_string_to_boolean_coercion() {
    tests::register_test("ToolValidator: string->boolean coercion", []() {
        std::string schema = R"({"type":"object","properties":{"flag":{"type":"boolean"}},"required":["flag"]})";
        ToolArguments args = {{"flag", "true"}};
        auto result = ToolValidator::validate("my_tool", schema, args);
        CHECK(!result.has_value());
        CHECK(args["flag"].is_boolean());
        CHECK_EQ(args["flag"].get<bool>(), true);
    });
}

// ─── Test 5: Null→default coercion (string) ───────────────────────────────

void test_null_to_string_coercion() {
    tests::register_test("ToolValidator: null->string coercion", []() {
        std::string schema = R"({"type":"object","properties":{"s":{"type":"string"}},"required":["s"]})";
        ToolArguments args = {{"s", nullptr}};
        auto result = ToolValidator::validate("my_tool", schema, args);
        CHECK(!result.has_value());
        CHECK(args["s"].is_string());
        CHECK_EQ(args["s"].get<std::string>(), "");
    });
}

// ─── Test 6: AdditionalProperties false ───────────────────────────────────

void test_additional_properties_false() {
    tests::register_test("ToolValidator: additionalProperties=false rejects extras", []() {
        std::string schema = R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"],"additionalProperties":false})";
        ToolArguments args = {{"x", 1}, {"extra", "oops"}};
        auto result = ToolValidator::validate("my_tool", schema, args);
        CHECK(result.has_value());
        CHECK(result->find("Validation failed") != std::string::npos);
    });
}

// ─── Test 7: Nested object coercion ───────────────────────────────────────

void test_nested_object_coercion() {
    tests::register_test("ToolValidator: nested object coercion", []() {
        std::string schema = R"({
            "type":"object",
            "properties":{
                "inner":{
                    "type":"object",
                    "properties":{
                        "n":{"type":"integer"}
                    }
                }
            }
        })";
        ToolArguments args = {{"inner", {{"n", "5"}}}};
        auto result = ToolValidator::validate("my_tool", schema, args);
        CHECK(!result.has_value());
        CHECK(args["inner"]["n"].is_number_integer());
        CHECK_EQ(args["inner"]["n"].get<int>(), 5);
    });
}

// ─── Test 8: Array items coercion ─────────────────────────────────────────

void test_array_items_coercion() {
    tests::register_test("ToolValidator: array items coercion", []() {
        std::string schema = R"({
            "type":"object",
            "properties":{
                "nums":{
                    "type":"array",
                    "items":{"type":"integer"}
                }
            }
        })";
        ToolArguments args = {{"nums", {"1", "2"}}};
        auto result = ToolValidator::validate("my_tool", schema, args);
        CHECK(!result.has_value());
        CHECK(args["nums"][0].is_number_integer());
        CHECK_EQ(args["nums"][0].get<int>(), 1);
        CHECK(args["nums"][1].is_number_integer());
        CHECK_EQ(args["nums"][1].get<int>(), 2);
    });
}

// ─── Test 9: Multiple errors aggregated ───────────────────────────────────

void test_multiple_errors_aggregated() {
    tests::register_test("ToolValidator: multiple errors aggregated", []() {
        std::string schema = R"({
            "type":"object",
            "properties":{
                "a":{"type":"integer"},
                "b":{"type":"integer"}
            },
            "required":["a","b"]
        })";
        ToolArguments args = nlohmann::json::object();
        auto result = ToolValidator::validate("my_tool", schema, args);
        CHECK(result.has_value());
        // Both missing fields should appear
        CHECK(result->find("a") != std::string::npos);
        CHECK(result->find("b") != std::string::npos);
    });
}

// ─── Test 10: Received arguments preserved (pre-coercion) ─────────────────

void test_received_arguments_preserved() {
    tests::register_test("ToolValidator: received arguments show original", []() {
        std::string schema = R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x","missing"]})";
        ToolArguments args = {{"x", "not_a_number"}};
        auto result = ToolValidator::validate("my_tool", schema, args);
        CHECK(result.has_value());
        CHECK(result->find("Received arguments:") != std::string::npos);
        // The original value "not_a_number" should appear in the error
        CHECK(result->find("not_a_number") != std::string::npos);
    });
}

// ─── Test 11: Schema parse failure ────────────────────────────────────────

void test_schema_parse_failure() {
    tests::register_test("ToolValidator: invalid schema JSON returns error", []() {
        std::string schema = "{not valid json";
        ToolArguments args = nlohmann::json::object();
        auto result = ToolValidator::validate("my_tool", schema, args);
        CHECK(result.has_value());
        CHECK(result->find("Invalid schema") != std::string::npos);
        CHECK(result->find("my_tool") != std::string::npos);
    });
}

// ─── Test 12: Cache hit (same schema twice, no crash) ─────────────────────

void test_cache_hit() {
    tests::register_test("ToolValidator: cache hit — repeat calls work correctly", []() {
        std::string schema = R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})";

        ToolArguments args1 = {{"n", "7"}};
        auto result1 = ToolValidator::validate("tool", schema, args1);
        CHECK(!result1.has_value());
        CHECK_EQ(args1["n"].get<int>(), 7);

        ToolArguments args2 = {{"n", "99"}};
        auto result2 = ToolValidator::validate("tool", schema, args2);
        CHECK(!result2.has_value());
        CHECK_EQ(args2["n"].get<int>(), 99);
    });
}

// ─── Main ──────────────────────────────────────────────────────────────────

int main() {
    std::cout << "=== pi-cpp tool validation tests ===\n\n";

    test_required_field_missing();
    test_type_mismatch();
    test_string_to_number_coercion();
    test_string_to_boolean_coercion();
    test_null_to_string_coercion();
    test_additional_properties_false();
    test_nested_object_coercion();
    test_array_items_coercion();
    test_multiple_errors_aggregated();
    test_received_arguments_preserved();
    test_schema_parse_failure();
    test_cache_hit();

    tests::print_summary();

    return tests::failed > 0 ? 1 : 0;
}
