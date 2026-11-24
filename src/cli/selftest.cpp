// hand::cli — the headless self-test (`hand --headless`, ctest app_headless):
// every suite in order, on one runtime showing the demo screen. Returns the
// number of failures. The suites are in src/cli/checks/, one per feature.

#include "checks/suites.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace hand::cli {

using namespace mayag;

int selftest(Runtime& rt, Vec2 size) {
    checks::Check ok;
    {   // the demo screen, as main left it
        const auto& m = rt.model();
        ok(m.tab().term->cols() > 20 && m.tab().term->rows() > 5, "the grid fits the window");
        const auto g = m.tab().term->snapshot();
        ok(g.lines[0].cells[0].text == "~" && !g.lines[0].cells[0].default_fg, "toe parsed colour output");
        ok(g.cursor_row == 6, "the cursor follows the output");
        rt.window().resize(Vec2{size.x / 2.0f, size.y / 2.0f});
        rt.tick();
        ok(rt.model().tab().term->cols() < g.cols, "a resize reflows the grid");
    }
    // order matters: later suites start from the state earlier ones leave
    static constexpr std::array<checks::Suite, 21> suites{
        checks::pty,
        checks::caches_exact,
        checks::keystroke,
        checks::settings_panel,
        checks::every_setting,
        checks::visual_settings,
        checks::terminal_features,
        checks::fish_integration,
        checks::resize_fish,
        checks::wide_squeeze,
        checks::wrap_spaces,
        checks::selection,
        checks::tab_list,
        checks::tabs,
        checks::tui_mouse,
        checks::pane_drag,
        checks::editor,
        checks::remote_files,
        checks::mac,
        checks::ssh,
        checks::config_roundtrip,
    };
    for (const auto suite : suites) {
        suite(rt, size, ok);
        // A toast or the size HUD leaves on a REAL-time timer (no frames while
        // it is up); a suite runs faster than that, so the next one would start
        // under whatever the last left on screen. Retire them as their timers
        // would.
        rt.send(hand::App::Msg{hand::App::ToastExpired{rt.model().toast.serial}});
        rt.send(hand::App::Msg{hand::App::HudExpired{rt.model().hud_serial}});
    }
    // jaal times every update (options::fold_budget_time, 8 ms) and names a
    // slow one on stderr. The self-test drives every feature once: none of
    // them may hold the loop -- the UI -- longer than a frame. Wall-clock, so
    // it is a GATE only where the machine is ours: ctest's serial `fold_budget`
    // (HAND_FOLD_BUDGET=strict); in a parallel run, or under a sanitizer,
    // a slow fold is still named on stderr but does not fail the suite.
    if (const char* s = std::getenv("HAND_FOLD_BUDGET"); s && std::string_view{s} == "strict")
        ok(rt.slow_folds() == 0, "no update ran over jaal's fold budget");
    else if (rt.slow_folds() > 0)
        std::printf("  note  %llu update(s) over jaal's fold budget (see stderr; HAND_FOLD_BUDGET=strict fails on it)\n",
                    static_cast<unsigned long long>(rt.slow_folds()));
    {   // ...and every widget the suites drove drew within one 120 Hz refresh
        // (8.3 ms): view + cascade + layout + paint, the part every backend pays
        // (the headless raster is the software path a GPU window does not run).
        // Measured: ~4.3 ms, a zoom frame rasterising every glyph at a new size
        // for the first time; ordinary frames are well under 1 ms.
        // The gate is on the frame's CPU time: on wall time it failed on a
        // loaded machine for frames whose paint "took" 11.8 ms on 122
        // instances -- the thread descheduled, not the code slow.
        const auto& w = rt.host().latency().slowest();
        const auto& cw = rt.host().latency().slowest_cpu();
        const double ms = mayag::LatencyStats::widget_ms(w);
        std::printf("  slowest widget frame %.2f ms  (view %.2f cascade %.2f layout %.2f paint %.2f, %u instances)\n", ms,
                    w.view_ms, w.cascade_ms, w.layout_ms, w.paint_ms, static_cast<unsigned>(w.instances));
        std::printf("  most widget work in a frame %.2f ms cpu  (wall: view %.2f cascade %.2f layout %.2f paint %.2f, %u instances)\n",
                    cw.widget_cpu_ms, cw.view_ms, cw.cascade_ms, cw.layout_ms, cw.paint_ms, static_cast<unsigned>(cw.instances));
        if (const char* s = std::getenv("HAND_FOLD_BUDGET"); s && std::string_view{s} == "strict")
            ok(cw.widget_cpu_ms < 8.0, "no widget frame's work misses a 120 Hz refresh (8 ms)");
    }
    std::printf(ok.failures() ? "FAIL\n" : "PASS\n");
    return ok.failures();
}

}  // namespace hand::cli
