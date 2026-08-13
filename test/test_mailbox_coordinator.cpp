#include "core/mailbox/mailbox_coordinator.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <algorithm>
#include <source_location>
#include <stdexcept>
#include <string_view>
#include <vector>

#include <unistd.h>

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
#define CHECK_EQ(left, right) tests::check((left) == (right), #left " == " #right)

using namespace pi::core;

int main() {
  const auto suffix =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto root = std::filesystem::temp_directory_path() /
                    ("pici-coordinator-" + std::to_string(::getpid()) +
                     "-" + std::to_string(suffix));
  std::filesystem::create_directories(root);
  TimestampMs now = 1'000;
  const auto options = [&] {
    MailboxCoordinatorOptions result;
    result.store.path = root / "mailbox.sqlite3";
    result.store.workspace_id = "workspace";
    result.store.workspace_path = root.string();
    result.store.claim_lease_ms = 100;
    result.store.clock = [&] { return now; };
    result.store.id_generator = [&] { return "generated"; };
    result.process_id = "process-1";
    result.root_agent_id = "root-agent";
    result.provider = "test";
    result.model_id = "model-a";
    result.heartbeat_interval = std::chrono::hours(1);
    result.cleanup_interval = std::chrono::hours(2);
    return result;
  };

  try {
    bool observed = false;
    auto callbacks = fan_out_agent_task_callbacks({
        [](const AgentTaskEvent &) { throw std::runtime_error("observer"); },
        [&](const AgentTaskEvent &) { observed = true; },
    });
    callbacks(AgentTaskClosedEvent{.id = "ignored"});
    CHECK(observed);

    MailboxCoordinator coordinator(options());
    CHECK(std::filesystem::exists(root / "mailbox.sqlite3"));
    coordinator.activate_root("session-a", "first");
    CHECK(coordinator.status().root_active);
    CHECK_EQ(coordinator.status().session_id.value(), std::string("session-a"));
    coordinator.set_root_running(true);
    coordinator.set_model("test", "model-b");
    CHECK(coordinator.status().root_running);

    coordinator.observe_task_event(AgentTaskSpawnedEvent{
        .id = "agent_1", .task_path = "/root/child", .parent_id = "root",
        .task_name = "child"});
    coordinator.observe_task_event(AgentTaskStatusChangedEvent{
        .id = "agent_1",
        .previous = AgentTaskStatusKind::pending_init,
        .current = AgentTaskStatusKind::completed});
    auto children = coordinator.store().list_agents(AgentQuery{
        .session_id = "session-a", .include_closed = true, .now_ms = now});
    CHECK_EQ(children.size(), std::size_t{2});
    CHECK(std::ranges::any_of(children, [](const auto &child) {
      return child.agent_id == "agent_1" && child.status == "completed";
    }));
    coordinator.observe_task_event(AgentTaskClosedEvent{.id = "agent_1"});
    CHECK(coordinator.store()
              .list_agents(AgentQuery{.agent_id = "agent_1",
                                      .include_closed = true,
                                      .now_ms = now})
              .front()
              .closed_at_ms.has_value());

    coordinator.activate_root("session-b", "second");
    CHECK_EQ(coordinator.status().session_id.value(), std::string("session-b"));
    CHECK_EQ(coordinator.store()
                 .list_agents(AgentQuery{.include_closed = true, .now_ms = now})
                 .size(),
                 std::size_t{3});

    coordinator.maintenance_tick();
    now += 100;
    coordinator.set_root_running(false);
    coordinator.stop();
    CHECK(!coordinator.status().root_active);
  } catch (const std::exception &error) {
    std::cout << "unexpected exception: " << error.what() << "\n";
    ++tests::failed;
  }

  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::cout << "Tests: " << tests::passed << " passed, " << tests::failed
            << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
