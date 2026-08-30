#include <optional>
#include <string>

#include "core/message_types.h"
#include "core/tool_validation.h"

#include <gtest/gtest.h>

using namespace pi::core;

// ─── Test 1: Required field missing ───────────────────────────────────────

TEST(ToolValidator, RequiredFieldMissing) {
  std::string schema =
      R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"]})";
  ToolArguments args = nlohmann::json::object();
  auto result = ToolValidator::validate("test_tool", schema, args);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->find("x") != std::string::npos);
  EXPECT_TRUE(result->find("Validation failed for tool") != std::string::npos);
}

// ─── Test 2: Type mismatch (no coercion possible) ─────────────────────────

TEST(ToolValidator, TypeMismatch) {
  std::string schema =
      R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})";
  ToolArguments args = {{"n", "abc"}};
  auto result = ToolValidator::validate("my_tool", schema, args);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->find("Validation failed") != std::string::npos);
}

// ─── Test 3: String→number coercion ──────────────────────────────────────

TEST(ToolValidator, StringToNumberCoercion) {
  std::string schema =
      R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})";
  ToolArguments args = {{"n", "42"}};
  auto result = ToolValidator::validate("my_tool", schema, args);
  EXPECT_TRUE(!result.has_value());
  EXPECT_TRUE(args["n"].is_number_integer());
  EXPECT_EQ(args["n"].get<int>(), 42);
}

// ─── Test 4: String→boolean coercion ─────────────────────────────────────

TEST(ToolValidator, StringToBooleanCoercion) {
  std::string schema =
      R"({"type":"object","properties":{"flag":{"type":"boolean"}},"required":["flag"]})";
  ToolArguments args = {{"flag", "true"}};
  auto result = ToolValidator::validate("my_tool", schema, args);
  EXPECT_TRUE(!result.has_value());
  EXPECT_TRUE(args["flag"].is_boolean());
  EXPECT_EQ(args["flag"].get<bool>(), true);
}

// ─── Test 5: Null→default coercion (string) ───────────────────────────────

TEST(ToolValidator, NullToStringCoercion) {
  std::string schema =
      R"({"type":"object","properties":{"s":{"type":"string"}},"required":["s"]})";
  ToolArguments args = {{"s", nullptr}};
  auto result = ToolValidator::validate("my_tool", schema, args);
  EXPECT_TRUE(!result.has_value());
  EXPECT_TRUE(args["s"].is_string());
  EXPECT_EQ(args["s"].get<std::string>(), "");
}

// ─── Test 6: AdditionalProperties false ───────────────────────────────────

TEST(ToolValidator, AdditionalPropertiesFalse) {
  std::string schema =
      R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"],"additionalProperties":false})";
  ToolArguments args = {{"x", 1}, {"extra", "oops"}};
  auto result = ToolValidator::validate("my_tool", schema, args);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->find("Validation failed") != std::string::npos);
}

// ─── Test 7: Nested object coercion ───────────────────────────────────────

TEST(ToolValidator, NestedObjectCoercion) {
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
  EXPECT_TRUE(!result.has_value());
  EXPECT_TRUE(args["inner"]["n"].is_number_integer());
  EXPECT_EQ(args["inner"]["n"].get<int>(), 5);
}

// ─── Test 8: Array items coercion ─────────────────────────────────────────

TEST(ToolValidator, ArrayItemsCoercion) {
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
  EXPECT_TRUE(!result.has_value());
  EXPECT_TRUE(args["nums"][0].is_number_integer());
  EXPECT_EQ(args["nums"][0].get<int>(), 1);
  EXPECT_TRUE(args["nums"][1].is_number_integer());
  EXPECT_EQ(args["nums"][1].get<int>(), 2);
}

// ─── Test 9: Multiple errors aggregated ───────────────────────────────────

TEST(ToolValidator, MultipleErrorsAggregated) {
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
  ASSERT_TRUE(result.has_value());
  // Both missing fields should appear
  EXPECT_TRUE(result->find("a") != std::string::npos);
  EXPECT_TRUE(result->find("b") != std::string::npos);
}

// ─── Test 10: Received arguments preserved (pre-coercion) ─────────────────

TEST(ToolValidator, ReceivedArgumentsPreserved) {
  std::string schema =
      R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x","missing"]})";
  ToolArguments args = {{"x", "not_a_number"}};
  auto result = ToolValidator::validate("my_tool", schema, args);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->find("Received arguments:") != std::string::npos);
  // The original value "not_a_number" should appear in the error
  EXPECT_TRUE(result->find("not_a_number") != std::string::npos);
}

// ─── Test 11: Schema parse failure ────────────────────────────────────────

TEST(ToolValidator, SchemaParseFailure) {
  std::string schema = "{not valid json";
  ToolArguments args = nlohmann::json::object();
  auto result = ToolValidator::validate("my_tool", schema, args);
  ASSERT_TRUE(result.has_value());
  EXPECT_TRUE(result->find("Invalid schema") != std::string::npos);
  EXPECT_TRUE(result->find("my_tool") != std::string::npos);
}

// ─── Test 12: Cache hit (same schema twice, no crash) ─────────────────────

TEST(ToolValidator, CacheHit) {
  std::string schema =
      R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})";

  ToolArguments args1 = {{"n", "7"}};
  auto result1 = ToolValidator::validate("tool", schema, args1);
  EXPECT_TRUE(!result1.has_value());
  EXPECT_EQ(args1["n"].get<int>(), 7);

  ToolArguments args2 = {{"n", "99"}};
  auto result2 = ToolValidator::validate("tool", schema, args2);
  EXPECT_TRUE(!result2.has_value());
  EXPECT_EQ(args2["n"].get<int>(), 99);
}

// ─── Main ──────────────────────────────────────────────────────────────────
