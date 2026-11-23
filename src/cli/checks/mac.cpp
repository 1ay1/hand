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
    // ── the Mac's ⌘ shortcuts and the input method ── run on every
    // machine: the keys say whose conventions they follow (Mods.keymap)
    using K = mayag::Key;
    using M = mayag::Mods;
    const M cmd{.super = true, .keymap = mayag::Keymap::mac};
    const std::size_t tabs0 = rt.model().tab_count();
    {   // the PC's Super belongs to the desktop: Super+T is not ⌘T there
        rt.press_key(K::t, M{.super = true, .keymap = mayag::Keymap::pc});
        for (int i = 0; i < 4; ++i) rt.tick();
        ok(rt.model().tab_count() == tabs0, "on the PC, Super+T is the desktop's, not a new tab");
    }
    rt.press_key(K::t, cmd);
    for (int i = 0; i < 4; ++i) rt.tick();
    ok(rt.model().tab_count() == tabs0 + 1, "\u2318T opens a tab");
    rt.press_key(K::n1, cmd);
    ok(rt.model().active() == 0, "\u23181 selects the first tab");
    rt.press_key(K::w, cmd);
    for (int i = 0; i < 4; ++i) rt.tick();
    if (rt.model().dialog.ask) rt.press_key(K::enter);
    for (int i = 0; i < 4; ++i) rt.tick();
    ok(rt.model().tab_count() == tabs0, "\u2318W closes it");
    rt.press_key(K::f, cmd);
    ok(rt.model().tab().search_open, "\u2318F finds");
    rt.press_key(K::escape);
    rt.press_key(K::comma, cmd);
    ok(rt.model().settings.open, "\u2318, opens settings");
    rt.press_key(K::comma, cmd);
    ok(!rt.model().settings.open, "...and closes them");
    const float fs = rt.model().font_size;
    rt.press_key(K::equal, cmd);
    ok(rt.model().font_size > fs, "\u2318= zooms in");
    rt.press_key(K::n0, cmd);
    ok(rt.model().font_size == fs, "\u23180 resets");
    const auto bytes0 = rt.model().tab().term->epoch();
    rt.press_key(K::j, cmd);   // bound to nothing: consumed, never typed
    ok(rt.model().tab().term->epoch() == bytes0 && !rt.model().dialog.ask, "an unbound \u2318 key does nothing");
    rt.send(hand::App::Msg{hand::App::Output{"old line\r\n"}});
    rt.press_key(K::k, cmd);
    rt.tick();
    ok(rt.model().tab().term->history() == 0 && rt.model().tab().term->search("old line") == 0, "\u2318K clears the screen and history");
    rt.model().tab().term->search_clear();
    // the input method: composing shows at the cursor, only the commit is typed
    rt.window().compose("\u3053\u3093");
    rt.tick();
    ok(rt.model().preedit == "\u3053\u3093" && rt.model().tab().grid && rt.model().tab().grid->preedit == "\u3053\u3093",
       "a composition shows at the cursor");
    rt.window().commit("\u4eca\u65e5\u306f");
    rt.tick();
    ok(rt.model().preedit.empty(), "...and goes once committed");
}

}  // namespace hand::cli::checks
