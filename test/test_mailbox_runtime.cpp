#include "cli/mailbox_runtime.h"

#include "cli/config.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <memory>
#include <source_location>
#include <string>
#include <string_view>

namespace tests {
int passed{0};
int failed{0};

bool check(bool condition, std::string_view expression,
           std::source_location location = std::source_location::current()) {
  if (condition) {
    ++passed;
    return true;
  }
  ++failed;
  std::cout << "FAIL " << location.file_name() << ":" << location.line()
            << " — " << expression << "\n";
  return false;
}
} // namespace tests

#define CHECK(expression) tests::check((expression), #expression)
#define CHECK_EQ(left, right)                                                  \
  tests::check((left) == (right), #left " == " #right)

int main() {
  using namespace pi;

  const auto workspace =
      std::filesystem::temp_directory_path() / "pici-mailbox-runtime-workspace";
  auto config = std::make_shared<cli::Config>();
  config->mailbox.path = "/config/mailbox.sqlite3";
  config->mailbox.scope = "global";
  config->mailbox.heartbeat_interval_ms = 1234;
  config->mailbox.stale_after_ms = 5678;
  config->mailbox.poll_interval_ms = 90;
  config->mailbox.claim_lease_ms = 4321;
  config->mailbox.retention_days = 12;

  cli::Args args;
  args.config_document = config;
  args.mailbox_path = "/cli/mailbox.sqlite3";
  core::Model model;
  model.provider = "provider-a";
  model.id = "model-a";

  const auto launch =
      cli::resolve_mailbox_launch_options(args, model, workspace);
  CHECK_EQ(launch.resolved_path, std::filesystem::path("/cli/mailbox.sqlite3"));
  CHECK_EQ(launch.coordinator.store.path, launch.resolved_path);
  CHECK_EQ(launch.coordinator.store.workspace_path,
           workspace.lexically_normal().string());
  CHECK(launch.coordinator.store.scope == core::MailboxScope::global);
  CHECK_EQ(launch.coordinator.store.claim_lease_ms, std::int64_t{4321});
  CHECK_EQ(launch.coordinator.store.retention_days, std::int64_t{12});
  CHECK_EQ(launch.coordinator.heartbeat_interval,
           std::chrono::milliseconds(1234));
  CHECK_EQ(launch.coordinator.stale_after, std::chrono::milliseconds(5678));
  CHECK_EQ(launch.coordinator.poll_interval, std::chrono::milliseconds(90));
  CHECK_EQ(launch.coordinator.provider, std::string("provider-a"));
  CHECK_EQ(launch.coordinator.model_id, std::string("model-a"));
  CHECK(!launch.coordinator.process_id.empty());
  CHECK(!launch.coordinator.root_agent_id.empty());
  CHECK(!launch.coordinator.store.workspace_id.empty());

  cli::MailboxRuntime disabled;
  CHECK(!disabled.enabled());
  CHECK(!disabled.task_event_callback());
  CHECK(disabled.claim_idle_root_turn().empty());

  std::cout << "passed: " << tests::passed << ", failed: " << tests::failed
            << "\n";
  return tests::failed == 0 ? 0 : 1;
}
