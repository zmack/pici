#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace pi::cli {

// UTF-8 helpers shared between InputRenderer's terminal output and the
// word-wrap planner below, so column bookkeeping in both places always
// agrees on how wide a given codepoint is.
std::size_t utf8_length(std::string_view text, std::size_t offset);
std::uint32_t utf8_codepoint(std::string_view text, std::size_t offset,
                             std::size_t length);
int codepoint_width(std::uint32_t codepoint);

// Length of the ANSI CSI escape sequence starting at `offset` (0 if there
// isn't one). Escape sequences are zero-width: they never consume columns
// and never become word-wrap break candidates.
std::size_t ansi_escape_length(std::string_view text, std::size_t offset);

// True if `c` is treated as inter-word whitespace for wrapping purposes.
// '\n' and '\r' are hard control characters handled by the caller (a row
// break and a column reset, respectively) and never reach this
// classification.
bool is_wrap_space(char c);

// One word-wrap break within a run of text that has no embedded '\n'
// (callers segment on hard newlines themselves before calling
// plan_word_wrap -- see its docs below). `content_end` is the offset up to
// which the row being ended actually paints content; `resume_offset` is
// the offset where the next row's content begins. The two differ only when
// trailing whitespace at the break is dropped rather than carried to the
// new row (standard word-wrap convention: `[content_end, resume_offset)`
// is whitespace that is never painted). When resume_offset == content_end,
// the break is a hard character-boundary wrap in the middle of a single
// token wider than the whole row (e.g. a long URL) -- the fallback for
// when word-boundary wrapping alone can't keep the token intact.
struct WordWrapBreak {
  std::size_t content_end{0};
  std::size_t resume_offset{0};
};

// Plans word-boundary wrapping for a single logical line of `text` (no
// embedded '\n') into rows of `columns` display columns, given that the
// row already sits at `start_column` when `text` begins painting (nonzero
// when a prompt painted part of the row first). Every row after the first
// soft break starts at column 0 -- continuation rows are not hang-indented
// under the prompt (see composer-textarea-rewrite.md, M2).
//
// Greedy, whitespace-delimited word wrap: words accumulate on a row until
// the next word wouldn't fit, then the row breaks at the whitespace before
// that word (dropped, not carried over). A single word wider than the full
// row width falls back to hard character-boundary wrapping for that word
// only, then resumes word-wrapping immediately after it. ANSI escape
// sequences are treated as zero-width throughout and never become break
// points themselves.
//
// Returns the ordered list of breaks encountered while laying out `text`.
// An empty result means `text` fits entirely on the starting row. Offsets
// in the result are relative to `text`.
std::vector<WordWrapBreak> plan_word_wrap(std::string_view text,
                                          std::size_t columns,
                                          std::size_t start_column);

} // namespace pi::cli
