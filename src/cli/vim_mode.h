#pragma once

#include <cstddef>
#include <string>

namespace pi::cli {

// Normal/Insert modal-editing state for the composer's optional vim mode
// (config.toml's [input] vim_mode = true; see
// plans/composer-textarea-rewrite.md, M5). Deliberately cut down from full
// Vim: motions h j k l w b e 0 $, operators d/c only (plus the doubled dd/cc
// whole-line convention) -- no text objects, no y/registers beyond the
// implicit unnamed one (shared with readline()'s own kill buffer), no
// visual/command-line mode.
enum class VimMode { Normal, Insert };

// Intercepts single-byte Normal-mode key input ahead of readline()'s plain
// key-dispatch chain (see readline.cpp) when vim mode is enabled and the
// engine's current mode is Normal. Every other key path -- Enter/submit,
// Ctrl+D/EOF, and all multi-byte escape sequences (arrows, Ctrl+Left/Right,
// mouse wheel, Alt+B/F, Alt+Enter, bracketed paste, the Kitty keyboard
// protocol) -- is left completely untouched by this class and keeps working
// exactly as it does without vim mode; see the comment at readline.cpp's
// vim-mode hook point for why those are deliberately out of scope here.
class VimEngine {
public:
  VimMode mode() const { return mode_; }
  void set_mode(VimMode mode) { mode_ = mode; }

  // Cancels any in-progress d/c operator without applying it. Called
  // whenever an Escape byte arrives (bare or as the start of some other
  // escape sequence), matching real Vim's "Escape always cancels a pending
  // operator" behavior.
  void cancel_pending() { pending_ = PendingOp::none; }

  struct Result {
    // True if buf, cursor, or mode changed and the caller should redraw.
    // False for a harmless no-op: an uncovered key, or a key that only
    // started/continued pending-operator state with no visible effect yet.
    bool changed{false};
  };

  // Processes one raw byte while in Normal mode, mutating buf/cursor (and
  // this engine's own mode/pending-operator state) in place. `kill_buffer`
  // is readline()'s own most-recent-kill slot (see readline.cpp), reused
  // here as vim mode's implicit unnamed register for d/c, so Ctrl+Y in
  // Insert mode can yank back a vim-mode deletion and vice versa.
  Result handle_normal_key(unsigned char c, std::string &buf,
                           std::size_t &cursor, std::string &kill_buffer);

private:
  enum class PendingOp { none, delete_op, change_op };

  static void apply_dd(std::string &buf, std::size_t &cursor,
                       std::string &kill_buffer);
  static void apply_cc(std::string &buf, std::size_t &cursor,
                       std::string &kill_buffer);

  VimMode mode_{VimMode::Insert};
  PendingOp pending_{PendingOp::none};
};

} // namespace pi::cli
