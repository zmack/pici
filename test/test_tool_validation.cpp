#include <optional>
#include <string>

#include "core/message_types.h"
#include "core/tool_validation.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace pi::core;

// ─── Test 1: Required field missing ───────────────────────────────────────

TEST(ToolValidator, RequiredFieldMissing) {
  std::string schema =
      R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x"]})";
  ToolArguments args = nlohmann::json::object();
  auto result = ToolValidator::validate("test_tool", schema, args);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, testing::HasSubstr("x"));
  EXPECT_THAT(*result, testing::HasSubstr("Validation failed for tool"));
}

// ─── Test 2: Type mismatch (no coercion possible) ─────────────────────────

TEST(ToolValidator, TypeMismatch) {
  std::string schema =
      R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})";
  ToolArguments args = {{"n", "abc"}};
  auto result = ToolValidator::validate("my_tool", schema, args);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, testing::HasSubstr("Validation failed"));
}

// ─── Test 3: String→number coercion ──────────────────────────────────────

TEST(ToolValidator, StringToNumberCoercion) {
  std::string schema =
      R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})";
  ToolArguments args = {{"n", "42"}};
  auto result = ToolValidator::validate("my_tool", schema, args);
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(args["n"].is_number_integer());
  EXPECT_EQ(args["n"].get<int>(), 42);
}

// ─── Test 4: String→boolean coercion ─────────────────────────────────────

TEST(ToolValidator, StringToBooleanCoercion) {
  std::string schema =
      R"({"type":"object","properties":{"flag":{"type":"boolean"}},"required":["flag"]})";
  ToolArguments args = {{"flag", "true"}};
  auto result = ToolValidator::validate("my_tool", schema, args);
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(args["flag"].is_boolean());
  EXPECT_EQ(args["flag"].get<bool>(), true);
}

// ─── Test 5: Null→default coercion (string) ───────────────────────────────

TEST(ToolValidator, NullToStringCoercion) {
  std::string schema =
      R"({"type":"object","properties":{"s":{"type":"string"}},"required":["s"]})";
  ToolArguments args = {{"s", nullptr}};
  auto result = ToolValidator::validate("my_tool", schema, args);
  EXPECT_FALSE(result.has_value());
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
  EXPECT_THAT(*result, testing::HasSubstr("Validation failed"));
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
  EXPECT_FALSE(result.has_value());
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
  EXPECT_FALSE(result.has_value());
  ASSERT_THAT(args["nums"], testing::SizeIs(2));
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
  EXPECT_THAT(*result, testing::HasSubstr("a"));
  EXPECT_THAT(*result, testing::HasSubstr("b"));
}

// ─── Test 10: Received arguments preserved (pre-coercion) ─────────────────

TEST(ToolValidator, ReceivedArgumentsPreserved) {
  std::string schema =
      R"({"type":"object","properties":{"x":{"type":"integer"}},"required":["x","missing"]})";
  ToolArguments args = {{"x", "not_a_number"}};
  auto result = ToolValidator::validate("my_tool", schema, args);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, testing::HasSubstr("Received arguments:"));
  // The original value "not_a_number" should appear in the error
  EXPECT_THAT(*result, testing::HasSubstr("not_a_number"));
}

// ─── Test 11: Schema parse failure ────────────────────────────────────────

TEST(ToolValidator, SchemaParseFailure) {
  std::string schema = "{not valid json";
  ToolArguments args = nlohmann::json::object();
  auto result = ToolValidator::validate("my_tool", schema, args);
  ASSERT_TRUE(result.has_value());
  EXPECT_THAT(*result, testing::HasSubstr("Invalid schema"));
  EXPECT_THAT(*result, testing::HasSubstr("my_tool"));
}

// ─── Test 12: Cache hit (same schema twice, no crash) ─────────────────────

TEST(ToolValidator, CacheHit) {
  std::string schema =
      R"({"type":"object","properties":{"n":{"type":"integer"}},"required":["n"]})";

  ToolArguments args1 = {{"n", "7"}};
  auto result1 = ToolValidator::validate("tool", schema, args1);
  EXPECT_FALSE(result1.has_value());
  EXPECT_EQ(args1["n"].get<int>(), 7);

  ToolArguments args2 = {{"n", "99"}};
  auto result2 = ToolValidator::validate("tool", schema, args2);
  EXPECT_FALSE(result2.has_value());
  EXPECT_EQ(args2["n"].get<int>(), 99);
}

// ─── Test 13: get_or_create_validator's cache-insert-order fix ────────────
//
// A schema whose set_root_schema() throws (e.g. an unresolvable local
// $ref -- nlohmann::json_schema::json_validator confirmed, by reading its
// source, to throw std::invalid_argument in this case) reaches
// get_or_create_validator via ToolValidator::validate(), which has no
// try/catch around that call (only around the later validator.validate()
// call) -- so the first call below throwing is expected and unchanged by
// this fix.
//
// The fix is about the SECOND call with the exact same schema string.
// Before the fix, g_validator_cache.emplace(schema_str, json_validator{})
// ran BEFORE set_root_schema(), so the first call's throw still left an
// empty (rootless) validator permanently cached under that schema string.
// The second call would then hit the cache, skip set_root_schema entirely,
// and call validate() on a validator with no root schema set -- which
// doesn't crash (confirmed by reading root_schema::validate: it reports
// "no root schema has yet been set..." via the error_handler) but silently
// degrades every later call for that tool into an ordinary validation
// failure instead of the same clear throw. After the fix, nothing is
// cached on failure, so the second call retries set_root_schema fresh and
// throws again, consistently -- proving the schema string isn't
// permanently poisoned.
TEST(ToolValidator, MalformedSchemaThrowDoesNotPoisonCacheForRetry) {
  const std::string schema =
      R"({"type":"object","properties":{"x":{"$ref":"#/definitions/Missing"}}})";

  ToolArguments args1 = nlohmann::json::object();
  EXPECT_THROW(ToolValidator::validate("tool", schema, args1),
              std::invalid_argument);

  ToolArguments args2 = nlohmann::json::object();
  EXPECT_THROW(ToolValidator::validate("tool", schema, args2),
              std::invalid_argument)
      << "a poisoned cache entry from the first (thrown) call would make "
         "this second call against the SAME schema string silently return "
         "a validation-failed string instead of throwing again";
}

// ─── Main ──────────────────────────────────────────────────────────────────
