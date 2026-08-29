# `apply_patch`: multi-file unified-diff editing

## Status

Implementation plan. Not started. Targets the current tree as of 2026-08-23
(`src/core/builtin_tools.cpp` `EditTool`/fuzzy-match helpers, `src/cli/
system_prompt.cpp` tool descriptions, `src/cli/config.cpp` `[tools]` handling).

Supersedes an earlier draft (`plan_U_apply_patch.md`, now folded into this
file). Revised after an Opus pre-implementation review flagged five
technical errors in the original draft (matcher reuse, ambiguity handling,
diff rendering, line-ending defaults, intra-patch dependencies) and one
concrete regression risk (child-agent tool leak); all are corrected below.

## Goal

Give pici a codex-style `apply_patch` tool: a single tool call that can add,
update, move/rename, and delete **multiple files atomically**, driven by the
V4A patch format that OpenAI coding models (and increasingly others, via
imitation in training data) emit natively (`*** Begin Patch`). The tool
streams a rendered unified diff as progress output, applies all hunks or
none, and reports precise per-hunk failure diagnostics.

This was flagged as a gap during a Codex-vs-pici feature survey: pici's only
mutation tools are `edit` (single old-text/new-text replacement per call,
fuzzy-matched) and `write` (whole-file). Multi-file refactors currently need
N sequential `edit`/`write` calls with N independent failure points and no
rollback.

## The decision: replace, don't add

**Recommendation: `apply_patch` becomes the primary code-modification
surface, staged over four phases. Do not ship two peer editing tools
long-term.**

Rationale:

1. **Model fit.** Pici already ships an `openai-codex-responses` provider
   (`src/core/providers/openai_codex_responses.cpp`); models served through
   it were trained to emit `*** Begin Patch` blocks. Today those emissions
   have no native consumer — the model is forced to translate its natural
   output format into per-call JSON edits, which is exactly where
   lost-context and mismatch errors come from.
2. **Two editors means ambiguous guidance.** Every system-prompt tool
   description, every Lua permission hook (`addons/permissions.lua`-style
   `before_tool_call` filters), and every model has to choose between `edit`
   and `apply_patch`. Codex tried both worlds historically and converged on
   one: `apply_patch` is its only mutation tool, to the point that it even
   intercepts patch text pasted into `bash` commands.
3. **Multi-file atomicity is a real capability win**, not just format
   parity: a patch is validated fully, then applied fully, or not at all.

What this plan does **not** do:

- Keep `edit` as a permanent co-equal alternative. During the transition
  (Phases 2–3) both are registered; removal is gated on evals (Phase 3).
- Replace `write`. Whole-file creation is cheap, unambiguous, and orthogonal;
  `*** Add File:` overlaps it, but `write` stays until separately evaluated.
- Port codex's exec-server/filesystem abstraction or `PathUri` machinery.
  pici applies directly through the existing `resolve_workspace_path` +
  file read/write helpers, same as `EditTool` does today.
- Build the bash-output patch interception (scanning completed `bash`
  output for a stray `*** Begin Patch` block) in the initial milestones —
  cut line, see Phase 4.

## Existing infrastructure this builds on

| Piece | Location | Reuse |
|-------|----------|-------|
| Unicode-normalizing text transform | `src/core/builtin_tools.cpp:629` (`normalize_for_fuzzy_match`) | Reused **only as the final rung** of a new line-vector seek ladder — see §4, this is not a drop-in reuse of `fuzzy_find`/`count_occurrences` |
| Workspace path confinement | `resolve_workspace_path()` (used throughout `builtin_tools.cpp`) | Same confinement for every hunk path, resolved up front |
| BOM/CRLF-preserving read/write | `EditTool`'s read/write path in `builtin_tools.cpp` | Preserve, don't replace — same behavior contract as `edit` today |
| Tool registration | `create_coding_tools()` / `create_read_only_tools()`, `builtin_tools.cpp:1363-1395` | `apply_patch` joins the coding set only (it's a mutation tool) |
| Tool-call progress streaming | `ToolUpdateCallback` (used by `bash`, others) | Emit the rendered diff *before* mutation, matching the "show what's about to happen" pattern |
| Config `[tools].list` allow/deny | `src/cli/config.cpp`, `config.toml.example` | New tool name added to the documented list; new `[tools] edit_mode` key follows the same table |
| Lua `before_tool_call` hook | README hooks section | No code change — hook already sees any tool by name |
| Self-hosted tests | `test/test_builtin_tools.cpp` pattern, CMake `foreach` block | New `test-apply-patch` target |

## Reference implementations studied

- `vendor/codex/codex-rs/apply-patch/src/parser.rs` (682 L) — grammar,
  marker constants, `Hunk`/`UpdateFileChunk`, strict-vs-lenient parsing.
- `vendor/codex/codex-rs/apply-patch/src/file_update.rs` (335 L) — chunk
  application, derive-new-contents, unified-diff rendering.
- `vendor/codex/codex-rs/apply-patch/src/seek_sequence.rs` (193 L) —
  tolerance ladder: exact → rstrip → trim → Unicode-normalized, operating on
  **line vectors** with a `start` offset (it seeks forward from the previous
  chunk's end). pici's existing `fuzzy_find`/`count_occurrences`
  (`builtin_tools.cpp:732-756`) are **not** a substitute: they operate on a
  whole file as one byte string, have no `start` offset, and — critically —
  on a fuzzy hit `EditTool` writes back the *normalized* version of the
  entire file (see `apply_edits` writing `base`, the normalized content),
  not just the matched region. That behavior must not carry over: this plan
  ports `seek_sequence`'s four-rung ladder directly over `vector<string>`
  lines (exact, rstrip-per-line, trim-per-line, and a final rung reusing
  `normalize_for_fuzzy_match` per line), matching only the lines actually
  covered by a chunk and leaving every other line byte-identical.
- `vendor/codex/codex-rs/apply-patch/src/streaming_parser.rs` (924 L) —
  incremental parsing. Not needed: pici applies a patch as one complete tool
  argument, never mid-stream from raw model output.
- `vendor/codex/codex-rs/apply-patch/src/{lib,invocation}.rs` — options,
  verification flow, heredoc extraction.
- `vendor/codex/codex-rs/core/src/tools/handlers/apply_patch*.rs` — tool
  schema, result presentation, bash interception (Phase 4 reference only).
- `vendor/pi/packages/coding-agent/src/core/tools/edit-diff.ts` — a second
  data point on LF/CRLF/BOM normalization conventions; confirms pici's
  existing `EditTool` behavior is already the right contract to preserve.

## Format (V4A)

```
*** Begin Patch
*** Add File: path/to/new.cpp
+line one
+line two
*** Update File: src/existing.cpp
@@ def classify_chunk(chunk):
-context lines (exact match required by seek ladder)
-removed line
+added line
*** Update File: src/old_name.h
*** Move to: include/new_name.h
@@
-deprecated fn
*** Delete File: tmp/scratch.txt
*** End Patch
```

Markers (constants ported from `parser.rs`): `*** Begin Patch`,
`*** End Patch`, `*** Add File: `, `*** Delete File: `,
`*** Update File: `, `*** Move to: `, `*** End of File`, and `@@` /
`@@ context` chunk anchors.

Notes:

- `@@ context` is a *hint line* (a function/class header) used to
  disambiguate which occurrence of the chunk body to match — not part of the
  replaced text.
- `*** End of File` anchors a chunk to the end of the file regardless of
  trailing content drift.
- pici deliberately drops codex's `Environment ID` support (multi-environment
  orchestration is out of scope).
- Empty `-`/`+` lines represent empty lines; a chunk consisting only of `+`
  lines appends at the anchor.

## Design

### 1. New files — `src/core/apply_patch.h/.cpp`

Parser + applier + diff renderer, mirroring codex's `parser.rs` +
`file_update.rs` + `seek_sequence.rs`, minus sandbox/exec-server plumbing
(estimated ~700–900 LOC).

```cpp
namespace pi::core {

struct UpdateFileChunk {
  std::optional<std::string> change_context; // "@@ hint"
  std::vector<std::string> old_lines;         // "-" and " " lines
  std::vector<std::string> new_lines;         // "+" and " " lines
  bool anchored_to_eof{false};                // "*** End of File"
};

using Hunk = std::variant<AddFile, DeleteFile, UpdateFile>;
// UpdateFile carries: path, optional move_path, ordered chunks.

struct Patch {
  std::vector<Hunk> hunks; // in patch order
};

struct ParseDiagnostic { // line numbers are 1-based
  std::size_t line{};
  std::string message;
};

// Throws-free: parse returns a diagnostic instead of throwing, matching the
// tool boundary's fail-loud style without adding exceptions to the parser.
std::optional<Patch> parse_patch(std::string_view text, bool lenient,
                                  ParseDiagnostic &error);

struct ApplyPatchOptions {
  bool create_parents{true}; // Add File creates missing directories
  // `preserve` is defined but not implemented in v1 (rejected with a clear
  // error) — see §5, it needs per-line terminator tracking this plan
  // defers. Default matches codex, not EditTool's CRLF-preserving contract.
  enum class LineEndings { normalize_lf, preserve } line_endings{
      LineEndings::normalize_lf};
};

struct AppliedPatch { // what the tool reports
  std::vector<std::string> added, updated, deleted, moved; // display paths
  std::string unified_diff; // full rendered diff for the result
};

AppliedPatch apply_patch(const std::filesystem::path &cwd, const Patch &patch,
                          const ApplyPatchOptions &options);

} // namespace pi::core
```

`ApplyPatchTool` (in `builtin_tools.cpp`) follows `EditTool`'s shape:
`prepare_arguments` unwraps `{input}` / string-coerced JSON, `execute`
catches `std::exception` → `error_result`. Single freeform string argument
(the whole patch as `input`), matching how models emit it — no `{"patches":
[...]}` alternative form; one format, one parameter.

### 2. Tool schema

```json
{"name":"apply_patch",
 "description":"Apply a multi-file patch using the V4A format ...",
 "input_schema":{"type":"object",
   "properties":{"input":{"type":"string",
     "description":"Full patch text starting with *** Begin Patch"}},
   "required":["input"],"additionalProperties":false}}
```

### 3. Parser algorithm

Line-based state machine over LF-normalized input (port of `parser.rs`):

1. Scan for `*** Begin Patch`; refuse if absent (lenient mode also accepts a
   bare hunk stream so pasted diffs work).
2. Dispatch on `*** Add File:` / `*** Delete File:` / `*** Update File:`.
   Subsequent `+` lines belong to Add; `*** Move to:` mutates the current
   Update hunk's destination; `@@`/`@@ ctx` starts a chunk; `-`/`+`/` `
   extend the current chunk; `*** End of File` flags the chunk.
3. Strict mode rejects: unknown `***` directives, chunks whose `old_lines`
   contain interior blank-line-only runs inconsistent with context, Add
   files with no `+` lines. Lenient mode (default, matches codex's
   `PARSE_IN_STRICT_MODE = false`) tolerates leading/trailing whitespace
   around markers and documented model quirks.
4. Validate chunk ordering per file: each chunk's anchor must occur
   at-or-after the previous chunk's match start (re-checked during apply).

### 4. Matching algorithm (seek ladder)

A new line-vector seek function, `seek_lines(lines, pattern, start)`, ported
from `seek_sequence.rs` — **not** a reuse of `fuzzy_find`/`count_occurrences`
(see the corrected reference note above). Four rungs, tried in order at the
given `start` offset:

1. exact line-for-line match;
2. rstrip-equal per line (ignore trailing whitespace);
3. trim-both-equal per line;
4. `normalize_for_fuzzy_match`-equal per line (smart quotes, dashes, spaces →
   ASCII) — this is the one rung that does reuse existing code.

EOF-anchored chunks try `lines.size() - pattern.size()` first, then fall
back to forward search.

**Ambiguity is resolved by chunk order, not rejected.** Chunks within one
file's `UpdateFile` hunk are applied in patch order, and each chunk's search
starts at `start = <end line of the previous chunk's match>` (0 for the
first chunk) — this mirrors codex's monotonically advancing `line_index`
(`file_update.rs:94-113,208`) and is *why* a patch containing repeated lines
(`}`, `return nil`) works at all: context and order disambiguate, the same
way a human reading a unified diff resolves it. This plan does **not** carry
over `edit`'s global "reject if the pattern matches more than once in the
whole file" discipline — that would reject legitimate patches. A chunk that
fails to match *at or after* its required `start` (i.e., the sequential
search runs off the end of the file with no hit at any rung) is the actual
failure case, reported with the chunk index, file, and the line the search
started from.

### 5. Application pipeline

1. **Resolve** every hunk path through `resolve_workspace_path` up front —
   escapes fail before any I/O.
2. **Build an in-memory overlay**: a `map<canonical_path, OverlayEntry>`
   tracking the pending state of every path touched anywhere in the patch
   (`Absent` after a Delete, `Content(lines)` after an Add/Update, tracked
   under its post-Move path once a Move is seen). Hunks are processed in
   patch order; a hunk that targets a path already in the overlay reads
   from the overlay instead of disk (this is what makes Add-then-Update,
   Move-then-Update, and Update-then-Delete on the same path well-defined —
   the original draft's "delete last among same-file operations" rule was
   not sufficient for this). A hunk that logically conflicts with the
   overlay's current state for that path (e.g. `Update` after that path was
   already `Delete`d earlier in the same patch) is a validation failure.
3. **Read & validate**: for each hunk, resolve its input from the overlay if
   present, else from disk (loaded once, cached in the overlay after first
   read). Run chunk matches via `seek_lines` (§4); update the overlay with
   the new content. Any failure aborts with a per-hunk diagnostic (hunk
   index, file, line, reason). Nothing has been written to disk yet — this
   is the atomicity guarantee; the overlay is the only mutable state during
   validation.
   - Add File: must not exist on disk *and* not already `Content` in the
     overlay (an empty existing file is allowed, matching codex).
   - Delete File: must exist (on disk or via overlay); an optional
     `expected_contents` hash is omitted in v1 (see Open questions).
   - Move to: destination must not exist (disk or overlay); source content
     carries over into the overlay under the new path, and the old path is
     marked `Absent`.
4. **Write**: once every hunk validates, flush the overlay to disk — one
   write per path that ended in `Content`, one removal per path that ended
   `Absent` and existed on disk at the start. Line-ending default is
   **normalize-to-LF**, matching codex's default (`lib.rs:64-67`) — this is
   a deliberate deviation from `EditTool`'s CRLF-preserving contract,
   documented explicitly in the tool description and README, because
   preserving original line endings per-file requires tracking a
   terminator per line (`UpdateFileChunk` would need a
   `line_endings` field alongside `old_lines`/`new_lines`, and the writer
   would need to reconstruct per-line terminators rather than pici's
   current whole-file "any CRLF found → CRLF the whole file" heuristic,
   which already mishandles mixed-ending files today). CRLF-preserving mode
   is deferred to a follow-up once that data-structure change is scoped on
   its own; v1's `ApplyPatchOptions::LineEndings` enum keeps both values
   defined but only `normalize_lf` is implemented, and `preserve` is
   rejected with a clear "not yet implemented" error rather than silently
   behaving like `normalize_lf`.
5. **Report**: build a unified diff per touched file **directly from each
   chunk's known `old_lines`/`new_lines` and the line range `seek_lines`
   matched** — no general-purpose diff algorithm is needed, because the
   exact changed line range is already known from matching (this is
   *pici's* simplification, not a claim about how codex does it: codex
   actually derives full new-file contents and re-diffs with
   `similar::TextDiff::from_lines`, which pici has no equivalent library
   for and does not need, since it never loses track of which lines
   changed). Join per-file diffs into `AppliedPatch::unified_diff`.

Result text: `"Applied patch: 2 added, 1 updated, 1 deleted, 1 renamed"`
followed by the diff. Emit the same diff through `ToolUpdateCallback`
**before** executing, so verbose renderers (and any future approval flow)
show what is about to happen.

### 6. Integration

- `create_coding_tools`: insert `apply_patch` before `edit` — ordering
  matters for `/tools` listing and system-prompt position.
- `create_read_only_tools` / mailbox child inheritance: unchanged (mutation
  tools are already excluded from the read-only set).
- **`agent_task.cpp`'s child-write-tool gate** (around lines 455-485,
  `ChildWriteTools`) hardcodes the tool names `"edit"`/`"write"`/`"bash"`
  that subagents may receive write access to, independent of the general
  `create_coding_tools`/`create_read_only_tools` split. `apply_patch` must
  be added to this list *deliberately* as part of registration — if it is
  registered as a coding tool but left out of `ChildWriteTools`, either (a)
  subagents silently lose patch-apply capability that `edit` gave them, or
  worse (b) if `ApplyPatchTool` is marked `child_safe=true` the way
  `EditTool` is without updating this gate, subagents could get an
  unguarded mutation tool regardless of `agents.write_tools` config. Phase 2
  acceptance must include a test asserting the gate's behavior explicitly,
  not just that `apply_patch` exists in the parent's tool list.
- `test/test_builtin_tools.cpp` currently asserts
  `create_all_tools().size() == 7` (or similar exact-count assertion) —
  expect this to need updating in Phase 2 as an intentional, reviewed
  change, not a surprise CI failure.
- `[tools].list` in `config.toml.example`: add `"apply_patch"`; document
  that setting the list disables unspecified builtins (existing semantics).
- Phase 3 adds `[tools] edit_mode = "patch" | "legacy" | "both"` (default
  flips `"legacy"` → `"patch"` only after evals pass). `"patch"` drops
  `edit` from the registry entirely rather than hiding it — a
  hidden-but-present tool confuses a model that enumerates `/tools`.
- `src/cli/system_prompt.cpp`: add a tool description for `apply_patch` and
  a guideline bullet: *"Prefer apply_patch for multi-file or multi-hunk
  changes; use write for brand-new whole files."*
- Lua hooks: no code changes — `before_tool_call` already sees `apply_patch`
  like any other tool by name. Update any permissions-addon example
  blocklist to cover both `edit` and `apply_patch` during the transition.
- ACP/RPC: nothing special; it rides the normal tool-call path.

## Phases

1. **Library**: `parse_patch` + `apply_patch` + seek ladder + diff renderer
   + full test suite. No tool registration. Shippable and revertible alone.
2. **Registration (opt-in)**: `apply_patch` joins `create_coding_tools`;
   system prompt + config example updated; diff-before-execute callback;
   permissions-addon example refreshed. Default `[tools].edit_mode` stays
   `"legacy"` (edit remains primary) — early adopters opt in.
3. **Default flip**: evals pass (see Eval gate below) → `edit_mode =
   "patch"` becomes the default; `edit` survives behind `"legacy"`/`"both"`;
   announce the flip in the commit message/docs.
4. **Cleanup (optional, cut line)**: bash-output interception — scan
   completed `bash` tool output for a `*** Begin Patch … *** End Patch`
   block; if found, log-and-skip executing it as a shell command and surface
   the parsed patch as an `apply_patch`-equivalent result instead (closes
   the `cat <<EOF >> file` escape hatch codex also closes). Also consider a
   standalone `pici --apply-patch < file` entry point for scripting/tests.
   Retire `edit` only if a telemetry-free period stays quiet. Defer unless
   real usage shows models leaning on the bash escape hatch.

Estimated size: ~900 LOC core, ~800 LOC tests, ~40 LOC wiring.

## Test plan (`test/test_apply_patch.cpp`)

**Parser**: minimal/whitespace-tolerant/lenient quirk cases; every malformed
marker; unterminated patch; chunk with context hint; `*** End of File`
anchor; `Move to` without a preceding `Update` (error); duplicate `Add`
(error).

**Matcher**: each ladder rung exercised independently (exact, rstrip, trim,
Unicode); ambiguity rejection with candidate lines; EOF fallback when
trailing content has drifted; pattern longer than the file; empty pattern
no-op.

**Applier**: multi-hunk happy path across 4 files; **atomic rollback** — a
patch where hunk 3 of 4 fails leaves the tree byte-identical (capture
hashes before applying); Add creates parent directories; Add-existing
fails; Delete-missing fails; Move-over-existing fails; CRLF file preserved;
BOM preserved; file ending without a trailing newline handled correctly;
symlinked path that would escape the workspace is rejected.

**Tool surface**: `execute()` round-trips through JSON args including the
string-coercion quirk; `error_result` message quality (mentions hunk index
+ line); the update-callback fires with the diff before mutation (assert
call order).

**Interop/regression**: rerun the existing `test_builtin_tools.cpp` edit
cases against equivalent patches where feasible, keeping both suites green
through Phase 2.

**Eval gate (blocks Phase 3)**: scripted tasks × {`edit`, `apply_patch`} ×
{a codex-family model, a non-OpenAI model}, measuring first-attempt success
rate and tokens spent. Flip the default only if `apply_patch` is at parity
or better on non-OpenAI models and strictly better on codex-family models.

## Risks and mitigations

1. **Two tools confuse the model during the transition.** Mitigated by the
   `edit_mode` config gate (Phase 2 default keeps `edit` primary) and by
   clear guideline text once `apply_patch` is promoted.
2. **Silent behavior drift from codex's reference implementation** (e.g. an
   edge case in the seek ladder). Mitigated by porting `seek_sequence`'s
   four-rung structure directly (§4) rather than approximating it with
   `edit`'s existing whole-file fuzzy matcher, which has different semantics
   (no `start` offset, only two rungs, and normalizes the entire file on a
   fuzzy hit) — a plausible-looking shortcut that would have silently
   diverged from codex's behavior on any fuzzy-matched chunk.
3. **Atomicity claim is only as strong as the write step.** A crash between
   per-file writes (step 3) could leave a partial patch on disk despite the
   "validate everything first" design. Document this limitation explicitly;
   true transactional writes (temp-file + rename) are a possible follow-up,
   not required for v1 since pici doesn't currently guarantee atomic
   multi-file writes anywhere else either.
4. **Windows path handling.** All walking/resolution uses the same
   `std::filesystem` + `resolve_workspace_path` helpers as `EditTool`
   already does; no new POSIX-only assumptions beyond symlink handling
   being best-effort.
5. **Collision with a future MCP tool of the same name.** `apply_patch` is
   a fixed built-in tool name; if MCP support (a separate, larger gap noted
   in the Codex survey) is added later, built-in tool names take precedence
   the same way `edit`/`write` already do.

## Open questions

- Keep `write` indefinitely, or fold it into `Add File` in Phase 4? Leaning
  toward keeping it — whole-file creation is common enough to deserve a
  dedicated, simpler tool.
- Should `Delete File` require an `expected_sha256` argument for extra
  safety once an approval/confirmation flow exists (also flagged as a gap
  in the Codex survey)? Cheap to add to the schema later; not needed now.
- Streaming/incremental patch parsing only matters if pici ever applies
  patches mid-stream from raw model output outside the tool-call path;
  not needed today. Skip.
- The default tool-call rendering surface (`main.cpp`'s
  `[tool: name(<raw JSON args>)]` line) will inline the *entire* patch text
  for `apply_patch` calls, unlike the `ToolUpdateCallback` diff which only
  fires around execution. Worth a follow-up look at whether that raw-args
  rendering needs a patch-aware summary, but not a blocker for Phase 1.
- `parse_patch`'s throws-free `(..., ParseDiagnostic&)` signature deviates
  from the rest of the tool-call boundary's throw→`error_result` style
  (`EditTool`/`WriteTool` both throw `std::runtime_error`). Kept as
  designed above since a parser producing a diagnostic value is a reasonable
  and common pattern for a pure library function with no I/O, but note this
  is a stylistic choice, not a strict requirement — revisit if it reads
  awkwardly once implemented.
- The three-way `edit_mode = "patch" | "legacy" | "both"` config may be
  more than one migration needs; a plain boolean would work too. Left as
  three-way for now since it makes the rollback path (`"legacy"`) and a
  transitional dual-tool state (`"both"`) each independently nameable, but
  worth simplifying if Phase 2 usage shows `"both"` never gets used.
