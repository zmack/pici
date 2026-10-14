#include "core/child_completion.h"

#include "core/agent_loop.h"
#include "core/input_provenance.h"
#include "core/message_types.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pi::core {

namespace {

constexpr std::string_view kHeader = "[pici child task completion]\n";

AgentInput text_input(std::string text, InputProvenance provenance) {
  UserMessage user;
  user.content.emplace_back(TextContent{.text = std::move(text)});
  return AgentInput{.message = Message{std::move(user)},
                    .presentation = std::move(provenance)};
}

} // namespace

std::string_view
child_completion_status_to_string(ChildCompletionStatus status) {
  switch (status) {
  case ChildCompletionStatus::completed:
    return "completed";
  case ChildCompletionStatus::errored:
    return "errored";
  case ChildCompletionStatus::interrupted:
    return "interrupted";
  }
  return "completed";
}

bool truncate_utf8(std::string &text, std::size_t max_bytes) {
  if (text.size() <= max_bytes)
    return false;
  std::size_t cut = max_bytes;
  // Back up over continuation bytes so the cut lands on a code point start.
  while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U)
    --cut;
  text.resize(cut);
  return true;
}

AgentInput child_completion_input(const ChildCompletion &completion) {
  std::string text(kHeader);
  text += "task_id=" + completion.task_id + "\n";
  text += "task_path=" + completion.task_path + "\n";
  text += "task_name=" + completion.task_name + "\n";
  text += "run_generation=" + std::to_string(completion.run_generation) + "\n";
  text += "status=" +
          std::string(child_completion_status_to_string(completion.status)) +
          "\n";
  text += "stop_reason=" +
          std::string(stop_reason_to_string(completion.stop_reason)) + "\n";
  if (completion.error)
    text += "error=" + *completion.error + "\n";
  if (completion.truncated)
    text += "truncated=true (read the full result with agents_get)\n";
  text += "\n" + completion.text;
  return text_input(std::move(text),
                    {.source = InputProvenance::Source::child_completion,
                     .message_id = completion.task_id + "#" +
                                   std::to_string(completion.run_generation),
                     .message_kind = std::string(
                         child_completion_status_to_string(completion.status)),
                     .sender_agent_id = completion.task_id,
                     .sender_task_path = completion.task_path});
}

AgentInput child_completion_dropped_input(std::size_t dropped) {
  return text_input(std::string(kHeader) +
                        "dropped=" + std::to_string(dropped) + "\n\n" +
                        std::to_string(dropped) +
                        " completion(s) were dropped because the pending "
                        "queue was full; list the agents to inspect them.",
                    {.source = InputProvenance::Source::child_completion,
                     .message_kind = "dropped"});
}

ChildCompletionInbox::Admission
ChildCompletionInbox::admit(ChildCompletion completion) {
  if (closed_)
    return Admission::closed;
  if (cancelled_.contains(completion.task_id))
    return Admission::cancelled;
  Key key{completion.task_id, completion.run_generation};
  if (seen_.contains(key))
    return Admission::duplicate;
  if (pending_.size() >= kMaxPending) {
    ++dropped_;
    return Admission::overflow;
  }
  seen_order_.push_back(key);
  seen_.insert(std::move(key));
  if (seen_order_.size() > kMaxRemembered) {
    seen_.erase(seen_order_.front());
    seen_order_.pop_front();
  }
  pending_.push_back(std::move(completion));
  return Admission::admitted;
}

ChildCompletionInbox::Batch ChildCompletionInbox::drain() {
  Batch batch;
  batch.completions.assign(std::make_move_iterator(pending_.begin()),
                           std::make_move_iterator(pending_.end()));
  batch.dropped = std::exchange(dropped_, 0);
  pending_.clear();
  return batch;
}

std::size_t ChildCompletionInbox::cancel(const std::string &task_id) {
  const auto removed = std::erase_if(
      pending_, [&](const ChildCompletion &c) { return c.task_id == task_id; });
  if (cancelled_.insert(task_id).second) {
    cancelled_order_.push_back(task_id);
    if (cancelled_order_.size() > kMaxRemembered) {
      cancelled_.erase(cancelled_order_.front());
      cancelled_order_.pop_front();
    }
  }
  return removed;
}

void ChildCompletionInbox::clear() {
  pending_.clear();
  seen_order_.clear();
  seen_.clear();
  cancelled_order_.clear();
  cancelled_.clear();
  dropped_ = 0;
  closed_ = false;
}

} // namespace pi::core
