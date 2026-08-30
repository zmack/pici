#pragma once

// Thin CLI-side adapter: translates cli::Args/config.toml into
// core::MailboxLaunchOptions. This is the one piece of the former
// cli::MailboxRuntime that genuinely needs to stay here -- reading
// cli::Args is a frontend concern. The runtime type itself
// (core::MailboxRuntime) and the plain construction helper
// (core::start_mailbox) have no Args dependency and moved to
// core/session/mailbox_runtime.h in plans/session-runtime-migration.md
// Phase 3.

#include "cli/args.h"
#include "core/models.h"
#include "core/session/mailbox_runtime.h"

#include <filesystem>

namespace pi::cli {

core::MailboxLaunchOptions
resolve_mailbox_launch_options(const Args &args, const core::Model &model,
                               std::filesystem::path workspace_path);

} // namespace pi::cli
