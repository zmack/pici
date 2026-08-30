#include "core/session/agent_session.h"
#include "core/session/session_store.h"
#include "core/sandbox.h"

#include <filesystem>
#include <iostream>
#include <memory>
#include <source_location>
#include <string_view>

namespace {

int passed = 0;
int failed = 0;

void check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
  if (condition) {
    ++passed;
    return;
  }
  ++failed;
  std::cerr << "FAIL " << location.file_name() << ":" << location.line()
            << " - " << expression << '\n';
}

#define CHECK(value) check(static_cast<bool>(value), #value)

void test_modes() {
  CHECK(pi::core::sandbox_mode_from_string("auto") ==
        pi::core::SandboxMode::auto_mode);
  CHECK(pi::core::sandbox_mode_from_string("required") ==
        pi::core::SandboxMode::required);
  CHECK(pi::core::sandbox_mode_from_string("off") ==
        pi::core::SandboxMode::disabled);
  CHECK(!pi::core::sandbox_mode_from_string("unknown"));
  CHECK(pi::core::sandbox_mode_to_string(pi::core::SandboxMode::disabled) ==
        "disabled");
  pi::core::SandboxLauncher::validate(pi::core::SandboxMode::disabled);
}

void test_session_round_trip() {
  const auto root = std::filesystem::temp_directory_path() / "pici-sandbox-test";
  std::filesystem::remove_all(root);

  auto store = std::make_shared<pi::core::SessionStore>(root);
  auto policy = std::make_shared<pi::core::SandboxPolicy>(
      pi::core::SandboxMode::disabled);
  pi::core::SessionRuntime session({.session_store = store,
                                  .sandbox_policy = policy});
  pi::core::SessionHeader header{.id = "sandbox-session",
                                 .model = "test",
                                 .provider = "test",
                                 .sandbox_mode = "disabled"};
  const auto id = session.create_session(header);
  CHECK(id == "sandbox-session");

  auto loaded = session.load_session(id);
  CHECK(loaded.has_value());
  CHECK(loaded && loaded->header.sandbox_mode == "disabled");

  session.set_sandbox_mode(pi::core::SandboxMode::required);
  CHECK(session.sandbox_mode() == pi::core::SandboxMode::required);
  session.activate_session(*loaded);
  CHECK(session.sandbox_mode() == pi::core::SandboxMode::disabled);

  std::filesystem::remove_all(root);
}

} // namespace

int main() {
  test_modes();
  test_session_round_trip();
  std::cout << "sandbox tests: " << passed << " passed, " << failed
            << " failed\n";
  return failed == 0 ? 0 : 1;
}
