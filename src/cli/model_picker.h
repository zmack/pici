#pragma once

// Native terminal ModelPicker (plans/object-taxonomy-migration.md Phase 9):
// a bounded operation over an immutable core::ModelCatalogView projection.
// It never touches core::AgentState, core::SessionStore,
// pi::auth::Authentication, or core::ModelCatalog/core::PiciProcess
// directly -- the caller resolves and commits the selected core::ModelKey
// through the session contract (core::SessionRuntime::resolve_model()/
// set_model()), and supplies any per-candidate availability label,
// default-model tracking, and catalog refresh as plain values/opaque
// callbacks. Renamed from ModelSelector; a future Lua picker adapter can
// convert the same ModelPickerInput/ModelPickerResult pair to/from Lua
// values without reaching into the catalog or session runtime itself.

#include "core/models.h"

#include <cstddef>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace pi::cli {

struct ModelPickerInput {
  core::ModelCatalogView catalog_view;
  core::ModelKey current;
  // Optional per-candidate display label (e.g. "configured"/"missing"); the
  // picker only calls this to render a row, never to resolve credentials.
  std::function<std::string(const core::ModelKey &)> availability;
  // Optional: sorted near the top and marked distinctly from the cursor
  // highlight, mirroring pi's ModelSelectorComponent. Loading/saving this
  // value is the caller's concern (see cli/model_picker_settings.h); the
  // picker only ever sees the resolved key.
  std::optional<core::ModelKey> default_model;
  // Optional: if set, the picker kicks off a background refresh once at
  // startup and re-renders with the returned view when it completes (or
  // reports its error). Runs on a detached thread the picker never joins --
  // see run_model_picker()'s comment on why. `stop_token` is cancelled if
  // the picker closes before the refresh finishes.
  std::function<core::ModelCatalogView(std::stop_token)> refresh_catalog;
  // Optional: called with the currently highlighted candidate when the user
  // presses Ctrl+S. The picker then behaves exactly as if Enter had been
  // pressed (see ModelPickerResult::saved_as_default).
  std::function<void(const core::ModelKey &)> save_as_default;
};

struct ModelPickerResult {
  bool cancelled{true};
  std::optional<core::ModelKey> selected;
  // True when `selected` was chosen via Ctrl+S (save-as-default) rather
  // than Enter. `save_as_default` has already been invoked by the time this
  // returns.
  bool saved_as_default{false};
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
// the next cursor position, wrapping top<->bottom (a no-op when count is 0).
// run_model_picker()'s terminal loop is a thin driver over this plus its own
// private key reader.
enum class ModelPickerKey { up, down, enter, escape, other };

std::size_t reduce_model_picker_cursor(std::size_t cursor, ModelPickerKey key,
                                       std::size_t count);

// Decoded meaning of one raw input byte that isn't part of an arrow-key
// escape sequence (those still need read_key()'s own lookahead). Split out
// from read_key() so search-box text editing (printable chars, backspace)
// and Ctrl+S are unit-testable without a TTY, the same way
// reduce_model_picker_cursor() is -- see test_model_picker.cpp.
struct ModelPickerKeyEvent {
  ModelPickerKey nav{ModelPickerKey::other};
  char text_char{0}; // non-zero only when this byte should append to the
                     // search query
  bool is_backspace{false};
  bool is_ctrl_s{false};
};

ModelPickerKeyEvent decode_plain_byte(unsigned char c);

// Case-insensitive subsequence match: every character of `query` must
// appear in `haystack` in order, not necessarily contiguously. An empty
// query matches everything.
bool fuzzy_match(std::string_view haystack, std::string_view query);

// Reorders `entries` for display: the current model first, then the default
// model (if set and different from current), then the remaining entries in
// their existing (catalog) order. Does not filter anything out.
std::vector<core::ModelCatalogEntry>
sort_entries_for_display(std::vector<core::ModelCatalogEntry> entries,
                         const core::ModelKey &current,
                         const std::optional<core::ModelKey> &default_model);

} // namespace pi::cli
