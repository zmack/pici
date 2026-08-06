#include "core/auth/openai_codex_oauth.h"

#include <iostream>
#include <nlohmann/json.hpp>
#include <source_location>
#include <string_view>

namespace tests {
int passed{0};
int failed{0};
int total{0};

bool check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
  ++total;
  if (condition) {
    ++passed;
    return true;
  }
  ++failed;
  std::cout << "  FAIL " << location.file_name() << ":" << location.line()
            << " — " << expression << "\n";
  return false;
}
} // namespace tests

#define CHECK(expression) tests::check((expression), #expression)
#define CHECK_EQ(left, right) tests::check((left) == (right), #left " == " #right)

int main() {
  CHECK_EQ(pi::auth::base64url_encode("hello"), std::string("aGVsbG8"));
  auto decoded = pi::auth::base64url_decode("aGVsbG8");
  CHECK(decoded.has_value());
  CHECK_EQ(*decoded, std::string("hello"));
  CHECK(!pi::auth::base64url_decode("aGVsbG=").has_value());
  CHECK(!pi::auth::base64url_decode("a").has_value());

  constexpr std::string_view verifier =
      "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
  CHECK_EQ(pi::auth::pkce_challenge(verifier),
           std::string("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM"));
  const auto generated = pi::auth::generate_pkce();
  CHECK_EQ(generated.verifier.size(), std::size_t{64});
  CHECK_EQ(generated.challenge, pi::auth::pkce_challenge(generated.verifier));

  const nlohmann::json payload = {
      {"https://api.openai.com/auth", {{"chatgpt_account_id", "acct-test"}}}};
  const auto jwt = std::string("header.") +
                   pi::auth::base64url_encode(payload.dump()) + ".signature";
  auto account = pi::auth::extract_chatgpt_account_id(jwt);
  CHECK(account.has_value());
  CHECK_EQ(*account, std::string("acct-test"));
  CHECK(!pi::auth::extract_chatgpt_account_id("not-a-jwt").has_value());

  std::cout << "\nTests: " << tests::total << " total, " << tests::passed
            << " passed, " << tests::failed << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
