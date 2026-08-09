# hand-agent — a terminal an AI agent can drive

`hand-agent` is a headless, GPU-free build of the `toe` terminal engine that
speaks **line-delimited JSON on stdio**. It gives an AI agent (or an MCP shim in
front of it) a *real* terminal: a genuine PTY so `vim`/`htop`/`less` behave as
they do for a human, a settled 2-D screen it can read as clean text, structured
OSC 133 command blocks, and deterministic waits.

It exists because an agent's built-in shell tool runs each command against a
**pipe**, fire-and-forget: no TTY, no interactivity, and it hands back raw ANSI
bytes (mostly noise). `hand-agent` gives a *settled, structured, token-frugal*
view instead. See `docs/AI_TERMINAL.md` for the why.

## Run

```sh
hand-agent [--cols N] [--rows N] [-- CMD ARGS...]
# default: 80x24 running $SHELL
```

One JSON request per line in on stdin; one JSON reply per line out on stdout.
Every reply has `"ok":true|false`; errors carry `"error"`.

## Protocol

| Request | Reply | Notes |
|---------|-------|-------|
| `{"op":"snapshot"}` | `{"ok":true,"text":"..."}` | Visible screen as clean UTF-8, trailing blanks trimmed. The token-frugal default read. |
| `{"op":"blocks","last":N}` | `{"ok":true,"blocks":[...]}` | OSC 133 command blocks (see below). `last` limits to the newest N; omit for all. |
| `{"op":"send","text":"ls\r"}` | `{"ok":true,"sent":true}` | Type literal text (include `\r` to submit). |
| `{"op":"send","keys":"<C-c>"}` | `{"ok":true,"sent":true}` | Vim-notation keys (see below). |
| `{"op":"key","name":"Enter"}` | `{"ok":true,"sent":true}` | One named special key. |
| `{"op":"wait","for":"idle","ms":2000,"quiet":120}` | `{"ok":true,"idle":true}` | Wait until output is quiet for `quiet` ms (respecting DEC 2026 sync), capped at `ms`. |
| `{"op":"wait","for":"pattern","re":"\\$ ","ms":5000}` | `{"ok":true,"matched":true\|false}` | Wait until the snapshot contains the substring `re`. |
| `{"op":"resize","cols":120,"rows":40}` | `{"ok":true,"resized":true}` | Resize the grid + PTY (`SIGWINCH`). |
| `{"op":"close"}` | `{"ok":true,"bye":true}` | Shut down. |

When the child exits, the next reply is `{"ok":true,"exited":true}` and the
process ends.

### Command block shape

Requires the shell to emit OSC 133 marks (shell integration). Each block:

```json
{ "id": 1, "command": "echo hi", "output": "hi",
  "cwd": "file:///tmp", "exitCode": 0, "finished": true, "durationMs": 4 }
```

`exitCode` is `null` while running; `finished` is `false` until the `D` mark.

### Vim-notation keys

`<CR>`/`<Enter>`, `<Esc>`, `<Tab>`, `<BS>`, `<Space>`, `<Up>`/`<Down>`/`<Left>`/
`<Right>` (auto DECCKM-aware), `<Home>`/`<End>`, `<PageUp>`/`<PageDown>`,
`<Delete>`, `<Insert>`, and Ctrl chords `<C-c>`, `<C-d>`, `<C-[>` … Text outside
`<...>` is sent literally.

## Example

```sh
printf '%s\n' \
  '{"op":"wait","for":"pattern","re":"$","ms":2000}' \
  '{"op":"send","text":"echo hello\r"}' \
  '{"op":"wait","for":"idle","ms":2000,"quiet":150}' \
  '{"op":"snapshot"}' \
  '{"op":"close"}' | hand-agent -- /bin/sh
```

```json
{"ok":true,"ready":true,"cols":80,"rows":24}
{"ok":true,"matched":true}
{"ok":true,"sent":true}
{"ok":true,"idle":true}
{"ok":true,"text":"sh-3.2$ echo hello\nhello\nsh-3.2$"}
{"ok":true,"bye":true}
```

## Notes

- **Settled reads:** `wait for:idle` treats the screen as unsettled while an app
  holds a DEC 2026 synchronized-output frame open, so a snapshot never captures a
  torn, half-drawn frame.
- **In-process, fast:** the whole VT model is `toe` — no headless-xterm scraping,
  no re-serializing the buffer per read.
- **DEC 2034** (the in-band JSON Semantic Block Query) is also available directly
  to programs running *inside* the terminal, independent of this stdio protocol.
