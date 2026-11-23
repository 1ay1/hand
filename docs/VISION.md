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
   checkpoints the tree. "Revert last turn" restores it in one step.

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

1. **Agent awareness**: detect agent tabs; working / needs-you / done state on
   the tab; OSC 9/777 + bell; system notification when unfocused.
   Shots: `agents`. Test: state transitions from scripted output.
2. **Rendering under agents**: DEC 2026, kitty keyboard (shift+enter), paste,
   no tearing at full redraw rate; stress with a real `claude` session.
3. **Baseline and changes list**: per-tab baseline, live changed-file count,
   local and remote. Test: real-ssh suite covers remote baseline.
4. **Review split**: `⌘⇧D` diff editor beside the agent, hunk walk, keep /
   revert, live reload with touched-line glow. Shots: `review`, `review-ssh`.
5. **Steer**: `⌘L` send selection, ⌘-click `path:line`.
6. **Checkpoints**: per-turn snapshots and "revert last turn".
7. **Remote persistence**: optional tmux reattach for agents on servers.

Every step is judged by one question: does a plain terminal user notice
nothing until they want it, and does an agent user never have to leave hand?
