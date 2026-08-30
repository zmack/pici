#include "core/auth/openai_codex_oauth.h"

#include <gtest/gtest.h>

#include <iostream>
#include <nlohmann/json.hpp>
#include <source_location>
#include <string_view>

TEST(OpenAICodexOAuth, EncodingAndAccountExtraction) {
  EXPECT_EQ(pi::auth::base64url_encode("hello"), std::string("aGVsbG8"));
  auto decoded = pi::auth::base64url_decode("aGVsbG8");
  EXPECT_TRUE(decoded.has_value());
  EXPECT_EQ(*decoded, std::string("hello"));
  EXPECT_TRUE(!pi::auth::base64url_decode("aGVsbG=").has_value());
  EXPECT_TRUE(!pi::auth::base64url_decode("a").has_value());

  constexpr std::string_view verifier =
      "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
  EXPECT_EQ(pi::auth::pkce_challenge(verifier),
            std::string("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));
  const auto generated = pi::auth::generate_pkce();
  EXPECT_EQ(generated.verifier.size(), std::size_t{64});
  EXPECT_EQ(generated.challenge, pi::auth::pkce_challenge(generated.verifier));

  const nlohmann::json payload = {
      {"https://api.openai.com/auth", {{"chatgpt_account_id", "acct-test"}}}};
  const auto jwt = std::string("header.") +
                   pi::auth::base64url_encode(payload.dump()) + ".signature";
  auto account = pi::auth::extract_chatgpt_account_id(jwt);
  EXPECT_TRUE(account.has_value());
  EXPECT_EQ(*account, std::string("acct-test"));
  EXPECT_TRUE(!pi::auth::extract_chatgpt_account_id("not-a-jwt").has_value());
}
