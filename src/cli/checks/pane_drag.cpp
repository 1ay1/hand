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
#include <string>
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
    // one drag: out past the max (640) and back, 5 moves a frame (4 coalesced).
    // `live`: the grid must follow the pointer every frame; else keep its size.
    const auto drag = [&](bool live, const char* what) {
        const int cols0 = rt.model().tab().term->cols();
        const float x0 = edge() - 2.0f;   // on the 5 px grip, 2 px in from the edge
        const float grab = edge() - x0;   // where in the grip it was taken
        rt.window().press(Vec2{x0, y});
        rt.tick();
        bool follows = true, grid_ok = true;
        std::uint64_t worst_cells = 0;   // re-wrap work in any one frame, every tab
        float worst = 0.0f;
        const int steps = 60;
        for (int s = 1; s <= steps; ++s) {
            const float t = static_cast<float>(s) / steps;
            const float target = x0 + (t < 0.5f ? t * 2.0f : (1.0f - t) * 2.0f) * 500.0f - (t > 0.5f ? 40.0f : 0.0f);
            for (int k = 1; k <= 5; ++k) rt.window().move_to(Vec2{target - static_cast<float>(5 - k) * 1.3f, y});
            std::uint64_t before = 0;
            for (std::size_t i = 0; i < rt.model().tab_count(); ++i) before += rt.model().at(i).term->reflowed_cells();
            rt.tick();
            if (s == steps / 4) {   // a pause with the grip held: nothing may settle under it
                rt.settle([](const App::Model&) { return false; }, std::chrono::milliseconds(150));
                rt.tick();
            }
            std::uint64_t after = 0;
            for (std::size_t i = 0; i < rt.model().tab_count(); ++i) after += rt.model().at(i).term->reflowed_cells();
            worst_cells = std::max(worst_cells, after - before);
            const float want = std::clamp(target + grab, 220.0f, 640.0f);
            const float off = std::fabs(edge() - want);
            worst = std::max(worst, off);
            follows = follows && off < 0.5f;
            const int cols = rt.model().tab().term->cols();
            grid_ok = grid_ok && (live ? cols == rt.model().metrics.fit_cols : cols == cols0);
        }
        ok(follows, (std::string{what} + ": the pane edge stays under the pointer through coalesced moves and the clamp (worst " +
                     std::to_string(worst) + " px)").c_str());
        ok(grid_ok, (std::string{what} + (live ? ": the grid re-wraps live, every frame of the drag"
                                               : ": the grid keeps its size while held (no 10k-line re-wrap while the pointer is held)")).c_str());
        ok(worst_cells <= App::kFrameReflowCells,
           (std::string{what} + ": no frame of the drag re-wraps more than the frame budget (" + std::to_string(worst_cells) +
            " cells, budget " + std::to_string(App::kFrameReflowCells) + ")").c_str());
        rt.window().release(Vec2{x0 - 40.0f, y});
        rt.tick();
        rt.settle([&](const App::Model& m) { return !m.pane.dragging; }, std::chrono::milliseconds(500));
        for (int i = 0; i < 3; ++i) rt.tick();
        ok(!rt.model().pane.dragging && rt.model().tab().term->cols() == rt.model().metrics.fit_cols,
           (std::string{what} + ": released, the grid fits the new width").c_str());
    };
    drag(true, "short history");
    {   // a full scrollback
        std::string hist;
        for (int i = 0; i < 10000; ++i) hist += std::to_string(i) + " " + std::string(100, 'x') + "\r\n";
        rt.send(App::Msg{App::Output{hist}});
    }
    drag(false, "10k-line history");
    rt.send(App::Msg{App::TogglePane{}});   // closed again for the suites after
    for (int i = 0; i < 3; ++i) rt.tick();
}

}  // namespace hand::cli::checks
