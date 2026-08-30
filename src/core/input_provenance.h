#pragma once

#include <optional>
#include <string>

namespace pi::core {

// Runtime-owned: where an agent input came from (mailbox, CLI, RPC, ACP),
// for routing/audit -- not presentation. Phase 7 of
// plans/session-runtime-migration.md folded this out of the combined
// RequestPresentation type (which conflated this with rendering) and out of
// AgentInput's separate InputSource field (which duplicated `source` here
// with a narrower ordinary/mailbox-only value set). AgentInput carries
// exactly one InputProvenance now; a renderer that needs to display this
// wraps it in its own frontend-owned type (see stream_renderer.h's
// RendererRequest) rather than this type growing presentation fields.
struct InputProvenance {
  enum class Source { ordinary, mailbox, follow_up };
  Source source{Source::ordinary};
  std::optional<std::string> message_id;
  std::optional<std::string> message_kind;
  std::optional<std::string> sender_agent_id;
  std::optional<std::string> sender_session_id;
  std::optional<std::string> sender_task_path;
  std::optional<std::string> sender_session_name;
};

} // namespace pi::core
