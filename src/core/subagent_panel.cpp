#include "core/subagent_panel.h"

namespace pi::core {
void SubagentPanel::mount(Renderer &renderer) {
  if (!renderer.owns_subagent_pane()) return;
  bridge_.set_pane_wake([this, &renderer] {
    renderer.set_subagent_pane(bridge_.pane_rows());
  });
}
void SubagentPanel::unmount() { bridge_.set_pane_wake({}); }
SubagentPanel::~SubagentPanel() { unmount(); }
} // namespace pi::core
