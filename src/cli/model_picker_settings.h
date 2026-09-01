#pragma once

// Small persisted store for the model picker's own preferences (currently
// just the default model set via Ctrl+S) -- not part of config.toml (which
// pici only ever reads, never writes back to; see cli/config.cpp) and not
// part of core::auth::CredentialStore (which is for OAuth/API-key secrets,
// and takes on real concurrent-writer locking this file deliberately
// doesn't need: it is only ever written from one interactive picker
// session, never from a separate process).

#include "core/models.h"

#include <filesystem>
#include <optional>

namespace pi::cli {

// Resolution order: $PICI_PICKER_SETTINGS_FILE, then
// $XDG_CONFIG_HOME/pici/picker_settings.json, then
// $HOME/.config/pici/picker_settings.json. Throws std::runtime_error if none
// of those can be determined (mirrors
// core::auth::default_auth_file_path()'s contract).
std::filesystem::path default_picker_settings_path();

// Returns std::nullopt if the file is missing, unreadable, or not valid
// JSON in the expected shape -- never throws on read.
std::optional<core::ModelKey> load_default_model();

// Creates the parent directory if needed and overwrites the file. Throws
// std::runtime_error if the write fails.
void save_default_model(const core::ModelKey &key);

} // namespace pi::cli
