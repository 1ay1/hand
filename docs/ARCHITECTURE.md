# hand — threading & communication architecture

The whole design rests on one rule: **threads share no mutable state; they
communicate only by moving owned message VALUES through typed mailboxes.** If the
only thing crossing a thread boundary is a value posted to a queue, there is no
data race to guard — the architecture forbids it, not a mutex sprinkled at call
sites. This is the Elm/actor model made concrete for C++ threads.

## Threads

- **GUI thread (main)** — owns the window + the single GL context. It is the
  ONLY thread that renders. It runs the event loop in `run_tabbed()`.
- **N actor threads (one per tab)** — each runs a `TabActor` that owns its tab's
  `toe::Terminal` (PTY + parser). It NEVER touches GL or the renderer.

The one object shared between a tab's actor and the GUI is that tab's `Screen`,
read by the GUI at render and written by the actor at parse. It is guarded by
that tab's own `std::mutex` (`render_lock`) — held only for the microseconds of a
pump or a render, per-tab (never across tabs), so contention is nil.

## Channels (typed, fd-backed mailboxes — MPSC)

    GUI  --TabCmd-->  actor      (TabInput / TabKey / TabStop)
    actor --GuiMsg--> GUI        (TabOutput / TabExited / TabTitleChanged / ...)

Each `Mailbox<Msg>` carries an eventfd (`wait_fd()`) the OWNER folds into its
poll set, so it blocks on "window OR a message" in ONE syscall — no busy-wait, no
condvar timing games.

## The GUI loop (one blocking wait, zero idle CPU)

    loop:
      poll_events(window)     -> chords / overlays / EventRouter -> Session
      pump()                  -> drain GUI mailbox -> gui_update -> interpret
      if take_present_request(): present            (interaction repaint)
      autoscroll step         (if a drag is past the edge)
      if a setting changed:   fan out apply() to all tabs
      if an animation is due: set_frame + present    (time-derived, direct)
      wait_readable({window fd, mailbox fd}, deadline)

The deadline comes from the **Animator**, the single animation-timing authority:

- a caret glide / spinning "running" glyph / bell fade / drag-autoscroll -> 16ms (60fps)
- a slow blink (steady cursor blink, or a done-attention *pulse*) -> its period (~64-530ms)
- nothing animating -> **-1 (block forever)**: the loop sleeps until real input
  or child output. Idle is ~1 iter/sec, 0 presents, 0% CPU.

**Nothing repaints on a timer.** A frame happens only for: real input, new child
output (mailbox wake), or an in-flight animation. `FrameGate` folds
generation/scroll/size/overlay/cursor/tab/interaction/anim-frame into one key and
skips the GPU when the key is unchanged.

## Why input isn't laggy

- Keystroke: window fd wakes the loop immediately; the EventRouter writes to the
  PTY under the tab's `render_lock` (held per-pass by the actor, so a flood can't
  starve it).
- Echo: the child echoes -> the actor's PTY fd wakes -> it pumps the grid and
  posts `TabOutput` (coalesced to ~1 per 8ms so a flood can't drown the GUI, but
  a coalesced change is never DROPPED — the actor wakes at the exact deadline to
  flush it) -> mailbox eventfd -> the GUI wakes THIS iteration and presents.

## Why it stays responsive under flood

- The actor drains its PTY to empty per wakeup and holds `render_lock` PER-PASS,
  not per-drain, so the GUI acquires it between passes (input never blocks behind
  a 256-line drain).
- Output notifications are rate-limited to ~125/s, so a runaway child can't peg a
  core with mailbox traffic; presents stay vsync-bounded.

## Invariants (structural, not conventions)

- Only the GUI thread touches GL / the renderer. Resize runs in `present`, on the
  GUI thread — it is intentionally NOT an actor command.
- Actors only ever call `pump_output`, `send_key`, `send_text`, `poll`, and
  status reads on their Session, all under `render_lock`.
- The pure `GuiModel` holds no thread and no Terminal; the runtime owns the
  `TabActor`s keyed by `TabId` and interprets `GuiCmd`s into effects.
