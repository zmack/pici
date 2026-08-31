#pragma once

// Native terminal ModelPicker (plans/object-taxonomy-migration.md Phase 9):
// a bounded operation over an immutable core::ModelCatalogView projection.
// It never touches core::AgentState, core::SessionStore, or
// pi::auth::Authentication directly -- the caller resolves and commits the
// selected core::ModelKey through the session contract
// (core::SessionRuntime::resolve_model()/set_model()), and supplies any
// per-candidate availability label as a plain projection function. Renamed
// from ModelSelector; a future Lua picker adapter can convert the same
// ModelPickerInput/ModelPickerResult pair to/from Lua values without
// reaching into the catalog or session runtime itself.

#include "core/models.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <string>

namespace pi::cli {

struct ModelPickerInput {
  core::ModelCatalogView catalog_view;
  core::ModelKey current;
  // Optional per-candidate display label (e.g. "configured"/"missing"); the
  // picker only calls this to render a row, never to resolve credentials.
  std::function<std::string(const core::ModelKey &)> availability;
};

struct ModelPickerResult {
  bool cancelled{true};
  std::optional<core::ModelKey> selected;
};

// `caller_owns_alt_screen` must be true when the active renderer already
// holds a persistent alternate-screen session (e.g. --render region); see
// tree_selector.h for why nesting a second `1049h`/`1049l` pair corrupts the
// display in that case. The caller must repaint (e.g. via
// Renderer::force_full_repaint()) once this returns.
ModelPickerResult run_model_picker(const ModelPickerInput &input,
                                   bool caller_owns_alt_screen = false);

// Pure navigation state machine, exposed for unit testing without a TTY:
// given the current cursor position, a key, and the candidate count, returns
// the next cursor position (a no-op when count is 0). run_model_picker()'s
// terminal loop is a thin driver over this plus its own private key reader.
enum class ModelPickerKey { up, down, enter, escape, other };

std::size_t reduce_model_picker_cursor(std::size_t cursor, ModelPickerKey key,
                                       std::size_t count);

} // namespace pi::cli
