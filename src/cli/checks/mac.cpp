// hand::cli::checks — the Mac: command shortcuts, the input method.

#include "suites.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hand::cli::checks {

using namespace mayag;

void mac([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── the Mac: ⌘ shortcuts, the input method ──
    using K = mayag::Key;
    using M = mayag::Mods;
    const std::size_t tabs0 = rt.model().tab_count();
    rt.press_key(K::t, M{.super = true});
    for (int i = 0; i < 4; ++i) rt.tick();
    ok(rt.model().tab_count() == tabs0 + 1, "\u2318T opens a tab");
    rt.press_key(K::n1, M{.super = true});
    ok(rt.model().active == 0, "\u23181 selects the first tab");
    rt.press_key(K::w, M{.super = true});
    for (int i = 0; i < 4; ++i) rt.tick();
    if (rt.model().dialog.ask) rt.press_key(K::enter);
    for (int i = 0; i < 4; ++i) rt.tick();
    ok(rt.model().tab_count() == tabs0, "\u2318W closes it");
    rt.press_key(K::f, M{.super = true});
    ok(rt.model().search_open, "\u2318F finds");
    rt.press_key(K::escape);
    rt.press_key(K::comma, M{.super = true});
    ok(rt.model().settings.open, "\u2318, opens settings");
    rt.press_key(K::comma, M{.super = true});
    ok(!rt.model().settings.open, "...and closes them");
    const float fs = rt.model().font_size;
    rt.press_key(K::equal, M{.super = true});
    ok(rt.model().font_size > fs, "\u2318= zooms in");
    rt.press_key(K::n0, M{.super = true});
    ok(rt.model().font_size == fs, "\u23180 resets");
    const auto bytes0 = rt.model().term->epoch();
    rt.press_key(K::j, M{.super = true});   // bound to nothing: consumed, never typed
    ok(rt.model().term->epoch() == bytes0 && !rt.model().dialog.ask, "an unbound \u2318 key does nothing");
    rt.send(hand::App::Msg{hand::App::Output{"old line\r\n"}});
    rt.press_key(K::k, M{.super = true});
    rt.tick();
    ok(rt.model().term->history() == 0 && rt.model().term->search("old line") == 0, "\u2318K clears the screen and history");
    rt.model().term->search_clear();
    // the input method: composing shows at the cursor, only the commit is typed
    rt.window().compose("\u3053\u3093");
    rt.tick();
    ok(rt.model().preedit == "\u3053\u3093" && rt.model().grid && rt.model().grid->preedit == "\u3053\u3093",
       "a composition shows at the cursor");
    rt.window().commit("\u4eca\u65e5\u306f");
    rt.tick();
    ok(rt.model().preedit.empty(), "...and goes once committed");
}

}  // namespace hand::cli::checks
