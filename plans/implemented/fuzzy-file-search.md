# Fuzzy file search for `find`

## Status

Implementation plan. Not started. Targets the current tree as of 2026-08-23
(`src/core/builtin_tools.cpp` `FindTool`/`GitIgnore`/glob helpers).

Revised after an Opus pre-implementation review found the original scorer
design broken for its own headline use case (multi-word queries didn't
match anything), an internally-inconsistent worked example, an unspecified
alignment strategy that a naive implementation would get wrong, and an
unbounded-work risk with no early exit. All are corrected below.

## Goal

Let the model locate files by approximate name — "the auth middleware file",
`buildtools cpp`, `usr auth` — without first having to construct a correct
glob or regex. This was flagged as a gap during a Codex-vs-pici feature
survey: Codex ships a dedicated fuzzy file-search tool
(`vendor/codex/codex-rs/file-search/`) built on the `ignore` crate (what
`ripgrep` uses) plus `nucleo-matcher` for fuzzy scoring. pici's `find` tool
(`builtin_tools.cpp:994-1067`) is glob-only: a miss means the model has to
guess a better pattern and retry, burning a turn.

This is scoped as a **small, self-contained addition to the existing `find`
tool**, not a new tool and not a port of codex's live-search session
machinery.

## Non-goals

- **No live/incremental search session.** Codex's `create_session()` +
  `FileSearchSession` + debounced `SessionReporter` machinery
  (`file-search/src/lib.rs:143-217`) exists to back an interactive TUI
  `@`-file-mention picker that re-queries as the user types. pici tool calls
  are one-shot request/response; there is no keystroke stream to debounce
  against. `run()` (`file-search/src/lib.rs:297-313`) — the synchronous
  one-shot wrapper codex itself uses for non-interactive callers — is the
  right shape to imitate, not the session API.
- **No `nucleo`/external fuzzy-matching dependency.** pici has no Rust-crate
  equivalent dependency story; this plan implements a small, self-contained
  subsequence fuzzy scorer in C++ instead of vendoring a matching library.
  This trades some ranking sophistication for zero new build dependencies,
  consistent with how pici already implements its own fuzzy *text* matcher
  (`normalize_for_fuzzy_match` et al.) rather than pulling one in.
- **No separate `file_search` tool.** Codex ships file-search as an
  independent crate/binary because its TUI and its agent tool have
  different callers. pici's model-facing surface has exactly one caller
  (the agent loop via a tool call), so a `mode` parameter on the existing
  `find` tool avoids growing the tool count and the system-prompt tool list
  for a capability that's a natural extension of "search for files."
- **No directory-vs-file match-type distinction** in v1 (codex's
  `MatchType::File`/`Directory`, `file-search/src/lib.rs:63-68`). `find`
  already only returns regular files (`builtin_tools.cpp:1032`); extending
  it to also match directories is an orthogonal change, not needed for the
  fuzzy-search gap this plan closes.

## Existing infrastructure this builds on

| Piece | Location | Reuse |
|-------|----------|-------|
| `FindTool` | `builtin_tools.cpp:994-1067` | Base tool gains a new optional parameter and code path; glob behavior is unchanged when the parameter is absent |
| `GitIgnore` | `builtin_tools.cpp:288-344` (`.load()`, `.ignored()`) | Same ignore-aware walk for the fuzzy path — no new ignore logic |
| Directory-skip helper | `should_skip_dir()`, `builtin_tools.cpp:282` | Reused verbatim in the fuzzy walk |
| Path-relativization | `relative_posix()`, `builtin_tools.cpp:410` | Reused verbatim for match paths |
| Recursive walk pattern | `std::filesystem::recursive_directory_iterator` loop in `FindTool::execute` | Same walk skeleton; only the per-entry test changes between glob and fuzzy modes |
| Result truncation | `truncate_head()`, `kMaxBytes` (`builtin_tools.cpp:47`) | Same output-size discipline as today's `find`/`grep` |
| Default result cap | `kFindDefaultLimit` (`builtin_tools.cpp:50`) | Glob mode keeps this default (`1000`); fuzzy mode gets its own smaller default (`kFuzzyFindDefaultLimit = 20`, see §3) since a ranked mode returning 1000 results defeats the ranking |

## Reference implementation studied

- `vendor/codex/codex-rs/file-search/src/lib.rs` — walker (`ignore` crate,
  `require_git(true)` gitignore scoping, `follow_links(true)`), scoring via
  `nucleo`, `cmp_by_score_desc_then_path_asc` tie-breaking (`lib.rs:326-339`
  — score descending, then path ascending). pici takes the **shape** of the
  synchronous `run()` entry point and the tie-break rule, not the threaded
  walker/matcher/channel architecture (`walker_worker`/`matcher_worker`,
  `lib.rs:417-615`), which exists solely to serve the live session API this
  plan explicitly excludes.
- `vendor/codex/codex-rs/file-search/README.md` — confirms the tool's scope
  ("fast fuzzy file search... honoring `.gitignore`") matches this plan's
  scope exactly, minus the TUI-picker use case.
- `git_repo_still_respects_local_gitignore_when_enabled` and
  `parent_gitignore_outside_repo_does_not_hide_repo_files` tests in
  `file-search/src/lib.rs:1075-1220` — codex's `require_git(true)` disables
  *all* gitignore processing (not just parent-directory rules) unless a
  `.git` directory is present, specifically to stop a broad parent
  `.gitignore` (e.g. `~/.gitignore` containing `*`) from silently suppressing
  an entire non-git-tracked search tree. This is a real correctness lesson
  worth checking pici's existing `GitIgnore` class against — a quick read of
  `builtin_tools.cpp:288-344` during Phase 0 shows it has **no `.git`
  presence check at all**, loads `.gitignore` from only `cwd` and `root`
  (not nested directories), and drops `!` negation patterns. This plan
  doesn't change `GitIgnore` itself (fuzzy mode inherits whatever behavior
  glob mode already has), but flags this as a pre-existing gap worth its own
  follow-up rather than an unknown (see Risks).

## Design

### 1. Tool schema change

`find`'s schema gains two optional properties; nothing required changes, so
every existing call (`{"pattern": "*.cpp"}`) behaves exactly as before:

```json
{"type":"object","properties":{
  "pattern":{"type":"string","description":"Glob pattern (e.g. '*.cpp'), or a fuzzy query when mode is 'fuzzy'"},
  "path":{"type":"string","description":"Directory to search in (default: current directory)"},
  "limit":{"type":"number","description":"Maximum number of results (default: 1000)"},
  "mode":{"type":"string","enum":["glob","fuzzy"],"description":"'glob' (default) matches pattern as a glob; 'fuzzy' ranks files by approximate name match, useful when you don't know the exact path or extension"}
 },"required":["pattern"],"additionalProperties":false}
```

Updated tool description: *"Search for files by glob pattern, or by fuzzy
name match when mode is 'fuzzy'. Returns matching file paths relative to
the search directory, ranked by relevance in fuzzy mode."*

### 2. New helper — `fuzzy_score_path()` in `builtin_tools.cpp`

A small subsequence scorer, in the same anonymous namespace as
`normalize_for_fuzzy_match`.

**Query tokenization (required for the plan's own headline use case).** The
Goal section's examples — `buildtools cpp`, `usr auth` — are
whitespace-separated queries where no single path contains a literal space.
A plain ordered-subsequence test over the whole query string would never
match anything for these inputs, silently defeating the feature. The query
is therefore split on whitespace into **atoms**; a candidate path must match
**every** atom (AND semantics, each atom scored independently as a
subsequence per §below), and the candidate's total score is the sum of
per-atom scores. This mirrors nucleo's `AtomKind::Fuzzy` multi-atom pattern
behavior, which is *why* codex's equivalent queries work
(`vendor/codex/codex-rs/file-search/src/lib.rs` uses `Nucleo`'s pattern
matching, which tokenizes this way by default).

```cpp
// Returns std::nullopt if any whitespace-separated atom of `query` fails to
// match `candidate` as an ordered, case-insensitive subsequence; otherwise
// a score where higher is better, summed across atoms. Each atom is scored
// independently by scoring_alignment() below.
std::optional<int> fuzzy_score_path(std::string_view candidate,
                                    std::string_view query);
```

**Alignment is not free to choose greedily.** A query atom generally has
multiple valid subsequence alignments against a candidate, and bonuses only
reward *some* of them — e.g. atom `at` against `src/agent_task.cpp` has a
greedy-leftmost alignment that binds `t` inside "agen**t**", never reaching
the `t` in "**t**ask", which would have scored a segment-boundary bonus.
Picking the alignment naively (leftmost or greedy) produces wrong,
non-reproducible-feeling rankings. This plan uses a small dynamic-program
per atom — `scoring_alignment(candidate, atom)` — over a
`|candidate| × |atom|` table, the same shape fzf/nucleo use internally: at
each `(i, j)` position, if `candidate[i]` matches `atom[j]`
case-insensitively, take the best of "match here" (previous best score
ending before `i`, plus this position's bonus, possibly plus a
consecutive-match bonus if `i-1, j-1` was also a match) vs. "skip
candidate[i]". This is `O(|candidate| · |atom|)` per atom, which is small
for file paths (a few hundred bytes at most) and requires no external
dependency — it is a bounded table-fill, not a general edit-distance
library.

Per-position bonus, applied when the DP chooses to match `candidate[i]`
against the atom:

- +2 if the match starts a path segment (previous char is `/` or
  start-of-string) or starts a "word" (previous char is `_`, `-`, `.`, or a
  lowercase→uppercase transition) — mirrors the intuition behind fzf/nucleo
  path-aware scoring without importing either.
- +1 if the match falls inside the final path segment (the basename) rather
  than a directory component — the model is usually thinking of a filename,
  not a directory name.
- +1 if this match is immediately adjacent (in `candidate`) to the previous
  matched character (rewards tight clusters — same spirit as `nucleo`'s gap
  bonus, expressed as a bonus for adjacency rather than a penalty for gaps,
  since every valid subsequence match already has the same fixed count of
  matched characters — see the note below on why a flat per-character base
  score would not discriminate between candidates at all).

There is no separate flat "base score" — a plain ordered-subsequence match
always matches exactly `|atom|` characters by definition (that's the
pass/fail condition), so a constant per-character base is the same for
every matching candidate and contributes nothing to relative ranking. All
differentiation comes from the bonuses above; the DP is maximizing *bonus
total*, not "score", to keep this explicit.

**Worked example** (replacing an earlier, incorrect example that referenced
a candidate string not actually containing the required characters — traced
through carefully this time): atom `atc` against candidate
`src/apply_the_change.cpp`. One valid alignment: `a` at the start of
`apply` (segment-boundary bonus), `t` at the start of `the` (segment-boundary
bonus), `c` at the start of `change` (segment-boundary bonus) — three
segment-boundary bonuses, no adjacency bonus since the matches aren't
consecutive. Compare atom `atc` against `src/attack.cpp`: `a`, `t`, `c` can
align consecutively inside `attack` (three adjacency bonuses instead, one
segment-boundary bonus for the leading `a`) — a different bonus profile,
illustrating why the DP (not a fixed preference for either "spread across
segments" or "packed together") has to actually compare both candidates'
best alignments rather than assume one shape is always better.

**Case and empty-query handling**: matching is case-insensitive throughout
(both the subsequence test and the bonus rules), but bonus computation
(word-boundary detection via lowercase→uppercase transition) reads the
original-case candidate text, not a lowercased copy. An empty query (or an
empty atom from collapsing whitespace) is treated as matching every
candidate with a score of `0` for that atom — consistent with "no filter
applied," analogous to glob mode's `pattern` defaulting to `*`.

This is unit-testable in isolation — no threading, no incremental state,
pure function of two strings — but is meaningfully more than "simple": the
DP alignment is the part that makes the scorer actually correct, not an
optional refinement.

### 3. `FindTool::execute` — fuzzy branch

A new constant `kFuzzyFindDefaultLimit = 20` (not `kFindDefaultLimit`,
which is `1000` and tuned for glob mode's exhaustive-listing use case — a
*ranked* mode returning 1000 mostly-noise paths burns context and defeats
the point of ranking; `20` matches codex's own default,
`file-search/src/lib.rs:125`).

**Glob mode can `break` at `limit` mid-walk (`builtin_tools.cpp:1041`);
fuzzy mode cannot** — ranking requires comparing every candidate against
every other, so the full tree must be walked and scored before truncating.
This makes the fuzzy path's total work `O(files in tree)` regardless of
`limit`, with no early exit available the way glob mode has one. Two
consequences this plan addresses rather than deferring:

1. **Honor the `std::stop_token` `execute()` already receives but
   `FindTool` currently discards.** This costs a check-every-N-entries
   branch inside the walk loop (same pattern as `should_skip_dir`'s per-
   entry check) and is the only real cancellation mechanism available since
   tool calls have no general timeout. This is Phase 1 work, not a
   follow-up gated on "if a real timeout is reported" — without it, fuzzy
   mode over a large uncooperative tree (see Risks: `vendor/codex` in this
   very repo, which `should_skip_dir`'s fixed
   `.git`/`node_modules`/`build`/`build-asan` list and `GitIgnore`'s
   cwd/root-only `.gitignore` loading do not exclude) is unabortable.
2. **Bound memory with a top-k selection instead of collect-then-sort.**
   Maintain a fixed-size (`limit`-bounded) min-heap (or `std::partial_sort`
   over a bounded buffer) of the best-scoring candidates seen so far,
   rather than accumulating every scored path in a `vector` before sorting.
   This keeps memory proportional to `limit`, not to tree size, matching
   the spirit of codex's approach even without porting its threaded
   walker/matcher split.

Walk skeleton otherwise matches the glob path (`GitIgnore` load,
`should_skip_dir`, `recursive_directory_iterator`) — collect `(relative_posix
path, fuzzy_score_path(path, pattern))` for every regular file via the
top-k structure above, dropping `nullopt` scores. **Symlink handling is
unchanged from today's `find`**: `std::filesystem::recursive_directory_iterator`
does *not* follow directory symlinks by default (`directory_options::none`),
so fuzzy mode inherits the same non-following behavior glob mode already
has — this plan does not claim parity with codex's explicit
`follow_links(true)`, and does not add symlink-following, to avoid
introducing loop-handling as new scope.

Sort/select by score descending, then path ascending for stable,
deterministic tie-breaking (matches codex's rule,
`file-search/src/lib.rs:326-339`). Output format matches glob mode (one
path per line) with one addition: when the result set was truncated by
`limit`, the existing `"[<limit> results limit reached]"` footer is reused
as-is — no separate fuzzy-specific footer, keeping the two modes' output
shape consistent so downstream parsing (if any) doesn't need to
special-case fuzzy results.

No new result type, no new tool registration — `create_coding_tools` and
`create_read_only_tools` need no changes since `find` is already registered
read-only in both.

### 4. What stays identical between modes

Path confinement (`resolve_workspace_path`), `.gitignore` handling
(`GitIgnore`), directory-skip rules (`should_skip_dir`), output truncation
(`truncate_head`, `kMaxBytes`), and the "not a directory" / empty-result
error messages. Only the per-entry test (regex match vs. fuzzy score) and
the sort step differ.

## Phases

### Phase 0 — scorer + tests (no tool behavior change)

- Add `fuzzy_score_path()` (and its supporting helpers) to
  `builtin_tools.cpp`'s anonymous namespace.
- Add unit tests directly in `test/test_builtin_tools.cpp` (or a focused
  `test/test_fuzzy_score.cpp` if the existing file is already large):
  subsequence rejection, whitespace tokenization with multi-atom AND
  matching, path-boundary bonus, basename bonus, adjacency bonus, the DP
  alignment picking the better-scoring alignment over greedy-leftmost on a
  constructed case, case-insensitivity, empty query/atom, query longer than
  candidate, tie-break ordering with a hand-built set of candidate scores.
- Acceptance: new tests green; nothing else changes; `find` behavior
  unchanged.

### Phase 1 — wire `mode: "fuzzy"` into `FindTool`

- Add the `mode` schema property and the fuzzy branch described in §3,
  including the `std::stop_token` check and bounded top-k selection.
- Acceptance: existing `test_builtin_tools.cpp` `find` tests still pass
  unmodified (glob mode untouched); new tests cover: multi-word/tokenized
  queries actually matching (the `buildtools cpp` / `usr auth`-style case
  from the Goal), the §2 worked example's two candidates producing the
  hand-traced bonus totals, fuzzy match across nested directories, ranking
  order for a multi-candidate fixture tree, `kFuzzyFindDefaultLimit`
  truncation footer in fuzzy mode, no-match empty result message,
  `.gitignore`-respecting fuzzy search (a gitignored file never appears
  regardless of how well it scores), and cancellation via `stop_token`
  actually stopping the walk on a large fixture tree.

### Phase 2 — documentation

- Update the `find` tool description string (already covered in §1) and
  `README.md`'s tool listing (if one exists alongside `edit`/`write`/`grep`)
  to mention fuzzy mode with a one-line example.
- No config or CLI surface changes — this is a tool-parameter addition, not
  a new feature toggle. (If a future need arises to disable fuzzy mode
  specifically, `[tools].list` already lets an operator exclude `find`
  entirely, and Lua `before_tool_call` can reject `mode: "fuzzy"` calls
  today with zero new plumbing.)

Estimated size: ~200–260 LOC scorer (including the DP alignment and
tokenization) + fuzzy branch, ~200 LOC tests — revised up from the original
draft's ~120–180 LOC once the DP alignment and top-k/cancellation handling
are counted. Still no new files required (unlike `apply_patch`, this is
small enough to live directly in `builtin_tools.cpp`).

## Risks and mitigations

1. **Scorer quality is unproven against real queries.** Mitigated by
   keeping the algorithm simple, documented, and easy to retune (all
   weights are named constants); Phase 1 acceptance includes a
   representative multi-file fixture so ranking regressions are caught by
   tests, not just vibes.
2. **Fuzzy mode over a huge tree walks and scores every file with no early
   exit** (unlike glob mode's `break`-at-`limit`), and this repo's own
   `vendor/codex` tree is a concrete example of something `should_skip_dir`'s
   fixed `.git`/`node_modules`/`build`/`build-asan` list and `GitIgnore`'s
   cwd/root-only `.gitignore` loading would not prune. This is why §3 makes
   honoring the existing `std::stop_token` a Phase 1 requirement rather than
   a follow-up — it's the only cancellation mechanism available and costs
   only a few lines to wire in.
3. **`GitIgnore` has no `.git`-presence check, unlike codex's
   `require_git(true)`** (confirmed by reading `builtin_tools.cpp:288-344`
   during this review — see the corrected Reference note above). A broad
   parent `.gitignore` above the search root can already silently suppress
   files in glob mode today; fuzzy mode inherits the same behavior rather
   than introducing a new bug, but doesn't fix the pre-existing one either.
   Flagged as a candidate follow-up for `GitIgnore` itself, out of scope for
   this plan.
4. **Query string ambiguity**: nothing stops a model from passing a
   glob-looking string (`*.cpp`) with `mode: "fuzzy"`. Tokenization (§2)
   splits on whitespace only, so `*.cpp` is a single atom scored as literal
   characters (including `*`), which will simply fail to match most files —
   not a crash, just a bad-result case covered by the "no matches" test in
   Phase 1. The tool description explicitly steers the model toward using
   `mode: "glob"` (the default) for pattern-shaped queries.
5. **The DP alignment (§2) is the one piece of new algorithmic complexity
   in this plan** — a bug there produces silently-wrong rankings, not a
   crash, which is harder to catch than a hard failure. Mitigated by making
   the worked example in §2 a directly-portable test case (two candidates,
   hand-traced expected bonus totals) rather than leaving the scorer's
   correctness to prose alone.

## Open questions

- Should fuzzy mode also score directory names (codex's `MatchType`
  distinction, explicitly excluded above)? Deferred until there's a
  concrete use case — `find` returning directories at all would be a
  separate, larger behavior change reviewed on its own.
- Is `+1` basename bonus / `+2` segment-boundary bonus / `+1` adjacency
  bonus the right relative weighting? These are starting constants; Phase
  1's fixture-based ranking tests are the mechanism for tuning them before
  this ships, not a promise that these exact numbers are final.
