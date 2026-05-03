#include "core/providers/faux.h"

#include <chrono>
#include <memory>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "core/agent_state.h"
#include "core/event_types.h"
#include "core/llm_client.h"
#include "core/message_types.h"

namespace pi::core {

FauxClient::FauxClient(std::vector<Script> scripts)
    : scripts_(std::move(scripts)), call_count_(0) {}

std::shared_ptr<AssistantMessage>
FauxClient::stream(const Model &model, const AgentContext &context,
                   const StreamOptions &options,
                   AssistantEventCallback on_event, std::stop_token stop_tok) {
  (void)context;
  (void)options;

  auto idx = call_count_.fetch_add(1);

  auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now().time_since_epoch())
                    .count();

  if (idx >= scripts_.size()) {
    auto msg = std::make_shared<AssistantMessage>();
    msg->api = "faux";
    msg->provider = "faux";
    msg->model = model.id;
    msg->stop_reason = StopReason::error;
    msg->error_message = "No more faux scripts queued";
    msg->timestamp = now_ms;
    if (on_event) {
      on_event(AssistantMessageErrorEvent{.reason = StopReason::error,
                                          .error = *msg});
    }
    return msg;
  }

  const auto &script = scripts_[idx];
  std::shared_ptr<AssistantMessage> final_msg;

  for (const auto &ev : script.events) {
    if (stop_tok.stop_requested())
      break;

    if (on_event)
      on_event(ev);

    std::visit(
        [&final_msg](const auto &e) {
          using T = std::decay_t<decltype(e)>;
          if constexpr (std::is_same_v<T, AssistantMessageDoneEvent>) {
            final_msg = std::make_shared<AssistantMessage>(e.message);
          } else if constexpr (std::is_same_v<T, AssistantMessageErrorEvent>) {
            final_msg = std::make_shared<AssistantMessage>(e.error);
          }
        },
        ev);

    if (script.delay_between && !stop_tok.stop_requested()) {
      std::this_thread::sleep_for(*script.delay_between);
    }
  }

  if (!final_msg) {
    final_msg = std::make_shared<AssistantMessage>();
    final_msg->api = "faux";
    final_msg->provider = "faux";
    final_msg->model = model.id;
    final_msg->stop_reason = StopReason::error;
    final_msg->error_message = "Faux script had no terminal event";
    final_msg->timestamp = now_ms;
  }

  return final_msg;
}

} // namespace pi::core
