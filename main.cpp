// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand — a terminal, a pun on foot. A native (Wayland/X11) keyboard-driven
// terminal on libtoe with no GTK, no VTE, no SDL: the window comes from
// toe::platform, the terminal from toe::Terminal, the config from VIBE.
//
// This file is deliberately thin. The engine (toe) owns everything from PTY to
// pixels; input policy lives in EventRouter; config parsing lives in config.*;
// time/blink and the poll set are their own small value types. What remains
// here is the *shape of a frame*, expressed as a sequence of named steps.

#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include <epoxy/gl.h>

#include "toe/gfx/render_target.hpp"
#include "toe/platform/backend.hpp"
#include "toe/terminal.hpp"

#include "blink.hpp"
#include "config.hpp"
#include "event_router.hpp"
#include "poll_set.hpp"

namespace hand {
namespace {

// How long we wait for the child's echo before rendering a keystroke, and the
// idle cursor-blink cadence. Named so the frame loop reads as intent.
constexpr int kEchoWaitMs = 3;    // local shells echo in µs; this is a ceiling
constexpr int kIdlePollMs = 250;  // keeps cursor blink crisp when nothing else wakes us
constexpr int kMinAnimMs = 16;    // don't spin faster than ~60fps on animations

// The render gate: a terminal frame is drawn only when something the user can
// see has changed. That "something" is fully captured by the damage generation
// plus the current blink state — so equality of this value across frames means
// "nothing to redraw". Comparing whole RenderKeys replaces four ad-hoc
// last_* variables with one total comparison.
struct RenderKey {
    std::uint64_t generation{0};
    BlinkState blink{};
    constexpr auto operator<=>(const RenderKey &) const = default;
};

// Compute the poll timeout for an idle wait: the blink cadence, tightened to an
// inline-image animation's next-frame deadline when one is playing.
[[nodiscard]] Timeout idle_timeout(const toe::Session &s, Millis now) noexcept {
    if (const std::uint64_t deadline = s.next_animation_deadline(); deadline != 0) {
        const std::int64_t wait =
            static_cast<std::int64_t>(deadline) - static_cast<std::int64_t>(now.value);
        const int clamped = static_cast<int>(std::clamp<std::int64_t>(wait, kMinAnimMs, kIdlePollMs));
        return Timeout::millis(clamped);
    }
    return Timeout::millis(kIdlePollMs);
}

} // namespace
} // namespace hand

int main(int argc, char **argv) {
    using namespace hand;
    const std::span<char *> args{argv, static_cast<std::size_t>(argc)};

    const toe::Config cfg = load_config(args);

    auto surface = toe::platform::open_surface("hand", toe::PixelSize{800, 500});
    if (!surface) {
        std::fprintf(stderr, "hand: %s\n", surface.error().message.c_str());
        return 1;
    }
    toe::platform::AnySurface &surf = *surface;
    toe::PixelSize px = surf.pixel_size();

    auto term = toe::Terminal::create(cfg, px);
    if (!term) {
        std::fprintf(stderr, "hand: %s\n", term.error().message.c_str());
        return 1;
    }

    bool running = true;
    std::string last_title;
    std::optional<RenderKey> drawn; // the key of the last rendered frame, if any

    while (running && !surf.should_close()) {
        // The lifecycle's only transition: Running -> (Running | Exited). A dead
        // terminal has no Session, so there is nothing to render or type into —
        // we return its exit code and the loop ends.
        const toe::Terminal::Poll p = term->poll();
        if (p.exited) return p.exited->code;
        toe::Session &session = *p.running;

        // 1. Reflect terminal -> window state (OSC 0/2 title, OSC 52 clipboard).
        if (std::string t = session.window_title(); t != last_title) {
            surf.set_title(t);
            last_title = std::move(t);
        }
        if (auto clip = session.take_clipboard_request()) {
            surf.set_clipboard(*clip);
        }

        // 2. Route window events -> Session actions via the exhaustive visitor.
        //    It tells us whether any event handed bytes to the child.
        EventRouter router{session, surf, px, running};
        surf.poll_events([&](const toe::platform::Event &ev) { std::visit(router, ev); });

        // 3. Zero-latency local echo: after sending input, flush and give the
        //    PTY a few ms to echo, draining what returns so the typed glyph
        //    lands in THIS frame instead of a vsync later. poll() returns the
        //    instant the echo is readable; the ceiling only bites for a busy
        //    child, and then we simply catch up on the next wake.
        bool child_gone = false;
        if (router.take_wrote_input()) {
            surf.flush();
            PollSet echo;
            echo.add(session.pty_fd());
            echo.wait(Timeout::millis(kEchoWaitMs));
            if (echo.ready(session.pty_fd()) && !session.pump_output()) child_gone = true;
        }

        // 4. Render only when the frame's RenderKey changed. The key folds the
        //    damage counter and both blink phases into one comparable value, so
        //    idle frames (nothing damaged, no blink flip) draw nothing.
        const Millis now = Millis::now();
        session.tick_animations(now.value); // may advance damage
        const RenderKey key{session.generation(), BlinkState::at(now)};
        if (child_gone || drawn != key) {
            glViewport(0, 0, px.w, px.h);
            auto rc = toe::gfx::RenderContext::adopt_current();
            session.render(rc, px, key.blink.cursor_on, key.blink.text_on);
            surf.swap();
            drawn = key;
        }

        // 5. Sleep until real work arrives — child output, a window event, or
        //    the blink/animation timer — instead of busy-spinning. EXCEPT under
        //    a flood: if output is still queued we loop straight back to drain
        //    it, having just rendered an intermediate frame.
        surf.flush();
        if (session.output_pending()) continue;

        PollSet wake;
        wake.add(session.pty_fd())
            .add(surf.event_fd())
            .add(surf.repeat_fd()); // -1 on backends without a repeat timer; skipped
        wake.wait(idle_timeout(session, now));
    }

    return 0;
}
