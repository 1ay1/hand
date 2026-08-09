# Terminals for AI — deep research & what toe/hand should build

> State of the "terminal for AI" space (late 2025 / 2026) and a concrete,
> leverage-ordered plan for toe (the embeddable VT engine) + hand (the GPU
> terminal). Sources at the bottom.

---

## 0. The one-paragraph thesis

A raw terminal is **write-only and lossy** for an AI agent: you can push bytes at
it, but you can't cleanly ask "what command ran, what did it output, did it
succeed, what's on screen *now*, has the UI finished redrawing?" Every agent
tool in the wild rebuilds a headless VT emulator to recover that structure — and
then drowns in tokens because raw CLI output is ~**89% noise**. The winning
"terminal for AI" is the one that (a) **maintains the 2-D screen + command
structure natively**, (b) **exposes it as clean, minimal, structured state** an
agent can query, and (c) **lets an agent drive interactive/full-screen programs**
deterministically (type, wait-until-settled, assert). `toe` is *already* the
hard part (a). The opportunity is (b) and (c).

---

## 1. There are TWO products called "terminal for AI"

| | Agent HOST | Agent-readable EMULATOR |
|---|---|---|
| **What** | An app that embeds agents (chat, blocks, review, orchestration) | A terminal an *external* agent can read & drive |
| **Examples** | Warp 2.0 (ADE), jetem | terminal-driver-mcp, agent-tui, tui-use, headless-terminal, Terminal Use |
| **Layer** | Application / product | Emulator / protocol |
| **hand fit** | Large, crowded, mostly not emulator work | **This is toe/hand's lane** |

Almost all the *hard, reusable, defensible* engineering lives in the second
column, and it's exactly where a fast, embeddable VT engine wins.

---

## 2. What the ecosystem actually built (the reference design)

The clearest single artifact is **terminal-driver-mcp** — effectively
*"Playwright for the terminal."* It is the de-facto spec of what an agent needs.
Its tool surface, distilled:

- **Persistent PTY sessions** (survive across tool calls) — so the agent can
  answer a password prompt, sit in a REPL/debugger, do `git rebase -i`. The
  built-in "run against a pipe, return on exit" model **cannot** do this.
- **Real TTY** — so `vim`/`htop`/`less` don't detect a pipe and hang/degrade;
  colors, redraws, `SIGWINCH` all work.
- **Read the 2-D screen as clean text** (not the ANSI byte log) — plus a
  **structured JSON cell model** (fg/bg, bold/italic/underline, cursor, OSC 8
  links) and a **region** extractor (grab just the status bar / a pane).
- **PNG screenshot** of the rendered screen for **vision models**.
- **Input**: text, special keys (DECCKM-aware arrows → SS3 in app mode),
  control chords (C0 legacy byte, else CSI-u/kitty), `raw_hex`, **bracketed
  paste** (atomic multi-line), **mouse** click/drag/wheel as SGR (only when the
  app enabled mouse tracking).
- **Synchronization — the crux**: `wait until = pattern | pattern_gone | idle |
  stable_screen | exit`. Waits return a **settled frame** (after match, wait for
  ~quiet, cap 500ms so animations can't stall), never a torn mid-render frame.
- **Frame-atomic reads via DECSET 2026 (synchronized output)**: while a modern
  TUI (ratatui/notcurses/textual) holds a frame open, reads block until commit
  (cap 250ms; expire a stuck frame >1s). This is the *clean* answer to "has it
  settled" vs. CPU heuristics.
- **OSC 133 command boundaries**: `wait_command` returns `{command, exit_code,
  duration_ms, output}` — **only that command's output**, "far cheaper in
  tokens." Typed via MCP `outputSchema` + `structuredContent`.
- **Assertions** (contains/absent/count/at/matches, retry-until `within_ms`) +
  **golden-screen snapshots** with volatile-region masks.
- **The emulator answers DA1/DSR/etc. on the app's behalf** so query-happy TUIs
  (neovim) don't hang probing.
- **Records every session to asciicast v2**; converts a recording → replayable
  deterministic test (no LLM in the loop, runs in CI with JUnit/HTML trace).

**agent-tui** frames the same need as *"Terminal Apps need a DOM"* — expose the
screen as text **or an outline with stable element refs** so the model can say
"click block 3 / the OK button" deterministically.

**Takeaway:** the required feature set is now well-defined and *converged*. toe
already owns the emulator core; the rest is an API surface + a few protocols.

---

## 3. Token economics reframes the whole design (the non-obvious insight)

This is the part most "AI terminals" miss and it should drive toe/hand's API:

- Measured over ~2,900 CLI commands: **10.3M of 11.6M input tokens (89.2%) were
  noise.** A 2-hour session ≈ 210K tokens of raw output — enough to blow a 200K
  context window with garbage. (~$1,750/mo/10-devs wasted.)
- **Vision agents used ~45× more tokens** than an API/text path in one benchmark
  and were far slower (~17 min vs ~20 s). A raw retina screenshot ≈ **~2,100
  tokens** and often hits server-side downscale caps.

**Design consequences for toe/hand:**
1. **Text-first, structured, minimal.** Default to *clean text of the relevant
   region* (or just the last command's output via OSC 133), not the whole
   scrollback, not a screenshot.
2. **Screenshots are a last resort**, offered but never the default; when used,
   render *tight* (region-only, low-DPI, bundled mono font) to stay token-frugal.
3. **Diff/delta reads**: "what changed since my last read" beats re-sending the
   whole grid. toe already tracks **damage rects** — expose them as the read
   primitive.
4. **Semantic slicing**: "just the error", "just the table", "just this pane"
   (region + OSC 133 + color-run model) instead of dumping everything.

toe's damage tracking + Screen model make (3) and (4) nearly free — that's a
genuine differentiator over xterm.js-headless wrappers that re-serialize the
whole buffer every read.

---

## 4. Competitive / architectural landscape

- **libghostty-vt** (Mitchell Hashimoto, announced Sep 2025): a *zero-dependency,
  no-libc, embeddable* VT parse+state library extracted from Ghostty, explicitly
  meant to be the reusable core for emulators, multiplexers, **and headless
  agent/automation consumers**. C API still "coming shortly / early testing." It
  *fully exposes the kitty graphics protocol* — claimed first native embeddable
  to do so.
  → **This is direct validation of toe's architecture and a competitor.** toe is
  C++ (broader reach than Zig), already split from the host (hand), already has
  kitty graphics + sixel, and is fast. The window to be "the embeddable VT engine
  with a first-class agent API" is *open right now* because libghostty's C API
  and agent story aren't shipped yet.
- **Warp 2.0**: repositioned as an "Agentic Development Environment" — command
  **blocks**, agent mode, MCP, cloud orchestrator (Oz), vertical tabs w/
  git/worktree/PR metadata, **secret redaction**. App-layer, but the block model
  + redaction are worth stealing at the emulator layer.
- **kitty / Ghostty / WezTerm / iTerm2 / VS Code**: the OSC 133 + kitty-graphics
  baseline every agent bridge rides on. Table stakes, not differentiation.

---

## 5. The protocols that matter (implement these in toe)

1. **OSC 133 semantic prompts** — `A` prompt / `B` input / `C` output / `D`
   finished(+exit code); `;C` carries the command line. Turns the byte stream
   into **command blocks**. Foundation for everything.
2. **OSC 7** — cwd per block (context).
3. **DECSET 2026 synchronized output** — atomic frames; *the* clean "settled"
   signal. toe's run loop already has begin/end frame + damage; wire the mode.
4. **DEC 2034 Semantic Block Query** (Contour) — a *machine-readable JSON query*
   for command blocks, **designed for AI agents**: enable → session token →
   `CSI > 1 ; N … b` → `DCS > 1 b {"blocks":[{command,output,exitCode,cwd,…}]}
   ST`. If toe implements this, *any* agent can ask the terminal directly, in a
   standard nobody had to invent, with zero scraping.
5. **kitty graphics + sixel** — ✅ already in toe. Lets agents *show* plots and
   lets vision models read what a TUI drew.
6. **kitty keyboard protocol / CSI-u** — unambiguous key input from agents.
7. **DA1/DSR auto-answers** — so query-happy TUIs don't hang under automation.

---

## 6. What toe/hand should build — leverage order

### Phase 1 — structured, queryable state (the moat, all in `toe`)
1. **OSC 133 zone tracking** in `toe::term::Screen`: per-row semantic flags +
   a ring of completed **command blocks** {command, cwd(OSC 7), exit, start/end,
   absolute rows}.
2. **DEC 2034 SBQUERY** on top of that ring (token handshake + JSON DCS reply).
3. **DECSET 2026** synchronized-output mode → expose a **"frame settled"**
   signal (the thing every bridge fakes with sleeps).
4. **Damage-delta read**: an API/escape that returns *only changed cells* since a
   cursor token — the token-frugal read primitive.

### Phase 2 — the agent-driver surface (headless daemon over the offscreen backend)
5. `TOE_HEADLESS=1` already runs the full engine windowless. Add a **local
   driver API** (unix socket + stdio) and ship it as an **MCP server**, matching
   the converged tool set:
   - `snapshot(format = text | json-cells | outline-with-refs | region)`
   - `blocks()` (SBQUERY data) · `wait(pattern|pattern_gone|idle|stable|exit)`
   - `send_keys` (vim-notation, DECCKM-aware, CSI-u, bracketed paste, mouse SGR)
   - `screenshot(region?, dpi?)` — PNG via the renderer, offscreen readback,
     **tight/low-DPI by default** (token budget)
   - `wait_command()` → typed `{command, exit_code, duration_ms, output}`
   - `assert(...)`, `resize`, `region`, `session_*` lifecycle
6. **Settled frames everywhere** — reads/waits return a repainted frame, never a
   torn one (use 2026 when present, damage-quiet heuristic otherwise).
7. **asciicast v2 recording** + **recording → deterministic replay test** (CI).

### Phase 3 — safety & token hygiene
8. **Secret redaction** on block/screen text *before* it leaves the API
   (Warp-style regex + entropy). Agent never sees the token you just `export`ed.
9. **Command policy hooks**: deny-list / approval-gate for keystrokes the agent
   may inject; mark blocks the agent may not read.
10. **Token-frugal defaults**: last-command-output over full-screen; region over
    whole grid; delta over full; text over screenshot.

### Phase 4 — human+agent shared surface (hand GUI, cheap once Phase 1 exists)
11. Block UI (fold/jump/"copy last output"), "an agent is driving" affordance,
    inline diffs, approve/deny for agent input — all the same OSC 133 data.

---

## 7. The pitch, one line

> **toe** = the fast, embeddable, C++ VT engine that is *natively agent-readable*
> — OSC 133 blocks + DEC 2034 JSON query + frame-settled + damage-delta reads,
> plus a headless MCP driver — so an agent reads and drives any terminal program
> with clean, minimal, structured state instead of scraping ANSI garbage.
> **hand** = the GPU terminal built on it (and the human's block UI).

This beats the xterm.js-headless MCP wrappers on **speed + token efficiency +
native structure**, and gets to "first embeddable VT with a real agent API"
before libghostty's C API/agent story ships.

---

## Sources
- terminal-driver-mcp (Playwright-for-terminal MCP; the converged spec) — github.com/funkyfunc/terminal-driver-mcp
- agent-tui "Terminal Apps need a DOM" — github.com/pproenca/agent-tui
- tui-use / headless-terminal / Terminal Use (PTY bridges, settled detection)
- OSC 133 semantic prompts — terminfo.dev/extensions/osc-133-semantic-prompts ; contour-terminal.org/vt-extensions/osc-133-shell-integration/
- DEC 2034 Semantic Block Query — contour-terminal.org/vt-extensions/semantic-block-query/
- DECSET 2026 synchronized output — (VT extension; ratatui/notcurses/textual emit it)
- libghostty-vt roadmap — mitchellh.com/writing/libghostty-is-coming ; kitty-graphics: x.com/mitchellh/status/2041253090205249584
- Token economics: "Stop Wasting 89% of Your AI Agent's Tokens on CLI Noise" — alies.dev/articles/cli-output-for-ai/ ; "AI vision agents use 45x more tokens" — theregister.com (2026-05-07) ; tinyscreenshot (~2100 tok/retina shot) — github.com/franzenzenhofer/tinyscreenshot
- Warp 2.0 ADE — warp.dev/terminal
- Sandboxing agent shell access — NVIDIA dev blog; "Approval Gates, Deny-Lists, and Sandboxes"
