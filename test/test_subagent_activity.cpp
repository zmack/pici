#include "core/subagent_activity.h"

#include <gtest/gtest.h>

#include <string>

TEST(SubagentActivity, TracksChildLifecycle) {
  using namespace pi::core;

  SubagentActivityBridge activity;
  activity.observe(AgentTaskSpawnedEvent{.id = "child-1",
                                         .task_path = "/root/research",
                                         .parent_id = std::string{"root"},
                                         .task_name = "research"});

  auto rows = activity.pane_rows();
  ASSERT_EQ(rows.size(), std::size_t{1});
  EXPECT_EQ(rows[0].name, "research");
  EXPECT_EQ(rows[0].status, "pending_init");
  EXPECT_EQ(rows[0].last_activity, "→ spawned research");
  EXPECT_EQ(activity.summary(), "1 running, 0 idle");

  activity.observe(
      AgentTaskStatusChangedEvent{.id = "child-1",
                                  .previous = AgentTaskStatusKind::pending_init,
                                  .current = AgentTaskStatusKind::running});
  rows = activity.pane_rows();
  ASSERT_EQ(rows.size(), std::size_t{1});
  EXPECT_EQ(rows[0].status, "running");
  // A non-terminal status update need not create a noisy activity line, but
  // the pane status must still be updated.
  EXPECT_EQ(rows[0].last_activity, "→ spawned research");

  activity.observe(AgentTaskClosedEvent{.id = "child-1"});
  EXPECT_TRUE(activity.pane_rows().empty());
  EXPECT_TRUE(activity.summary().empty());
  EXPECT_EQ(activity.recent("child-1").size(), std::size_t{0});
}
