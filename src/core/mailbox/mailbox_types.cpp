#include "core/mailbox/mailbox_types.h"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace pi::core {

std::string_view mailbox_error_code_to_string(MailboxErrorCode code) {
  switch (code) {
  case MailboxErrorCode::not_found:
    return "not_found";
  case MailboxErrorCode::permission_denied:
    return "permission_denied";
  case MailboxErrorCode::ambiguous_target:
    return "ambiguous_target";
  case MailboxErrorCode::busy:
    return "busy";
  case MailboxErrorCode::invalid_message:
    return "invalid_message";
  case MailboxErrorCode::invalid_claim:
    return "invalid_claim";
  case MailboxErrorCode::incompatible_schema:
    return "incompatible_schema";
  case MailboxErrorCode::corrupt:
    return "corrupt";
  case MailboxErrorCode::internal:
    return "internal";
  }
  return "internal";
}

MailboxError::MailboxError(MailboxErrorCode code, std::string_view message)
    : std::runtime_error(std::string(message)), code_(code) {}

std::string_view mailbox_message_kind_to_string(MailboxMessageKind kind) {
  switch (kind) {
  case MailboxMessageKind::steer:
    return "steer";
  case MailboxMessageKind::note:
    return "note";
  case MailboxMessageKind::request:
    return "request";
  case MailboxMessageKind::reply:
    return "reply";
  }
  return "note";
}

std::optional<MailboxMessageKind>
mailbox_message_kind_from_string(std::string_view value) {
  if (value == "steer")
    return MailboxMessageKind::steer;
  if (value == "note")
    return MailboxMessageKind::note;
  if (value == "request")
    return MailboxMessageKind::request;
  if (value == "reply")
    return MailboxMessageKind::reply;
  return std::nullopt;
}

} // namespace pi::core
