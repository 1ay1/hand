// SPDX-License-Identifier: LGPL-2.0-or-later
//
// App<S> — the frame loop, MONOMORPHIC over the concrete surface type S.
//
// hand is deliberately thin: the engine (toe) owns everything from PTY to
// pixels; input policy lives in EventRouter<S>; config parsing lives in
// config.*; time/blink and the poll set are their own small value types. What
// remains here is the *shape of a frame*, expressed as a sequence of named
// steps.
//
// Everything is compile-time dispatched. `S` is a concrete `platform::Surface`
// model chosen once at startup (see dispatch.cpp); there is no AnySurface, no
// vtable, no std::function in the hot loop. Every surface call inlines and
// optional surface refinements (title, partial damage, key-repeat fd) resolve
// through the `platform::` `if constexpr` shims — folding to nothing on
// backends that don't provide them.

#ifndef HAND_APP_LOOP_HPP
#define HAND_APP_LOOP_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

#include <epoxy/gl.h>

#include "toe/gfx/render_target.hpp"
#include "toe/terminal.hpp"

#include "hand/app/blink.hpp"
#include "hand/app/event_router.hpp"
#include "hand/app/poll_set.hpp"
#include "hand/platform/surface.hpp"

namespace hand {

// How long we wait for the child's echo before rendering a keystroke, and the
// idle cursor-blink cadence. Named so the frame loop reads as intent.
inline constexpr int kEchoWaitMs = 3;    // local shells echo in µs; this is a ceiling
inline constexpr int kIdlePollMs = 250;  // keeps cursor blink crisp when nothing else wakes us
inline constexpr int kMinAnimMs = 16;    // don't spin faster than ~60fps on animations
inline constexpr int kFloodPresentMs = 33; // ~30Hz present cap while flooding
inline constexpr int kCoalesceWaitMs = 2;  // brief refill window to coalesce a bursty stream
inline constexpr int kCoalesceRounds = 8;  // cap the coalesce so render/input never starve

// The render gate: a terminal frame is drawn only when something the user can
// see has changed. That "something" is fully captured by the damage generation
// plus the current blink state — so equality of this value across frames means
// "nothing to redraw". Comparing whole RenderKeys replaces ad-hoc last_*
// variables with one total comparison.
struct RenderKey {
    std::uint64_t generation{0};
    BlinkState blink{};
    constexpr auto operator<=>(const RenderKey &) const = default;
};

// Compute the poll timeout for an idle wait: the blink cadence, tightened to an
// inline-image animation's next-frame deadline when one is playing.
[[nodiscard]] inline Timeout idle_timeout(const toe::Session &s, Millis now) noexcept {
    if (const std::uint64_t deadline = s.next_animation_deadline(); deadline != 0) {
        const std::int64_t wait =
            static_cast<std::int64_t>(deadline) - static_cast<std::int64_t>(now.value);
        const int clamped =
            static_cast<int>(std::clamp<std::int64_t>(wait, kMinAnimMs, kIdlePollMs));
        return Timeout::millis(clamped);
    }
    return Timeout::millis(kIdlePollMs);
}

// The application: a terminal driven over one concrete surface. Construct with
// an already-open surface (GL context current) and the live Terminal, then
// run() until the child exits or the window closes. Returns the child's exit
// code.
template <pf::Surface S>
class App {
public:
    App(S &surface, toe::Terminal &term, toe::PixelSize px) noexcept
        : surf_(surface), term_(term), px_(px) {}

    [[nodiscard]] int run() {
        bool running = true;
        std::string last_title;
        std::optional<RenderKey> drawn;    // key of the last rendered frame
        std::uint64_t last_present_ms = 0; // for the flood frame-rate cap

        while (running && !surf_.should_close()) {
            // 1. The lifecycle's only transition: Running -> (Running | Exited).
            //    A dead terminal has no Session — return its exit code and stop.
            const toe::Terminal::Poll p = term_.poll();
            if (p.exited) return p.exited->code;
            toe::Session &session = *p.running;

            // 2. Route window events -> Session actions via the exhaustive
            //    visitor. It reports whether any event handed bytes to the child.
            EventRouter<S> router{session, surf_, px_, running};
            surf_.poll_events([&](const pf::Event &ev) { std::visit(router, ev); });

            // 2b. Drain child output HERE, decoupled from the window event
            //     cadence. poll_events fires a ChildOutput at most once per
            //     dispatch, so under a bursty stream relying on it alone paces
            //     throughput to one gulp per event-loop turn — each turn paying a
            //     compositor roundtrip + a sleep/wake (≈0 CPU, huge wall time).
            //     Instead, once the PTY fd is readable, drain in a tight loop
            //     until it's dry (drain() yields at a byte budget so input/render
            //     still get a turn under a real flood), coalescing a couple ms so
            //     a bursty producer's writes merge into one drain. Throughput is
            //     PTY/model-bound, not event-loop-bound.
            bool child_gone = false;
            {
                PollSet ready;
                ready.add(session.pty_fd());
                ready.wait(Timeout::millis(0)); // non-blocking probe
                int coalesce_budget = kCoalesceRounds;
                while (ready.ready(session.pty_fd())) {
                    if (!session.pump_output()) { child_gone = true; break; }
                    if (session.output_pending()) break;   // hit the byte budget: yield
                    if (--coalesce_budget <= 0) break;     // yield to render/input
                    ready.wait(Timeout::millis(kCoalesceWaitMs));
                }
            }

            // 3. Zero-latency local echo: after sending input, flush and give the
            //    PTY a few ms to echo, draining what returns so the typed glyph
            //    lands in THIS frame instead of a vsync later.
            if (router.take_wrote_input()) {
                pf::flush(surf_);
                PollSet echo;
                echo.add(session.pty_fd());
                echo.wait(Timeout::millis(kEchoWaitMs));
                if (echo.ready(session.pty_fd()) && !session.pump_output()) child_gone = true;
            }
            if (child_gone) continue; // re-poll to observe the exit transition

            // 4. Render — but only if something visible changed and the flood
            //    frame-rate cap allows it. The RenderKey folds damage + blink
            //    into one comparison.
            const Millis now = Millis::now();
            const BlinkState blink = BlinkState::at(now);
            const RenderKey key{session.generation(), blink};

            // Title follows the child (OSC 0/2). Cheap string compare gates it.
            if (std::string t = session.window_title(); t != last_title) {
                pf::title(surf_, t);
                last_title = std::move(t);
            }

            const bool flood = session.output_pending();
            const bool rate_ok = !flood || (now.value - last_present_ms) >= kFloodPresentMs;
            if ((child_gone || drawn != key) && rate_ok) {
                glViewport(0, 0, px_.w, px_.h);
                auto rc = toe::gfx::RenderContext::adopt_current();
                const toe::DamageRect dmg = session.render(rc, px_, blink.cursor_on, blink.text_on);
                pf::present(surf_, dmg.empty() ? toe::DamageRect::full(px_) : dmg);
                drawn = key;
                last_present_ms = now.value;
            }

            // 5. Sleep until real work arrives — child output, a window event, or
            //    the blink/animation timer — instead of busy-spinning. EXCEPT
            //    under a flood: if output is still queued we loop straight back to
            //    drain it, having just rendered an intermediate frame.
            pf::flush(surf_);
            if (!session.output_pending()) {
                PollSet wake;
                wake.add(surf_.event_fd())
                    .add(session.pty_fd())
                    .add(pf::repeat_fd(surf_)); // -1 on backends without a repeat timer; skipped
                wake.wait(idle_timeout(session, now));
            }
        }
        return 0;
    }

private:
    S &surf_;
    toe::Terminal &term_;
    toe::PixelSize px_;
};

// Deduction so `App{surf, term, px}` names `App<decltype(surf)>`.
template <pf::Surface S>
App(S &, toe::Terminal &, toe::PixelSize) -> App<S>;

} // namespace hand

#endif // HAND_APP_LOOP_HPP
