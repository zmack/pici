# Composer / input-box rewrite: multi-line textarea, word-wrap, vim mode

## Goal

Replace pici's single-line input box (`src/cli/readline.cpp`) with a real
multi-line textarea: word-boundary wrapping, Shift+Enter for newlines,
standard line-editing bindings, an optional vim mode, and a visual refresh —
without regressing history recall, tab completion, or transcript scrolling,
all of which currently share the same key-handling loop.

Comparison research (pici vs. the vendored Codex TUI at
`vendor/codex/codex-rs/tui`) is the basis for this plan. This revision folds
in an Opus review pass that found real blockers in the original M1 scoping —
see "Review findings folded in" below for what changed and why.

## Current state (what's actually broken today)

**Buffer model.** The whole editor is a flat `std::string buf` with a byte
cursor (`readline.cpp:613-619`). `\r`/`\n` unconditionally submits
(`readline.cpp:703-704`) before any buffer insertion — there is no code path
by which a newline can ever reach `buf`. Consequence: pasting multi-line text
truncates to its first line and fires spurious early submissions for the
rest (no bracketed-paste handling exists either — no DECSET `?2004`
anywhere, so a pasted `\n` is byte-identical to a keystroke).

**Confirmed wrap-boundary clip bug** (the one reported this session),
**widened per review.** `InputRenderer::redraw` (`readline.cpp:256-311`)
captures `cursor_position` inside `write_wrapped` at the top of each loop
iteration (`readline.cpp:361-364`) or at the end-of-text tail
(`readline.cpp:405-408`) — i.e. *before* that character's wrap decision is
made. Three distinct ways this produces a bad position, all the same root
cause:

1. **End-of-text, row exactly full.** `redraw` appends a synthetic blank row
   when `column == columns` (`readline.cpp:283-287`) *after*
   `cursor_position` was already captured against the pre-adjustment layout.
   Cursor math (`readline.cpp:291-297`) walks back onto the full row's last
   cell; `CSI n C` clamps at the rightmost column, so the reverse-video
   cursor block overwrites the real last glyph there.
2. **Mid-text, cursor sits right at a soft-wrap point.** No synthetic row is
   involved here — the top-of-loop capture at `readline.cpp:361-364` records
   `{row, columns}` before the wrap check at `readline.cpp:393-397` emits the
   `\r\n`. Same clamp-and-overwrite failure, no end-of-text needed to
   trigger it.
3. **Wide glyph (CJK/emoji) landing exactly on the wrap boundary.** Capture
   records `{row, columns-1}`, but the glyph itself paints at `{row+1, 0}`
   after the width-triggered wrap. The cursor block lands one cell short,
   over the *previous* character instead.

All three share one root cause: **the cursor position is captured before the
layout decision for the character at the cursor is made.** The fix is an
invariant, not a special case — see M0 below.

**Wrapping is character-boundary only.** `write_wrapped`
(`readline.cpp:356-409`) wraps mid-word whenever `column + width > columns`,
with no lookahead for whitespace. It does correctly understand `\n` as a
hard row break already (`readline.cpp:374-382`) and already skips ANSI
escapes (`readline.cpp:365-372`, via `ansi_escape_length`,
`readline.cpp:232-243`) since `prompt_` itself carries SGR codes
(`main.cpp:2028`: `"\n\033[1;36m>\033[22;39m "`) — both are reusable, but the
escape-skipping behavior specifically must be preserved by any wrap-planner
replacement, not just the `\n` handling.

**Editing is minimal.** Only Backspace and Left/Right (`[D`/`[C`) touch the
buffer (`readline.cpp:539-546, 721-727`). Up/Down/Home/End/PageUp/PageDown
are all consumed by `control_fn` for **transcript scrolling**
(`readline.cpp:551-574`), not cursor movement. There is **no input history
recall anywhere** — `ControlAction` has exactly six values, all scroll
(`readline.h:15-24`), the only `control_fn` in the codebase is scroll-only
(`main.cpp:1877-1895`), and nothing in `main.cpp` retains submitted lines.
This was an open question in the original draft of this plan; it's settled
now — Up/Down are free to become pure cursor-row movement with no
history-recall fallback to design around.

**Visual chrome.** Fixed `\033[100m` background tint (`readline.cpp:230`),
no border, no leading prompt glyph beyond `prompt_` itself, no
terminal-background query (no OSC 11 anywhere in `core/terminal.cpp`).

**No modal/vim infrastructure** exists anywhere in the codebase today.

**`readline()`'s surrounding plumbing, relevant to every later milestone:**
- Raw mode is entered **per call**, not once per process
  (`readline.cpp:600-611`) — the main loop re-enters it on every submit and
  every mailbox wake (`main.cpp:2020`-area `while (true)`, call at
  `main.cpp:2061`). Any protocol probe (M4's Kitty query, M6's OSC 11 query)
  must be cached process-wide, not re-run per prompt.
- `c_iflag` is untouched by `RawMode::enter` (`readline.cpp:149-152`) — only
  `ECHO|ICANON` are cleared. `ICRNL` is still on, so the line discipline
  still turns CR into NL: Enter and Ctrl+J are **already indistinguishable**
  at the byte level today (both arrive as `0x0A`). `IXON` is also still on,
  so Ctrl+S/Ctrl+Q are eaten by the driver before they ever reach pici.
- Only draft text and cursor position survive across `readline()` calls,
  hand-carried by the caller (`main.cpp:2028-2029, 2075-2076`). Nothing else
  persists — no home for vim mode state, a kill ring, or cached probe
  results.
- `readline()`'s signature is already nine positional parameters
  (`readline.h:79-86`) with exactly one call site.
- In `--render region` mode specifically, the input box has a **fixed
  one-row budget**: `scroll_region_sequence` sets DECSTBM to rows
  `1..height-2` (`region_renderer.cpp:566-570`) and the prompt is anchored at
  `height-1` (`region_renderer.cpp:1231-1235`) so its leading `\n` lands
  exactly on the last row. There is currently no mechanism for the composer
  to claim more than that one row.

## Design decisions this plan is making

1. **Fix the clip bug independently and first**, stated as the invariant
   from the "Confirmed wrap-boundary clip bug" section above, not as a
   narrow end-of-text special case.
2. **Word-wrap needs one-line lookahead, not a rewrite of the whole
   renderer**, plus three things the original draft of this plan missed:
   - The **same invariant from decision 1** must hold for the planner's
     cursor-position output — a cursor offset that coincides with a soft
     break is inherently ambiguous (end of row N vs. start of row N+1)
     unless the planner enforces "always normalize to start of next row."
     Sharing one break-point list between the paint loop and the cursor calc
     removes disagreement about *where* breaks are, but not this — the
     normalization rule has to be applied at whichever single point emits
     the final cursor position.
   - The planner needs a `start_column` input, not just `columns` — the
     prompt paints first and carries `column` into the buffer's own layout
     (`readline.cpp:279-281`), so a planner that assumes the buffer always
     starts at column 0 will misplace every wrap on row 1 by the prompt's
     width.
   - The planner must skip ANSI escapes exactly as `write_wrapped` does
     today (see above) — zero-width sequences must not consume columns or
     become break candidates.
   Per logical line (segment between hard `\n`s): greedy-accumulate
   codepoints until the next whitespace-delimited word would overflow the
   row, break at the last whitespace before that point; if a single token is
   wider than the full row (URLs, paths), hard-break it character-by-
   character as a fallback.
3. **Enter no longer auto-submits once the buffer is multi-line-aware —
   but the submit-key choice needs a termios change, not just a key
   choice.** Because `ICRNL` is on (see above), neither of the two options
   originally floated here — "Ctrl+Enter" or "Ctrl+J" — actually works:
   Ctrl+Enter isn't distinguishable from Enter in any legacy encoding, and
   Ctrl+J arrives as the same byte as Enter while `ICRNL` is set. The real
   choice is: clear `ICRNL` in `RawMode::enter` (then `\r` = Enter, `\n` =
   Ctrl+J, cleanly distinguishable), or pick a genuinely free control byte.
   Clearing `ICRNL` is the smaller change and is assumed going forward;
   flagged for explicit confirmation in Open questions.
4. **Bracketed paste is a prerequisite for M1, not a nice-to-have.** The
   original draft claimed M1 fixes multi-line paste "as a side effect" of
   Enter no longer submitting — that's only true once bracketed paste
   exists, because without DECSET `?2004` a pasted newline is byte-identical
   to a keystroke and would just insert instead of (harmlessly) doing
   nothing paste-related. It's cheap: DECSET/DECRST `?2004` plus recognizing
   `ESC [ 200~` / `ESC [ 201~` in `read_escape_sequence`
   (`readline.cpp:486-535`). It also matters for M5, so pasted text isn't
   parsed as vim commands.
5. **The composer needs an explicit height model — this was entirely
   missing from the original plan.** Two separate constraints, both
   real:
   - `clear_previous` (`readline.cpp:334-354`) erases the box via *relative*
     cursor motion over `rendered_rows_`. That's only valid while every
     rendered row is still on-screen; a draft taller than the terminal (or
     taller than the space below the anchor) causes the terminal to scroll
     out from under the relative-motion bookkeeping, and the box starts
     smearing. A max composer height + internal viewport (matching Codex's
     approach) is needed.
   - In `--render region` mode, the reserved row range is hardcoded to
     `height-2` (see above) — a multi-line draft has nowhere to grow inside
     that region without either shrinking the transcript area dynamically as
     the composer grows, or reserving a fixed max composer height up front.
     This needs a decision (flagged in Open questions) before M1 can be
     considered done for region mode specifically.
6. **Scroll bindings must move before cursor-row navigation can use
   Up/Down.** Proposed remap: transcript scroll moves to Ctrl+Up/Ctrl+Down
   (line) and PageUp/PageDown stays for page scroll; Home/End become
   buffer-line home/end when the cursor is in the textarea, with a modifier
   (e.g. Ctrl+Home/Ctrl+End) for transcript top/bottom. Since there's no
   history recall to preserve a fallback for (see above), this remap is
   simpler than originally scoped — still needs user sign-off since it
   changes muscle memory.
7. **Shift+Enter is a terminal-protocol problem, not a pici problem**, and
   is under-scoped in ways the review pass caught in detail — see M4 below.
   Alt+Enter (legacy `ESC` + key, no negotiation needed) remains the
   universal fallback and should ship before the Kitty-protocol probe.
8. **Vim mode's initial scope is cut down from the original draft.** Full
   `d y c` × five text-object pairs is roughly as much implementation work
   as M1+M2+M3 combined, for an opt-in feature. Initial scope: Normal/Insert
   modes, motions `h j k l w b e 0 $`, operators `d`/`c` only, no text
   objects. Text objects (`iw/aw`, `i(/a(`, `i"/a"`) and the `y` operator are
   a follow-up once the composer has settled in real use.
9. **M3 and M5 share editing primitives — they can't be built in
   parallel.** Word motions, line-boundary detection, and kill/yank all need
   one implementation, not two. M3 defines the primitive API (word-left/
   right, line-start/end, kill-region); M5 consumes it. This changes the
   sequencing from the original draft (see below).
10. **A small pre-M1 refactor pays for itself immediately.** Given the
    per-call raw-mode entry, the growing parameter list, and the need for
    something to hold vim state / a kill ring / cached probe results across
    calls, introduce a `ReadlineOptions` (call-time config) +
    caller-owned `ReadlineState` (persists across calls) pair before M1
    lands, rather than bolting more positional parameters onto
    `readline()` through M1-M5 one at a time.

## Milestones

### M0 — Fix the wrap-boundary clip bug (standalone, ships first) [DONE]
- Fix, stated as an invariant: the recorded cursor position must always
  satisfy `column + width_of_char_at_cursor <= columns` (equivalently
  `column < columns`); any capture that would violate it normalizes to
  `{row+1, 0}`. Concretely: move the printable-character capture in
  `write_wrapped` to *after* the wrap-emit block (`readline.cpp:393-397`),
  keeping a top-of-loop capture only for the escape/`\n`/`\r` branches (which
  legitimately want the pre-wrap column); apply the same normalization to
  the end-of-text tail capture and to `redraw`'s synthetic-row case. As a
  belt-and-braces guard, never emit `\033[<n>C` with `n >= columns` at
  `readline.cpp:296` — that's the line that turns a bad position into
  visible corruption.
- Files: `src/cli/readline.cpp` (`InputRenderer::redraw`,
  `InputRenderer::write_wrapped`).
- Test: no existing input-rendering tests cover this —
  `test/test_readline.cpp` is `forkpty`-driven and asserts only on the raw
  output byte stream, with no terminal-emulator model, and it currently
  passes `nullptr` for `winsize` (`test/test_readline.cpp:174`), which falls
  back to 80 columns via `terminal_columns()` (`readline.cpp:213-220`). A
  regression test needs to (a) pass an explicit `winsize` to `forkpty` and
  (b) assert on emitted escapes directly (e.g. "no `\033[<columns>C` is ever
  emitted with `n >= columns`") rather than on rendered glyphs. Cover all
  three cases from "Confirmed wrap-boundary clip bug" above, not just
  end-of-text.
- Acceptance: no glyph is overwritten by the cursor block when a line
  exactly fills the input width, when the cursor sits mid-text at a soft
  wrap point, or when a wide glyph lands on the wrap boundary — at any
  terminal width.

### M1 — Multi-line buffer model + prerequisites [DONE]

Implemented: `ICRNL` cleared in `RawMode::enter`; Enter still submits,
Alt+Enter (`ESC` + `\r`) inserts a newline; bracketed paste (`?2004h`/`l`,
`ESC[200~`/`ESC[201~`) lands pasted content as an inert block that never
submits; the buffer now carries embedded `\n` directly (no structural
change needed — cursor motion across a `\n` byte already worked once
newlines could reach the buffer); a `core::kMaxComposerRows` (6) height cap
bounds `InputRenderer`'s painted rows via a measure-then-paint viewport
pass, and `region_renderer.cpp` reserves the same fixed row budget instead
of the old 1-row assumption. The `ReadlineOptions`/`ReadlineState`
pre-refactor from design decision 10 was skipped — no concrete M1 consumer
needed it, and building it speculatively ahead of M3/M4/M5 (which do need
cross-call state) would have been premature; `readline()`'s signature is
unchanged. 14 readline tests + 2 region-renderer tests cover this
milestone; each was verified to fail without its corresponding fix before
being confirmed green (same protocol as M0).
This milestone now includes four items the original draft treated as
out-of-scope or missed entirely (design decisions 3-5, 10 above). None of
them are separable from "make Enter insert a newline" without producing a
broken interim state.

- Pre-milestone refactor: `ReadlineOptions` + caller-owned `ReadlineState`
  (design decision 10).
- Clear `ICRNL` in `RawMode::enter`, making `\r` (Enter) and `\n` (Ctrl+J)
  distinguishable. Enter (`\r`) stays bound to submit; Alt+Enter becomes
  the newline-insert binding from this milestone onward (see "Decisions
  resolved").
- Bracketed paste: DECSET/DECRST `?2004`, recognize
  `ESC [ 200~`/`ESC [ 201~` in `read_escape_sequence`, treat pasted content
  as a block insert regardless of embedded `\n`; paste never triggers
  submit, even on a trailing newline (design decision 4; "Decisions
  resolved").
- Replace `std::string buf` / byte `cursor` with a structure that supports
  embedded `\n` and a `{row, col}`-addressable cursor (one `std::string`
  with `\n` separators plus a derived line-offset index is sufficient — no
  rope needed).
- Composer height model: max composer height + internal viewport so
  `clear_previous`'s relative-motion bookkeeping stays valid. For
  `--render region`: reserve a fixed max composer height up front rather
  than shrinking the transcript region dynamically as the draft grows (see
  "Decisions resolved").
- Enter on an empty buffer is a no-op (see "Decisions resolved").
- `write_wrapped`'s existing `\n` handling (`readline.cpp:374-382`) already
  does the right thing once real newlines exist — reuse it, don't rewrite
  it, for this milestone; M2 replaces the per-character wrap decision only.
- Files: `src/cli/readline.cpp`, `src/cli/readline.h` (buffer type,
  `ReadlineOptions`/`ReadlineState`, `readline()` main loop),
  `src/core/region_renderer.cpp` (reserved-row handling).
- Acceptance: can type a multi-line message, move the cursor with
  Left/Right across line boundaries, backspace across a line join, and
  submit via the new explicit key, in both plain and region render modes.
  Multi-line clipboard paste lands intact and doesn't trigger early
  submission. A draft taller than one row doesn't corrupt the display in
  region mode.

### M2 — Word-boundary wrapping
- Implement the lookahead break-point pre-pass described in design
  decision 2, replacing the character-only check in `write_wrapped`
  (`readline.cpp:390-397`), including the `start_column` input and ANSI-
  escape skipping called out there.
- Cursor-position tracking must be computed against the same break-point
  list the paint loop uses, and must apply the M0 invariant at whichever
  single point emits the final cursor position — this is what actually
  prevents an M0-shaped bug from reappearing here, not just sharing the
  break list (design decision 2).
- Files: `src/cli/readline.cpp` — but note the wrap-planner needs to move
  out of the anonymous namespace (`readline.cpp:128, 245`) into its own
  translation unit (e.g. `src/cli/wrap.{h,cpp}`) for it to be unit-testable
  at all, since `test/` links the whole `.cpp` as one TU
  (`CMakeLists.txt:670`).
- Test: unit tests for the wrap-planner directly (width N, various inputs,
  including a prompt-carried `start_column`) — independent of terminal I/O.
- Acceptance: typing a long sentence wraps at spaces, not mid-word; a
  single long unbroken token (e.g. a URL) still wraps without overflowing
  the box; cursor stays visually correct while navigating a wrapped line,
  including right at a soft-wrap boundary.

### M3 — Standard line-editing bindings + key remap + shared primitives
- Decide and implement the scroll-vs-cursor remap from design decision 6.
  No history-recall fallback needs designing around (there is none).
- Add: Home/End (line), Ctrl+A/Ctrl+E, word-left/word-right (Alt+B/Alt+F or
  Ctrl+Left/Ctrl+Right — pick one, confirm terminal compatibility), Ctrl+W
  (delete word back), Ctrl+U (kill to line start), Ctrl+K (kill to line
  end), Ctrl+Y (yank last kill — needs the kill ring living in
  `ReadlineState` from M1), Up/Down for cursor row movement within a
  multi-line draft.
- Implement these as a shared primitive API (word-boundary detection,
  line-start/end, kill-region) rather than inline key handlers — M5
  consumes this directly (design decision 9).
- Files: `src/cli/readline.cpp` (`handle_escape_sequence`, main key loop,
  new primitives), `src/cli/readline.h` (`ControlAction` if scroll bindings
  move — currently `readline.h:15-24`, one consumer at `main.cpp:1877`).
- Acceptance: each binding above works and doesn't regress transcript
  scrolling or tab completion.

### M4 — Shift+Enter via Kitty keyboard protocol
Under-scoped in the original draft; five concrete gaps folded in below.

- Probe once per process (not per `readline()` call — see "surrounding
  plumbing" above), on first raw-mode entry: `CSI > 1 u` followed
  immediately by a DA1 query (`\x1B[c`) as a **sentinel terminator** — since
  effectively every terminal answers DA1, the probe finishes as soon as DA1
  arrives instead of always paying a fixed timeout, and only pathological
  terminals fall through to a bounded timeout at all.
- **Replay any type-ahead the probe's read window swallows.** Buffer
  whatever was read during the probe and feed it back into the normal event
  path afterward — without this, keystrokes typed during startup are lost,
  and stray late probe-reply bytes get misinterpreted as keys (exactly the
  "garbage on screen" failure mode the acceptance criterion is meant to
  rule out).
- **Explicit override for tmux/screen.** tmux only forwards kitty keys with
  `extended-keys on` (3.4+); older tmux and screen may swallow the query or
  answer on the terminal's behalf, producing a false "supported" result
  where Shift+Enter then never actually arrives. Add a config/env override
  to force the fallback path regardless of probe result.
- **Push/pop lifecycle on every exit path, including abnormal ones.**
  `ISIG` is deliberately left enabled (`readline.cpp:130-131`), so Ctrl+C
  can terminate the process while the enhancement flags are still pushed —
  pop (`CSI < u`) needs a cleanup path that runs on that exit too, not just
  the normal one.
- **This changes key decoding throughout `readline.cpp:688-733`, not just
  adds a new sequence to parse.** Under the disambiguate-escape-codes flag,
  bytes the current loop already special-cases (a bare ESC, for one) get
  re-encoded as `CSI u` sequences. Scope this as a change to the whole
  key-decode path, not an addition alongside it.
- If supported: bind Shift+Enter → newline, plain Enter → submit
  (superseding M1's interim explicit-submit binding once the protocol is
  confirmed present). If unsupported: Alt+Enter stays the newline binding,
  M1's explicit-submit key stays as-is — **state this as the permanent
  behavior for non-Kitty terminals**, not an interim state, since it's what
  a large fraction of users will actually get (Open questions).
- Share probe infrastructure with M6's OSC 11 query rather than building
  two ad-hoc terminal-capability probes (design decision below; Codex
  batches its equivalent queries under one shared deadline).
- Files: `src/cli/readline.cpp` (`RawMode::enter`, `read_escape_sequence`,
  `handle_escape_sequence`), new shared probe utility consumed by M6.
- Acceptance: Shift+Enter inserts a newline in a Kitty-protocol-capable
  terminal (e.g. kitty, wezterm, ghostty); behavior degrades cleanly (no
  hang, no garbage on screen, no swallowed type-ahead) in a terminal that
  never replies meaningfully to the probe (plain xterm, Linux VT, tmux
  without `extended-keys`).

### M5 — Vim mode (Normal/Insert + core motions/operators; text objects deferred)
- Depends on M3's shared primitives, not just M1 (design decision 9) —
  sequence as `M1 → M3 → M5`, not in parallel with M3.
- Initial scope (design decision 8): `VimMode::{Normal,Insert}`; motions
  `h j k l w b e 0 $`; operators `d`/`c` only. No text objects, no `y`, no
  registers beyond a single implicit one — these are a follow-up once the
  rest of the composer has been in real use for a while.
- Implementation shape: a `VimEngine` that intercepts key events ahead of
  the plain-editor key loop, toggled per design decision below (Open
  questions), operating on the M1 buffer via M3's primitives.
- Files: new `src/cli/vim_mode.{h,cpp}`, wired into `readline()`'s key
  dispatch.
- Acceptance: can compose and edit a multi-line message using only the
  covered motions/operators; falls back to plain Insert-mode editing for
  anything not covered rather than eating or misinterpreting keys.

### M6 — Visual polish
- Query actual terminal background (OSC 11, `\033]11;?\007`) and
  alpha-blend a tint instead of the fixed `\033[100m`
  (`readline.cpp:230`) — reusing M4's shared probe infrastructure and
  process-wide caching rather than a second ad-hoc probe. Blending requires
  truecolor (`48;2;r;g;b`) output, so this also needs a truecolor-support
  check alongside the background query, not just the query itself.
- Leading prompt glyph (`›` or similar) in place of/alongside `prompt_`.
- Persistent single-line footer hint row below the input showing live
  shortcuts (mirrors the existing `status_line_` row already drawn above
  the input at `readline.cpp:270-276`).
- Files: `src/cli/readline.cpp`, `src/core/terminal.cpp` (OSC 11 query,
  truecolor detection).
- Acceptance: input box tint looks correct on both a light-background and
  dark-background terminal with truecolor support; degrades to the current
  fixed tint on a terminal without it; footer hint doesn't break existing
  row-count bookkeeping (`rendered_rows_`) used by `clear_previous()`.

## Sequencing

```
M0 (independent, ship anytime)
M1 → M2 → M3 → M4
         → M3 → M5
M6 (shares probe infra with M4; can trail it, not fully independent)
```

M1 is the hard dependency for everything except M0. M5 now depends on M3
(shared editing primitives), not just M1 — this is a correction from the
original draft, which had them wrongly parallelizable. M6 shares terminal-
probe infrastructure with M4 and should be sequenced after it, or built
against the same shared probe utility if genuinely run in parallel.

## Decisions resolved

- **M1 submit key: Enter submits, Alt+Enter inserts a newline.** After
  clearing `ICRNL`, `\r` (plain Enter) stays bound to submit — matching
  every other chat TUI's default — and Alt+Enter (legacy `ESC` + `\r`,
  detectable immediately, no protocol negotiation) is the newline-insert
  binding from M1 onward, not gated behind M4. This is also the
  **permanent** fallback for terminals that never pass M4's Kitty-protocol
  probe (tmux without `extended-keys`, plain xterm, Linux VT) — Shift+Enter
  is additive once M4 lands, Alt+Enter keeps working either way.
- **Paste is always inert.** Bracketed-paste content never triggers submit,
  even if it ends in a newline — avoids accidental sends from clipboard
  content that happens to end with a blank line.
- **Region-mode composer height: fixed reservation, not dynamic shrink.**
  The transcript region is sized once at startup/resize accounting for a
  max composer height (e.g. up to 6 rows); the composer scrolls internally
  past that rather than the transcript region resizing on every keystroke
  that changes the draft's row count (which would mean touching DECSTBM
  continuously).
- **Empty-buffer Enter is a no-op**, matching today's behavior
  (`main.cpp:2079-2080` already skips empty submitted lines).

## Open questions (need user decisions before/at listed milestones)

- **M2:** do wrapped continuation rows hang-indent under the prompt, or
  start at column 0?
- **M3:** confirm the scroll-key remap (Ctrl+Up/Down for transcript scroll)
  is acceptable before implementing — it changes existing muscle memory.
  (No history-recall fallback to design around — confirmed absent from the
  codebase.)
- **M5:** is vim mode opt-in via config, a runtime toggle (e.g. `Ctrl+V` or
  a `:` style command), or always-on with an Insert-mode default?

## Non-goals

- No border/box-drawing chrome — both pici today and Codex read as
  "modern" without one; not worth the added complexity.
- No full Vim parity (no `:` command mode, no marks/registers beyond a
  single yank buffer, no visual mode, text objects deferred past initial
  M5 scope) — see design decision 8.
- No rope data structure or other exotic buffer implementation — pici's
  input sizes don't warrant it.

## Review findings folded in

This plan was reviewed by a second pass (Opus) after the initial draft. Its
verdict: the M0 diagnosis was sound but narrower than reality (folded in
above as three cases sharing one invariant, not one special case); M1 as
originally drafted was not implementable as-is, missing four real blockers
(region-renderer row reservation, composer height cap, bracketed paste,
`ICRNL`) that are now folded into M1 directly; M5's dependency on M3 was
mis-scoped as parallel when the two need shared primitives; M4 was missing
a probe sentinel, type-ahead replay, a tmux/screen escape hatch, and
explicit push/pop lifecycle handling — all folded in above. M5's scope was
also flagged as oversized for an opt-in feature relative to M1-M3 combined;
scope was cut accordingly (design decision 8).
