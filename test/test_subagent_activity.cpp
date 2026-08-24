#include "core/subagent_activity.h"

#include <iostream>
#include <string>
#include <string_view>

namespace tests {
int passed = 0;
int failed = 0;

void check(bool condition, std::string_view expression, int line) {
  if (condition)
    ++passed;
  else {
    ++failed;
    std::cout << "FAIL line " << line << " — " << expression << "\n";
  }
}
} // namespace tests

#define CHECK(expr) tests::check((expr), #expr, __LINE__)
#define CHECK_EQ(a, b) tests::check((a) == (b), #a " == " #b, __LINE__)

int main() {
  using namespace pi::core;

  SubagentActivityBridge activity;
  activity.observe(AgentTaskSpawnedEvent{.id = "child-1",
                                         .task_path = "/root/research",
                                         .parent_id = std::string{"root"},
                                         .task_name = "research"});

  auto rows = activity.pane_rows();
  CHECK_EQ(rows.size(), std::size_t{1});
  CHECK_EQ(rows[0].name, std::string{"research"});
  CHECK_EQ(rows[0].status, std::string{"pending_init"});
  CHECK_EQ(rows[0].last_activity, std::string{"→ spawned research"});
  CHECK_EQ(activity.summary(), std::string{"1 running, 0 idle"});

  activity.observe(AgentTaskStatusChangedEvent{
      .id = "child-1",
      .previous = AgentTaskStatusKind::pending_init,
      .current = AgentTaskStatusKind::running});
  rows = activity.pane_rows();
  CHECK_EQ(rows[0].status, std::string{"running"});
  // A non-terminal status update need not create a noisy activity line, but
  // the pane status must still be updated.
  CHECK_EQ(rows[0].last_activity, std::string{"→ spawned research"});

  activity.observe(AgentTaskClosedEvent{.id = "child-1"});
  CHECK(activity.pane_rows().empty());
  CHECK(activity.summary().empty());
  CHECK_EQ(activity.recent("child-1").size(), std::size_t{0});

  std::cout << tests::passed << " passed, " << tests::failed << " failed\n";
  return tests::failed == 0 ? 0 : 1;
}
