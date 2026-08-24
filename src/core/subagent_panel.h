#pragma once

#include "core/subagent_activity.h"

namespace pi::core {
class SubagentPanel {
public:
  explicit SubagentPanel(SubagentActivityBridge &bridge) : bridge_(bridge) {}
  SubagentPanel(const SubagentPanel &) = delete;
  SubagentPanel &operator=(const SubagentPanel &) = delete;
  void mount(Renderer &renderer);
  void unmount();
  ~SubagentPanel();
private:
  SubagentActivityBridge &bridge_;
};
} // namespace pi::core
