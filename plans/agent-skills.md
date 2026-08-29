# Agent skills for pici

## Status

Implementation plan. Not started. Targets the current tree as of
2026-08-20 (`src/core/builtin_tools.cpp` tool sets, `src/cli/system_prompt.cpp`
context assembly, `src/main.cpp` `cmd_run` wiring).

## Goal

Give the model access to on-demand, task-specific instruction packages
("skills") stored as `SKILL.md` files in the workspace and user config
directories. Skills follow the emerging cross-tool convention used by
Claude Code's Agent Skills, Codex (`vendor/codex/codex-rs/skills/`), and pi
(`vendor/pi/packages/coding-agent/src/core/skills.ts`): a directory per skill,
a YAML-frontmatter header declaring `name` and `description`, and a Markdown
body with the actual instructions.

The design follows the **progressive disclosure** model both upstreams settled
on:

1. At session start, pici scans skill directories and injects only a compact
   **index** (name + one-line description + path) into the system prompt.
2. When a task matches, the model calls a new built-in `skill` tool to load
   the full body of exactly one skill into the transcript.
3. The body may reference sibling files (scripts, templates, references);
   the model reads those with the existing `read` tool.

This keeps the always-on token cost proportional to the number of skills
(tens of tokens each), not their content (potentially thousands each), and
keeps unrelated skill text out of every turn.

## Non-goals for the first milestone

- **Implicit / automatic invocation.** Codex has `allow_implicit_invocation`
  policy and command-based detection (`invocation.rs`, `mentions.rs`). V1 is
  explicitly model-invoked via the `skill` tool only. Implicit triggers are a
  follow-up once real usage shows whether they are needed.
- **Skill-provided executable scripts run as tools.** Skills may *reference*
  scripts the model can run through `bash`; pici does not turn them into
  first-class tools or grant them any sandbox exemption.
- **Remote/plugin skills**, dependency declarations between skills, interface
  assets (icons, brand colors), product gating — all deferred. The frontmatter
  parser tolerates unknown keys so richer formats remain forward-compatible.
- **Live rescan mid-session.** Skills are scanned once at startup, like
  context files today. `/reload-addons` extension comes later.
- **Writing skills from the model.** No `skill_write` tool; authors use an
  editor.

## Existing infrastructure this builds on

| Piece | Location | Reuse |
|-------|----------|-------|
| Context-file discovery (walk cwd→root, innermost wins) | `src/main.cpp:883` `load_context_files()` | Same walk pattern for project skill roots |
| System-prompt assembly | `src/cli/system_prompt.cpp` `build_system_prompt()` | New section after `# Project Context` |
| Built-in tool registration | `src/core/builtin_tools.cpp` (`create_coding_tools` / `create_read_only_tools`) | `skill` tool added to both sets |
| Read-only tool inheritance | mailbox children inherit `create_read_only_tools()` | Subagents get skills for free |
| Config plumbing | `src/cli/config.h` `Config`, `[addons]` pattern in `src/cli/config.cpp:523` | New `[skills]` table |
| Slash commands | TUI dispatch around `src/main.cpp:1829` | `/skills` list command |
| Self-hosted tests | `test/test_system_prompt.cpp` pattern, CMake `foreach` block at `CMakeLists.txt:584` | New `test-skills` target |
| Faux-control integration testing | `docs/faux-control.md`, `src/core/providers/faux_control.cpp` | End-to-end skill-load scenario without a live model |

## Reference implementations studied

- `vendor/codex/codex-rs/skills/src/parser.rs` — frontmatter extraction,
  validation limits (name ≤ 64, description ≤ 1024), and a line-oriented
  "repair" pass for prose descriptions that break strict YAML
  (`description: Build for AWS: ECS`). We copy the limits and the tolerance
  philosophy, not the serde machinery.
- `vendor/codex/codex-rs/skills/src/loading.rs` — multi-root loading with
  snapshot caching; we adopt multi-root precedence but skip caching (single
  scan per session).
- `vendor/pi/packages/coding-agent/src/core/skills.ts` — gitignore-aware
  discovery and frontmatter parsing via a shared util
  (`src/utils/frontmatter.ts`: `---` delimited, first `---` wins).
- `vendor/pi/packages/coding-agent/src/core/tools/` — confirms upstream keeps
  skill loading out of the file tools; we likewise give skills their own tool.

## Design

### 1. On-disk layout and discovery

A skill is a directory containing `SKILL.md`. The directory name is the
canonical skill name unless frontmatter overrides it.

Scanned roots, in ascending precedence (later overrides earlier on name
collision):

1. `<agent-dir>/skills/` — user scope, `$XDG_CONFIG_HOME/pici/skills`
   (default `~/.config/pici/skills/`)
2. Workspace ancestors, outermost → innermost (same walk direction as
   `load_context_files()`), checking each of:
   - `.pici/skills/`
   - `skills/` (plain, for ecosystem compatibility)
   - `.agents/skills/` (cross-tool convention)

Compatibility scans (read-only, lower precedence than any native root):

3. `.claude/skills/` and `.codex/skills/` in workspace ancestors — many
   repositories already carry these; pici already reads `CLAUDE.md`, so
   reading Claude-format skills is consistent with existing behavior.

Rules:

- Discovery depth is bounded (see §5 hardening) and directories named
  `node_modules`, `.git`, or starting with `.` inside a skill root itself are
  skipped (except the root names above).
- A `SKILL.md` whose frontmatter fails validation produces a **diagnostic**
  (surfaced like config diagnostics, never fatal) and is skipped.
- Name collisions: highest-precedence root wins; within one root, the
  lexicographically first path wins deterministically; losers produce a
  diagnostic naming the winner.

### 2. Frontmatter format

```markdown
---
name: release-checklist
description: Steps to cut a release: version bump, changelog, tag, smoke test.
---

# Release checklist

1. Run `make release` …
```

- Delimiters: leading `---` line, closed by the next line that is exactly
  `---` (mirrors `frontmatter.ts`). Missing frontmatter ⇒ diagnostic, skill
  skipped.
- Recognized keys (all others parsed and retained but unused):
  - `name` (optional; defaults to directory name). Validated: 1–64 chars,
    lowercase alphanumerics and `-`/`_` (matches codex limits; keeps the name
    usable as a tool argument).
  - `description` (required, 1–1024 chars). This is the retrieval signal —
    docs should encourage "what it does + when to use it".
  - `short-description` (optional, ≤ 256) — reserved for future UI/status-line
    use; parsed, not injected in v1.
- Parsing strategy: **line-oriented subset**, no YAML dependency. Accept
  `key: value` and `key: >`/`|` folded scalars flattened naively; apply a
  codex-style repair pass (if a value contains `: `, retry treating the rest
  of the line as the scalar). Unknown keys ignored. This lives in
  `parse_skill_frontmatter()` with unit tests covering: valid minimal,
  missing description, over-long fields, prose-with-colons repair, CRLF
  input, BOM, empty body, no closing delimiter.

### 3. Core types — new files `src/core/skills.{h,cpp}`

```cpp
namespace pi::core {

struct SkillMetadata {
  std::string name;          // canonical, unique across the catalog
  std::string description;   // from frontmatter, ≤ 1024 chars
  std::string path;          // absolute path to SKILL.md
  std::filesystem::path dir; // skill directory (for sibling-file reads)
  std::string scope;         // "user" | "project" | "compat"
};

struct SkillCatalog {
  std::vector<SkillMetadata> skills;      // sorted by name
  std::vector<std::string> diagnostics;   // parse/validation/collision notes
};

// Scan all roots; pure function of (cwd, agent_dir) — no I/O beyond stat/read.
SkillCatalog discover_skills(const std::filesystem::path &cwd,
                             const std::filesystem::path &agent_dir);

// Load and validate one skill body. Returns error text on failure.
// Enforces size limit and path containment (§5).
std::expected<std::string, std::string> load_skill_body(const SkillMetadata &);

}
```

`discover_skills()` is deliberately a free function returning a plain struct:
it is called once in `cmd_run()` before the agent is constructed, and the
result is passed by const-ref into prompt building and the tool factory. No
global state, matching how `ContextFile`s flow today.

### 4. Prompt injection

In `build_system_prompt()`, after the `# Project Context` section, when the
catalog is non-empty:

```
# Skills

Structured instruction packages available in this workspace. When a task
matches one of these descriptions, call the `skill` tool with its name to
load the full instructions before proceeding.

- release-checklist: Steps to cut a release: version bump, changelog, tag.
- pdf-extraction: Extract tables/text from PDF files using python + pdfplumber.
```

- Only `name: description` lines — never paths (paths leak layout and waste
  tokens; the tool resolves them internally).
- If the catalog exceeds 48 entries, truncate the list at 48 and append a
  final line telling the model how many more exist and that `skill` accepts
  exact names only (prevents unbounded prompt growth; a diagnostic suggests
  pruning).
- Empty catalog ⇒ nothing injected, and the `skill` tool is not registered
  (so prompts and tool lists stay consistent).

### 5. The `skill` tool

Registered in `builtin_tools.cpp` in **both** `create_coding_tools()` and
`create_read_only_tools()` (it is read-only; mailbox children therefore
inherit it automatically). Signature mirrors the existing internal tool
shape:

- Name: `skill`
- Description: "Load the full instructions of a named skill. Use when the
  task matches a skill listed under '# Skills' in your system prompt."
- Schema: `{"type":"object","properties":{"name":{"type":"string"}},
  "required":["name"],"additionalProperties":false}`
- Behavior on execute:
  1. Exact-name lookup in the catalog (no fuzzy matching in v1; on miss,
     return an error result listing the closest three names — cheap and
     saves a whole retry turn).
  2. `load_skill_body()` reads the file fresh from disk (no cache — edits
     during a long session are picked up on next call, which aids skill
     development).
  3. Result content: the body verbatim, prefixed with one header line
     `Loaded skill '<name>' from <path>. Sibling files referenced below can
     be read relative to <dir>.`

Hardening limits (constants in `skills.cpp`, unit-tested):

- Body size cap: 128 KiB after stripping; larger ⇒ error result, not a
  crash or silent truncation (truncation would silently teach the model a
  wrong procedure).
- Discovery per-root entry cap: 256 skills; total cap 512.
- Path containment: the resolved `SKILL.md` path must remain inside its
  declared root (reject symlink escapes discovered during scan); sibling-file
  reads go through the normal `read` tool, which already constrains to the
  workspace/sandbox — skills gain no new file access.

Security posture: skill text is untrusted-ish workspace content, exactly like
AGENTS.md/CLAUDE.md today — same trust level, same threat model (prompt
injection from a cloned repo). Documented in the README section rather than
mitigated in code; the sandbox and permission layers already gate what the
instructions can actually do.

### 6. Config and CLI surface

`config.toml`:

```toml
[skills]
enabled = true            # master switch, default true
dirs = ["~/my-skills"]    # extra roots, lowest precedence, above user scope
max_index = 48            # prompt index cap
```

CLI:

- `--no-skills` — disable scanning and omit the tool (peer of
  `--no-context-files` at `args.cpp:254`).
- `/skills` slash command — prints the catalog table (name, scope, path,
  description) plus any diagnostics; works in the interactive TTY only, like
  `/models`.

### 7. Lua surface (minimal)

Expose the catalog read-only to add-ons:

```lua
-- in any hook
local cats = pici.skills and pici.skills.list() or {}
```

No hook may mutate the catalog in v1. This lets `status_line` addons show
active-skill hints later and costs almost nothing to add while wiring
`cmd_run`. Mutation hooks (`on_skill_load` filtering) are explicitly deferred
to avoid freezing an API before usage patterns exist.

## Phases

### Phase 0 — core module + tests (no behavior change)

- Add `src/core/skills.{h,cpp}`: frontmatter parser, discovery walker,
  `load_skill_body()`.
- Add `test/test_skills.cpp` (parser edge cases, precedence matrix, collision
  diagnostics, size caps, symlink containment) and register `test-skills` in
  the CMake `foreach`.
- Acceptance: `ctest -R test-skills` green; nothing else changes.

### Phase 1 — prompt injection

- Thread `SkillCatalog` from `cmd_run()` into `build_system_prompt()`
  (new trailing parameter; update `test_system_prompt.cpp`).
- Inject the `# Skills` section per §4 including the 48-entry cap.
- Acceptance: faux-control run with a fixture workspace shows the section;
  `--no-context-files`-style flag (`--no-skills`) suppresses it.

### Phase 2 — the `skill` tool

- Implement in `builtin_tools.cpp`, registered in both factory functions,
  gated on non-empty catalog.
- Miss returns closest-name suggestions; body loads fresh from disk.
- Acceptance: faux-control scenario where the scripted model calls `skill`,
  receives the body, and the transcript records the tool result; mailbox
  child session (read-only tools) sees the tool present.

### Phase 3 — config + CLI

- `[skills]` table in `config.cpp` following the `[addons]` pattern;
  `--no-skills`; `/skills` command with completion stub (readline already
  completes slash commands).
- Diagnostics printed alongside existing config diagnostics at startup.
- Acceptance: `test-config` covers TOML parsing; manual TTY check for
  `/skills`.

### Phase 4 — documentation + fixtures

- README section ("Skills") mirroring the addons section: layout, frontmatter
  reference, security note, worked example.
- Fixture skill(s) under `test/fixtures/skills/` reused by tests and the
  faux-control scenario.

### Future (not in this plan's milestones)

- Implicit invocation policies (`policy:` frontmatter), command-triggered
  skills, `/reload-skills`, Lua mutation hooks, skill-authored sub-tools,
  telemetry counters (loads per skill) behind the OTel switch.

## Risks and mitigations

1. **Prompt bloat from large catalogs.** Hard cap + truncation notice (§4);
   cap is config-tunable.
2. **Model confusion between `skill` and `read`.** The tool description and
   the injected preamble both state the contract ("call `skill`, then `read`
   siblings"); faux-control test asserts the sequence works with only these
   hints.
3. **Frontmatter dialect drift** across ecosystems (Claude/Codex/pi extras
   like `allowed-tools`, `metadata:` blocks). Parser ignores unknown keys and
   survives unknown *shapes* because it is line-oriented; a malformed exotic
   key degrades to a diagnostic on that one skill, never the scan.
4. **Collision with future MCP tool names.** Skill names occupy their own
   namespace (the `skill` tool's argument space), so no registry conflict.
5. **Windows path handling.** All walking uses `std::filesystem` with
   `error_code` overloads, matching `load_context_files()`; no POSIX-only
   assumptions beyond symlinks being best-effort.

## Open questions

- Should `skills/` (plain) be scanned by default, or only `.pici/skills/`?
  Scanning plain `skills/` maximizes ecosystem compatibility but risks
  picking up unrelated directories in odd repos. Default plan: scan it, but
  drop it behind `[skills] scan_plain_dir = true` (default true) so it can be
  turned off without a code change.
- Whether `/compact`-style remote compaction interacts with skill results —
  currently none: skill bodies are ordinary `ToolResultMessage` content and
  compact like any tool output.
