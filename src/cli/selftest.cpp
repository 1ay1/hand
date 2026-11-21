// hand::cli — the headless self-test (`hand --headless`, ctest app_headless):
// every suite in order, on one runtime showing the demo screen. Returns the
// number of failures. The suites are in src/cli/checks/, one per feature.

#include "checks/suites.hpp"

#include <array>
#include <cstdio>

namespace hand::cli {

using namespace mayag;

int selftest(Runtime& rt, Vec2 size) {
    checks::Check ok;
    {   // the demo screen, as main left it
        const auto& m = rt.model();
        ok(m.term->cols() > 20 && m.term->rows() > 5, "the grid fits the window");
        const auto g = m.term->snapshot();
        ok(g.lines[0].cells[0].text == "~" && !g.lines[0].cells[0].default_fg, "toe parsed colour output");
        ok(g.cursor_row == 6, "the cursor follows the output");
        rt.window().resize(Vec2{size.x / 2.0f, size.y / 2.0f});
        rt.tick();
        ok(rt.model().term->cols() < g.cols, "a resize reflows the grid");
    }
    // order matters: later suites start from the state earlier ones leave
    static constexpr std::array<checks::Suite, 15> suites{
        checks::pty,
        checks::settings_panel,
        checks::every_setting,
        checks::visual_settings,
        checks::terminal_features,
        checks::fish_integration,
        checks::resize_fish,
        checks::wide_squeeze,
        checks::wrap_spaces,
        checks::selection,
        checks::tabs,
        checks::tui_mouse,
        checks::mac,
        checks::ssh,
        checks::config_roundtrip,
    };
    for (const auto suite : suites) suite(rt, size, ok);
    std::printf(ok.failures() ? "FAIL\n" : "PASS\n");
    return ok.failures();
}

}  // namespace hand::cli
