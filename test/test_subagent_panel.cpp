#include "core/subagent_panel.h"

#include <gtest/gtest.h>

#include <string>

class FakeRenderer final : public pi::core::Renderer {
public:
  explicit FakeRenderer(bool owns) : owns_(owns) {}
  void on_text_delta(std::string_view) override {}
  bool owns_subagent_pane() const override { return owns_; }
  void set_subagent_pane(
      const std::vector<pi::core::SubagentPaneRow> &rows) override {
    ++calls;
    last_rows = rows;
  }
  bool owns_;
  int calls{0};
  std::vector<pi::core::SubagentPaneRow> last_rows;
};

TEST(SubagentPanel, UpdatesOwnedRenderer) {
  pi::core::SubagentActivityBridge bridge;
  FakeRenderer renderer(true);
  pi::core::SubagentPanel panel(bridge);
  panel.mount(renderer);
  bridge.observe(pi::core::AgentTaskSpawnedEvent{.id = "child",
                                                 .task_path = "/child",
                                                 .parent_id = std::nullopt,
                                                 .task_name = "research"});
  ASSERT_EQ(renderer.calls, 1);
  ASSERT_EQ(renderer.last_rows.size(), std::size_t{1});
  EXPECT_EQ(renderer.last_rows[0].name, "research");
  panel.unmount();
}

TEST(SubagentPanel, UnmountStopsUpdates) {
  pi::core::SubagentActivityBridge bridge;
  FakeRenderer renderer(true);
  pi::core::SubagentPanel panel(bridge);
  panel.mount(renderer);
  bridge.observe(pi::core::AgentTaskSpawnedEvent{.id = "child",
                                                 .task_path = "/child",
                                                 .parent_id = std::nullopt,
                                                 .task_name = "research"});
  ASSERT_EQ(renderer.calls, 1);
  panel.unmount();
  bridge.observe(pi::core::AgentTaskSpawnedEvent{.id = "child2",
                                                 .task_path = "/child2",
                                                 .parent_id = std::nullopt,
                                                 .task_name = "other"});
  EXPECT_EQ(renderer.calls, 1);
}

TEST(SubagentPanel, IgnoresRendererWithoutPane) {
  pi::core::SubagentActivityBridge bridge;
  FakeRenderer unsupported(false);
  pi::core::SubagentPanel no_panel(bridge);
  no_panel.mount(unsupported);
  bridge.observe(pi::core::AgentTaskSpawnedEvent{.id = "child3",
                                                 .task_path = "/child3",
                                                 .parent_id = std::nullopt,
                                                 .task_name = "ignored"});
  EXPECT_EQ(unsupported.calls, 0);
}
