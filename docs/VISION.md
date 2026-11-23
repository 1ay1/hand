# hand: a terminal with an agentic editor

**hand is a terminal.** It opens like foot: one window, one shell, no chrome,
instant. Built into it is an editor whose job is to **watch, review and steer
the work of the coding agents you run in the terminal** (Claude Code, Codex,
Aider, Gemini, opencode, …), locally or on any SSH host.

| | Primary surface | Where the AI lives | Example |
|---|---|---|---|
| Editor with a terminal | the editor | a chat panel in the editor | VS Code, Cursor, Zed |
| Agentic terminal | the terminal | AI built into the shell | Warp |
| **Terminal with an agentic editor** | **the terminal** | **in the agents you already run; the editor reviews their work** | **hand** |

hand itself never calls a model, holds an API key or has a chat box. The
agent is any TUI. What hand adds is everything *around* it: knowing what the
agent is doing, seeing what it changed, and fixing or rejecting that change
without leaving the terminal.

## Who it's for

Developers who live in the terminal and run coding agents, often several at
once and often on remote boxes (dev servers, GPU machines, prod-adjacent
hosts). hand is open source and built to be loved by them, not merely used.

Its edge, in one line: **terminal-native, any agent, local and remote, nothing
to install.** Editors (Zed, Cursor, VS Code) host agents inside an editor;
agent vendors build review into their own tool; multi-agent runners assume
local worktrees. Nobody makes the terminal itself the place where you run,
watch and review every agent on every machine.

## v1 is one demo

> `ssh gpu-box`, start three agents in three tabs. One turns ◆: it needs
> you. Answer it. Another finishes: `⌘⇧D`, walk its diff, revert one hunk,
> select a function and `⌘L` "don't touch this". Back to the shell with
> `Esc`. Thirty seconds, no setup, nothing installed on the box.

Everything that serves this demo ships. Everything else waits.

## What makes developers fall in love

- **It is a great terminal first.** Matches Ghostty/kitty on latency, frame
  times, font rendering, IME and TUI correctness, measured side by side with
  a real `claude` session. Nobody discovers the editor in a terminal that
  feels off.
- **Zero setup.** No account, no server install, no config to get started;
  agent support lights up when an agent runs. One config file (`hand.vibe`)
  and a GUI for every setting.
- **Fast everywhere.** Instant launch, no frame drops under agent redraws,
  remote actions in tens of milliseconds over the shared connection.
- **Honest and safe.** hand never overwrites your unsaved edits, never
  phones home, marks prod hosts in red, and every revert is undoable.
- **Beautiful and quiet.** Restrained, precise UI; nothing on screen that
  isn't earning its place.
- **Hackable.** Small, readable C++ with self-tests and screenshot scenes;
  easy to build, easy to contribute to.

## How hand knows what an agent is doing

Reliable signals first, guesses last:

1. **Agent hooks.** Claude Code `Notification`/`Stop` hooks, Codex `notify`,
   and similar: hand ships a tiny helper (`hand notify …`, which writes an
   OSC sequence to its own tty) and offers to add it to the agent's config.
   Works over SSH because the OSC travels down the same pty.
2. **Terminal signals.** OSC 9 / OSC 777 notifications, BEL, title changes.
3. **Heuristics.** Output flowing = working; quiet at a prompt = needs you.

## Baselines with parallel agents

A baseline belongs to a working tree, not a tab. Each agent tab records its
tree (`git rev-parse --show-toplevel`, so worktrees are separate trees).
Two agents in the same tree share one baseline and hand says so on both
tabs, suggesting `git worktree add`. Outside git, the baseline is file
mtimes under the tab's directory.

## The loop

```
 run agent in a tab ──► hand sees it's an agent, takes a baseline
        ▲                         │
        │                 agent writes files ──► changes list fills live
        │                         │
 send feedback (⌘L)  ◄── review: diff per hunk, keep / revert / edit
```

1. **Run.** `claude` in a tab, local or after `ssh box`. hand recognises the
   foreground process as an agent (configurable list) and snapshots the
   working tree as the tab's baseline (git, or file mtimes outside a repo).
2. **Watch.** The tab shows the agent's state: ● working (output flowing),
   ◆ needs you (bell, OSC 9/777, or idle at a prompt), ✓ done. When hand is
   not focused, "needs you" is a system notification. A small count on the
   tab says how many files it changed.
3. **Review.** `⌘⇧D` opens the editor as a split beside the agent, on the
   tab's machine: the changed files, each as a diff against the baseline.
   `]c`/`[c` walk hunks; keep, revert (writes the file back), or just edit.
   Files you already have open reload as the agent writes them, the touched
   lines glowing briefly; your unsaved edits are never overwritten silently.
4. **Steer.** Select lines (or a hunk) and `⌘L`: hand pastes
   `path:12-30` plus the snippet into the agent's prompt as a bracketed
   paste. ⌘-click any `path:line` the agent prints to open it at that line.
5. **Undo a turn.** Each time the agent goes back to "needs you", hand
   checkpoints the tree (a git stash-style commit object, never touching
   your branch). "Revert last turn" restores it in one step. Agents with
   their own rewind (Claude Code) keep theirs; hand's works for every agent
   and for edits made outside the agent.

`Esc` / `⌘W` always returns to the plain terminal. None of this is visible
until an agent runs or you call it.

## Remote is the same

Every step above works on an SSH host through the shared connection
(`hand::remote`): baseline, diffs and checkpoints are `git` commands over the
ControlMaster, files read and save through the same path as ⌘P. Nothing is
installed on the server. A dropped link reconnects on its own, and with
`ssh.tmux` the agent keeps running on the server and is reattached.

## What hand will not do

- No built-in LLM, chat panel, API keys, AI autocomplete or "explain this".
- No language server, debugger or extension system. The editor is for
  reading diffs and quick fixes, not for living in.
- No agent framework or orchestration DSL. Agents are just programs in tabs.
- No chrome by default. A plain launch must stay as simple and fast as foot.

## Order of work

1. **Terminal bar**: side-by-side with Ghostty on a real `claude` session —
   input latency, frame times, DEC 2026, kitty keyboard (shift+enter), paste,
   IME. Fix every loss before building review features.
2. **Agent awareness**: detect agent tabs; working / needs-you / done state on
   the tab from hooks, OSC 9/777, bell, then heuristics; system notification
   when unfocused; `hand notify` helper. Shots: `agents`. Test: state
   transitions from scripted output.
3. **Baseline and changes list**: per-tree baseline (worktree-aware), live
   changed-file count, shared-tree warning, local and remote. Test: real-ssh
   suite covers remote baseline.
4. **Review split**: `⌘⇧D` diff editor beside the agent, hunk walk, keep /
   revert, live reload with touched-line glow. Shots: `review`, `review-ssh`.
5. **Steer**: `⌘L` send selection, ⌘-click `path:line`.
6. **Checkpoints**: per-turn snapshots and "revert last turn".
7. **Remote persistence**: optional tmux reattach for agents on servers.
8. **Launch**: README with the demo as a GIF, one-line install (brew, AUR,
   release binaries for macOS / Linux / Windows), contributor guide.

Every step is judged by two questions: does a plain terminal user notice
nothing until they want it, and does an agent user never have to leave hand?
