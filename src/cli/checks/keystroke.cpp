// hand::cli::checks — what a keystroke costs on the real screen.
//
// Measured with tools/photon.sh on Hyprland: a surface that is idle when an
// input lands is shown ~1 ms after its commit; one that is already drawing
// every refresh (an animation) waits for the next vblank -- ~11 ms more. So
// the echo of a typed key must arrive with nothing left moving: the cursor
// is at its cell in the very frame that shows the character, and no frame
// clock keeps running after it.

#include "suites.hpp"

#include <hand/app.hpp>

#include <cmath>
#include <string>

namespace hand::cli::checks {

using namespace mayag;

void keystroke([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    using E = mayag::Edit<App::Model>;
    rt.send(App::Msg{E{[](App::Model& m) { m.cfg.cursor_anim = CursorAnim::glide; m.cfg.cursor_trail = 1.0f; m.cfg.cursor_blink = false; }}});
    rt.window().drive_frames(true);   // the frame clock runs: Tick moves the cursor
    rt.send(App::Msg{App::Output{"\x1b[2J\x1b[H$ "}});
    for (int i = 0; i < 120; ++i) rt.tick();   // anything from before has settled
    bool at_cell = true;
    for (const char ch : std::string{"echo"}) {
        rt.send(App::Msg{E{[](App::Model& m) { m.tab().fx.typed_at = m.time; }}});   // a key went to the pty...
        rt.send(App::Msg{App::Output{std::string(1, ch)}});                           // ...and its echo came back
        const auto& se = rt.model().tab();
        const auto cur = se.term->cursor();
        at_cell = at_cell && std::fabs(se.cur_x - static_cast<float>(cur.col)) < 0.001f &&
                  std::fabs(se.fx.tx - static_cast<float>(cur.col)) < 0.001f;
    }
    ok(at_cell, "a typed key's echo: the cursor (and its tail) is at its cell in the frame that shows it");
    rt.tick();
    ok(cli::motion_settled(rt.model()) && !rt.host().frames_on(),
       "...and nothing keeps animating after it (an idle surface is shown a refresh sooner)");
    // a big jump still travels: the glide is for motion you'd lose track of
    rt.send(App::Msg{E{[](App::Model& m) { m.tab().fx.typed_at = m.time; }}});
    rt.send(App::Msg{App::Output{"\x1b[1;60H"}});
    ok(std::fabs(rt.model().tab().cur_x - 59.0f) > 1.0f, "a jump across the row still glides");
    for (int i = 0; i < 120; ++i) rt.tick();
    rt.window().drive_frames(false);
    rt.send(App::Msg{E{[](App::Model& m) { m.cfg.cursor_blink = true; }}});

    // An idle window draws nothing: after ~10 s of blinks with no keys and no
    // output the cursor rests, solid, and no frames are asked for (a fading
    // blink otherwise animated ~40 frames a second forever).
    {
        const int n = static_cast<int>(10000.0f / rt.model().cfg.blink_ms) + 2;
        for (int i = 0; i < n; ++i) { rt.send(App::Msg{App::Blink{}}); for (int k = 0; k < 40; ++k) rt.tick(); }
        ok(rt.model().blinks_idle >= n - 2 && rt.model().cursor_on && rt.model().tab().cur_a == 1.0f && !rt.host().frames_on(),
           "an idle window stops blinking: the cursor rests solid and no frames are drawn");
        rt.send(App::Msg{App::Typed{"x"}});
        ok(rt.model().blinks_idle == 0, "...and a key wakes the blink");
        for (int i = 0; i < n; ++i) rt.send(App::Msg{App::Blink{}});
        rt.send(App::Msg{App::Output{"done\r\n"}});
        ok(rt.model().blinks_idle == 0, "...and so does output (a resting window has no blink timer to notice it)");
    }
}

}  // namespace hand::cli::checks
