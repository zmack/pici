#include "core/subagent_panel.h"

namespace pi::core {
void SubagentPanel::mount(Renderer &renderer) {
  unmount();
  if (!renderer.owns_subagent_pane()) return;
  auto state = std::make_shared<CallbackState>();
  state->mounted = true;
  callback_state_ = state;
  bridge_.set_pane_wake([state, &bridge = bridge_, &renderer] {
    std::scoped_lock lock(state->mutex);
    if (!state->mounted)
      return;
    renderer.set_subagent_pane(bridge.pane_rows());
  });
}
void SubagentPanel::unmount() {
  if (callback_state_) {
    std::scoped_lock lock(callback_state_->mutex);
    callback_state_->mounted = false;
  }
  bridge_.set_pane_wake({});
}
SubagentPanel::~SubagentPanel() { unmount(); }
} // namespace pi::core
