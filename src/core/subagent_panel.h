#pragma once

#include "core/subagent_activity.h"

#include <memory>
#include <mutex>

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
  struct CallbackState {
    std::mutex mutex;
    bool mounted{false};
  };

  SubagentActivityBridge &bridge_;
  std::shared_ptr<CallbackState> callback_state_;
};
} // namespace pi::core
