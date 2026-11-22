// hand::cli::checks — the settings panel: open, navigate, edit, close.

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

void settings_panel([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── the settings panel ──────────────────────────────────────────
    using K = mayag::Key;
    rt.window().resize(size);
    rt.tick();
    // ── links + search first, on a known screen ──────────────────
    rt.send(hand::App::Msg{hand::App::Output{std::string{
        "\x1b[2J\x1b[Hsee https://example.com/docs ok\r\n"
        "\x1b]8;;https://toe.dev\x1b\\osc8 link\x1b]8;;\x1b\\ and needle, needle\r\n"}}});
    rt.tick();
    ok(rt.model().tab().term->link_at(8, 0) == "https://example.com/docs", "a bare URL is detected under the pointer");
    ok(rt.model().tab().term->link_at(1, 1) == "https://toe.dev", "an OSC 8 hyperlink resolves to its target");
    ok(rt.model().tab().term->link_at(0, 0).empty(), "plain text is not a link");
    rt.press_key(K::f, Mods{.shift = true, .ctrl = true});
    ok(rt.model().tab().search_open, "ctrl+shift+f opens search");
    rt.type("needle");
    ok(rt.model().tab().term->search_count() == 2, "typing finds every match in the scrollback");
    rt.tick();
    ok(rt.model().tab().grid && rt.model().tab().grid->matches.size() == 2 && rt.model().tab().grid->current_match >= 0,
       "matches are highlighted, the current one marked");
    const auto cur = rt.model().tab().term->search_current();
    rt.press_key(K::enter);
    ok(rt.model().tab().term->search_current() != cur, "enter steps to the next match");
    rt.press_key(K::escape);
    ok(!rt.model().tab().search_open && rt.model().tab().term->search_count() == 0, "esc closes and clears it");
    rt.press_key(K::comma, Mods{.ctrl = true});
    ok(rt.model().settings.open, "ctrl+, opens settings");
    ok(rt.node_rect(dsl::node_id("settings.rows")).has_value(), "the mayag settings_panel is on screen");
    rt.type("j");   // vi keys: font.family -> font.size
    ok(rt.model().settings.ui.selected == 1, "j selects the next setting");
    const float before = rt.model().font_size;
    const int cols = rt.model().tab().term->cols();
    rt.press_key(K::right, Mods{.shift = true});
    ok(rt.model().cfg.font_size == before + 5.0f && rt.model().font_size == before + 5.0f,
       "shift+right steps font.size ×10, live");
    ok(rt.model().tab().term->cols() < cols, "and the grid refits to the bigger cells");
    rt.type("r");
    ok(rt.model().cfg.font_size == hand::Config{}.font_size, "r resets it");
    rt.type("/");
    rt.type("scheme");
    rt.press_key(K::enter);
    ok(rt.model().settings.ui.query == "scheme" && !rt.model().settings.ui.filtering, "/ filters, enter keeps the filter");
    const auto bg0 = rt.model().palette.bg;
    const int scheme0 = rt.model().cfg.scheme;
    rt.press_key(K::enter);   // the scheme row: open its select
    rt.tick();
    ok(rt.model().settings.ui.menu >= 0, "enter opens the scheme dropdown");
    ok(rt.node_rect(dsl::part_id("settings", dsl::kSetMenuPart)).has_value() ||
       rt.node_rect(dsl::node_id("settings.colors.scheme.select")).has_value(), "the menu is on screen");
    rt.press_key(K::down);
    ok(rt.model().palette.bg != bg0, "↓ in the menu previews the next scheme live");
    rt.press_key(K::escape);
    ok(rt.model().settings.ui.menu < 0 && rt.model().palette.bg == bg0 && rt.model().cfg.scheme == scheme0,
       "esc closes the menu and reverts the preview");
    rt.press_key(K::enter);
    rt.type("dracula");
    rt.press_key(K::enter);
    ok(rt.model().palette.bg == 0x282A36, "typing filters 913 schemes, enter keeps one");
    rt.press_key(K::right);
    ok(rt.model().palette.bg != 0x282A36, "→ steps to the next scheme without the menu");
    rt.press_key(K::escape);   // clears the filter
    rt.press_key(K::escape);   // closes
    ok(!rt.model().settings.open, "esc clears the filter, then closes");
    {
        // motion: the panel fades out (still drawn a moment), the blink fades
        rt.window().drive_frames(true);
        rt.press_key(K::comma, Mods{.ctrl = true});
        rt.tick();
        ok(rt.model().settings.t > 0.0f && rt.model().settings.t < 1.0f, "the settings panel fades in");
        for (int i = 0; i < 30; ++i) rt.tick();
        ok(rt.model().settings.t == 1.0f, "...and settles");
        rt.press_key(K::escape);
        rt.tick();
        ok(!rt.model().settings.open && rt.model().settings.t > 0.0f, "closing, it fades out instead of vanishing");
        for (int i = 0; i < 30; ++i) rt.tick();
        ok(rt.model().settings.t == 0.0f, "...and is gone");
        // the real blink timer must not flip it back mid-check (a loaded machine
        // can fire it in the same step): park it far away while we look
        const float blink_ms = rt.model().cfg.blink_ms;
        rt.send(hand::App::Msg{hand::msg::Edit{[](hand::App::Model& mm) { mm.cfg.blink_ms = 60000.0f; mm.cursor_on = true; mm.tab().cur_a = 1.0f; }}});
        rt.send(hand::App::Msg{hand::App::Blink{}});
        rt.tick();
        const float mid = rt.model().tab().cur_a;
        for (int i = 0; i < 30; ++i) rt.tick();
        ok(mid > 0.0f && mid < 1.0f && (rt.model().tab().cur_a == 0.0f || rt.model().tab().cur_a == 1.0f), "the cursor blink fades, not snaps");
        rt.send(hand::App::Msg{hand::msg::Edit{[blink_ms](hand::App::Model& mm) { mm.cfg.blink_ms = blink_ms; }}});
        rt.window().drive_frames(false);
    }
    {
        // ── the dropdown with the mouse ────────────────────────────────────
        rt.press_key(K::comma, Mods{.ctrl = true});
        rt.tick();
        // the interface-font select: on the first screen of the panel (a click
        // lands where a user's would, so the target must be in view)
        const auto trig = dsl::node_id("settings.font.ui.select");
        rt.click(trig);
        ok(rt.model().settings.ui.menu >= 0, "clicking the dropdown opens it");
        rt.click(trig);
        ok(rt.model().settings.ui.menu < 0, "clicking it again closes it");
        rt.click(trig);
        rt.click(dsl::part_id("settings.nav", 0));   // the sidebar: never under the menu
        ok(rt.model().settings.ui.menu < 0 && rt.model().settings.open,
           "clicking elsewhere in the panel closes it (and the panel stays)");
        rt.click(trig);
        ok(rt.model().settings.ui.menu >= 0, "and it opens again after that");
        rt.press_key(K::escape);
        rt.press_key(K::escape);
        if (rt.model().settings.open) rt.press_key(K::escape);
    }
    {
        // ── the close button ─────────────────────────────────────────────
        rt.press_key(K::comma, Mods{.ctrl = true});
        rt.tick();
        const bool was_open = rt.model().settings.open;
        rt.click(dsl::node_id("settings.close"));
        rt.tick();
        ok(was_open && !rt.model().settings.open, "the settings panel's close button closes it");
        if (rt.model().settings.open) rt.press_key(K::escape);
    }
}

}  // namespace hand::cli::checks
