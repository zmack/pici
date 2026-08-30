#include "core/sandbox.h"
#include "core/session/agent_session.h"
#include "core/session/session_store.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

TEST(Sandbox, Modes) {
  EXPECT_EQ(pi::core::sandbox_mode_from_string("auto"),
            pi::core::SandboxMode::auto_mode);
  EXPECT_EQ(pi::core::sandbox_mode_from_string("required"),
            pi::core::SandboxMode::required);
  EXPECT_EQ(pi::core::sandbox_mode_from_string("off"),
            pi::core::SandboxMode::disabled);
  EXPECT_FALSE(pi::core::sandbox_mode_from_string("unknown"));
  EXPECT_EQ(pi::core::sandbox_mode_to_string(pi::core::SandboxMode::disabled),
            "disabled");
  pi::core::SandboxLauncher::validate(pi::core::SandboxMode::disabled);
}

TEST(Sandbox, SessionRoundTrip) {
  const auto root =
      std::filesystem::temp_directory_path() / "pici-sandbox-test";
  std::filesystem::remove_all(root);

  auto store = std::make_shared<pi::core::SessionStore>(root);
  auto policy = std::make_shared<pi::core::SandboxPolicy>(
      pi::core::SandboxMode::disabled);
  pi::core::SessionRuntime session(
      {.session_store = store, .sandbox_policy = policy});
  pi::core::SessionHeader header{.id = "sandbox-session",
                                 .model = "test",
                                 .provider = "test",
                                 .sandbox_mode = "disabled"};
  const auto id = session.create_session(header);
  EXPECT_EQ(id, "sandbox-session");

  auto loaded = session.load_session(id);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->header.sandbox_mode, "disabled");

  session.set_sandbox_mode(pi::core::SandboxMode::required);
  EXPECT_EQ(session.sandbox_mode(), pi::core::SandboxMode::required);
  session.activate_session(*loaded);
  EXPECT_EQ(session.sandbox_mode(), pi::core::SandboxMode::disabled);

  std::filesystem::remove_all(root);
}
