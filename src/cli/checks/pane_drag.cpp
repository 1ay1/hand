// hand::cli::checks — the file pane's edge follows the pointer.
//
// Real pointer events through mayag's input layer: several moves a frame
// (the host coalesces them -- their motion must not be lost), past the
// pane's maximum and back (a clamp must not leave the edge off the pointer),
// and the terminal grid waits for the drag to pause before it reflows.

#include "suites.hpp"

#include <hand/app.hpp>

#include <chrono>
#include <cmath>
#include <thread>

namespace hand::cli::checks {

using namespace mayag;

void pane_drag([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    rt.send(App::Msg{mayag::Edit<App::Model>{[](App::Model& m) {
        m.cfg.files_left = true; m.cfg.files_width = 300.0f;
    }}});
    if (!rt.model().pane.open) rt.send(App::Msg{App::TogglePane{}});
    for (int i = 0; i < 4; ++i) rt.tick();
    const float top = rt.model().strip_h();
    const float y = top + 120.0f;
    const auto edge = [&] { return rt.model().pane.width; };
    const int cols0 = rt.model().tab().term->cols();
    const float x0 = edge() - 2.0f;   // on the 5 px grip, 2 px in from the edge
    const float grab = edge() - x0;   // where in the grip it was taken
    rt.window().press(Vec2{x0, y});
    rt.tick();
    bool follows = true, waited = true;
    float worst = 0.0f;
    // out to past the max (640) and back, 5 moves a frame: 4 of them coalesced
    const int steps = 60;
    for (int s = 1; s <= steps; ++s) {
        const float t = static_cast<float>(s) / steps;
        const float target = x0 + (t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f) * 500.0f - (t > 0.5f ? 40.0f : 0.0f);
        for (int k = 1; k <= 5; ++k) {
            rt.window().move_to(Vec2{target - static_cast<float>(5 - k) * 1.3f, y});
        }
        rt.tick();
        const float want = std::clamp(target + grab, 220.0f, 640.0f);
        const float off = std::fabs(edge() - want);
        worst = std::max(worst, off);
        follows = follows && off < 0.5f;
        waited = waited && rt.model().tab().term->cols() == cols0;
    }
    ok(follows, ("the pane edge stays under the pointer through coalesced moves and the clamp (worst " +
                 std::to_string(worst) + " px)").c_str());
    ok(waited, "the terminal grid keeps its size while the edge is held (no reflow per column)");
    rt.window().release(Vec2{x0 - 40.0f, y});
    rt.tick();
    rt.settle([&](const App::Model& m) { return !m.pane.dragging; }, std::chrono::milliseconds(500));
    for (int i = 0; i < 3; ++i) rt.tick();
    ok(!rt.model().pane.dragging && rt.model().tab().term->cols() == rt.model().metrics.fit_cols,
       "released, the grid fits the new width at once");
    rt.send(App::Msg{App::TogglePane{}});   // closed again for the suites after
    for (int i = 0; i < 3; ++i) rt.tick();
}

}  // namespace hand::cli::checks
