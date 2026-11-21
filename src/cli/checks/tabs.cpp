// hand::cli::checks — tabs: open, switch, move, drag, close, background output.

#include "suites.hpp"

#include "../../app/detail.hpp"

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

void tabs([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── tabs ────────────────────────────────────────────────────────────
    using K = mayag::Key;
    rt.window().drive_frames(true);
    // the pill's motion is what these check; line (the default) has none
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) {
        mm.cfg.decorations = true; mm.cfg.tabs_always = false;   // the strip only from the second tab, like foot
        mm.strip.t = 0.0f; hand::App::fit(mm);
        mm.cfg.tab_style = hand::TabStyle::pill; mm.cfg.tab_height = 40.0f; }}});
    const auto settle = [&] { for (int i = 0; i < 60; ++i) rt.tick(); };
    settle();
    ok(rt.model().tab_count() == 1 && rt.model().strip_h() == 0.0f, "one tab: no strip, like foot");
    const int rows1 = rt.model().tab().term->rows();
    rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[2J\x1b[HTAB-ONE"}}});
    rt.press_key(K::t, Mods{.shift = true, .ctrl = true});
    settle();
    ok(rt.model().tab_count() == 2 && rt.model().active() == 1, "ctrl+shift+t opens a tab and switches to it");
    ok(rt.model().strip_h() > 30.0f && rt.node_rect(dsl::node_id("tabs")).has_value(), "the tab strip slides in");
    ok(rt.model().tab().term->rows() < rows1, "and every grid refits below it");
    rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[2J\x1b[HTAB-TWO"}}});
    const auto top = [&] {   // the whole visible screen, as one string
        std::string s0;
        const auto g = rt.model().tab().term->snapshot();
        for (const auto& l : g.lines) for (const auto& c : l.cells) s0 += c.text;
        return s0;
    };
    const auto has = [&](const char* w) { return top().find(w) != std::string::npos; };
    ok(has("TAB-TWO") && !has("TAB-ONE"), "the new tab has its own screen");
    rt.press_key(K::tab, Mods{.ctrl = true});
    settle();
    ok(rt.model().active() == 0 && has("TAB-ONE") && !has("TAB-TWO"), "ctrl+tab switches back, screen intact");
    // output for a background tab lands there, and marks it
    const hand::SessionId bg = rt.model().tabs[1].sid;
    rt.send(hand::App::Msg{hand::App::Output{std::string{"\r\nbackground!"}, bg}});
    rt.tick();
    ok(rt.model().tabs[1].unread && !has("background!"), "background output goes to its tab and marks it unread");
    rt.press_key(K::n2, Mods{.alt = true});
    settle();
    ok(rt.model().active() == 1 && !rt.model().tab().unread, "alt+2 jumps to it, and it is read");
    rt.press_key(K::t, Mods{.shift = true, .ctrl = true});
    settle();
    rt.press_key(K::page_up, Mods{.shift = true, .ctrl = true});
    settle();
    ok(rt.model().tab_count() == 3 && rt.model().active() == 1, "ctrl+shift+pgup moves the tab left");
    rt.click(dsl::part_id("tabs", 0));
    settle();
    ok(rt.model().active() == 0, "clicking a tab selects it");
    ok(rt.model().strip.motion.pill.ready(), "the active pill tracks it");
    {   // the marker: aligned with its tab, and elastic on the way
        using E6 = mayag::Edit<hand::App::Model>;
        rt.send(hand::App::Msg{E6{[](hand::App::Model& m) { m.cfg.tab_style = hand::TabStyle::underline; m.cfg.tab_anim = hand::TabAnim::stretch; }}});
        settle();
        const auto tab0 = rt.node_rect(dsl::part_id("tabs", 0)), mk = rt.node_rect(dsl::node_id("tabs.marker"));
        const auto strip = rt.node_rect(dsl::node_id("tabs"));
        const bool aligned = tab0 && mk && strip && mk->left() >= tab0->left() - 0.5f && mk->right() <= tab0->right() + 0.5f &&
                             std::fabs((mk->left() - tab0->left()) - (tab0->right() - mk->right())) < 1.0f &&
                             std::fabs(mk->bottom() - strip->bottom()) < 1.0f;
        ok(aligned, "the underline is centred on its tab, on the strip's edge");
        rt.click(dsl::part_id("tabs", 2));
        rt.tick(); rt.tick();
        const auto mid = rt.node_rect(dsl::node_id("tabs.marker"));
        const auto tab2 = rt.node_rect(dsl::part_id("tabs", 2));
        ok(mid && tab2 && mid->width() > tab2->width(), "on the way to the next tab it stretches");
        settle();
        const auto end = rt.node_rect(dsl::node_id("tabs.marker"));
        ok(end && tab2 && std::fabs(end->width() - tab2->width()) < 1.0f && std::fabs(end->left() - tab2->left()) < 1.0f, "...and lands, its own width again");
        rt.click(dsl::part_id("tabs", 0));
        settle();
    }
    {
        // ── hover a tab, then its ×: it appears and stays, frame after frame ──
        using E9 = mayag::Edit<hand::App::Model>;
        int flicker = 0;
        std::string which;
        for (int style = 0; style < 4; ++style) {
            rt.send(hand::App::Msg{E9{[style](hand::App::Model& mm) {
                mm.cfg.tab_style = static_cast<hand::TabStyle>(style); mm.cfg.tab_height = style == 3 ? 26.0f : 36.0f;
                hand::App::fit(mm); }}});
            settle();
            for (std::size_t t = 0; t < 2; ++t) {
                // a real pointer: across the whole tab, a few px at a time
                const auto tr = rt.node_rect(dsl::part_id("tabs", t));
                if (tr) {
                    int lost = 0, moved = 0;
                    for (float x = tr->left() + 2.0f; x < tr->right() - 1.0f; x += 2.0f) {
                        rt.window().move_to(Vec2{x, tr->center().y});
                        rt.tick();
                        if (!rt.node_rect(dsl::part_id("tabs", dsl::tab_part_close + t))) ++lost;
                        const auto now = rt.node_rect(dsl::part_id("tabs", t));
                        if (!now || std::fabs(now->width() - tr->width()) > 0.5f || std::fabs(now->left() - tr->left()) > 0.5f) ++moved;
                    }
                    if (lost) { ++flicker; which += " s" + std::to_string(style) + "t" + std::to_string(t) + ":sweep-lost" + std::to_string(lost); }
                    if (moved) { ++flicker; which += " s" + std::to_string(style) + "t" + std::to_string(t) + ":reflowed" + std::to_string(moved); }
                }
                rt.window().move_to(rt.host().center_of(dsl::part_id("tabs", t)));
                for (int f = 0; f < 4; ++f) rt.tick();
                const auto x = rt.node_rect(dsl::part_id("tabs", dsl::tab_part_close + t));
                if (!x) { ++flicker; which += " s" + std::to_string(style) + "t" + std::to_string(t) + ":no-x"; continue; }
                rt.window().move_to(x->center());
                for (int f = 0; f < 12; ++f) {
                    rt.tick();
                    const auto x2 = rt.node_rect(dsl::part_id("tabs", dsl::tab_part_close + t));
                    if (!x2 || std::fabs(x2->center().x - x->center().x) > 0.5f) {
                        ++flicker; which += " s" + std::to_string(style) + "t" + std::to_string(t) + "@" + std::to_string(f);
                        break;
                    }
                }
            }
        }
        if (flicker) std::printf("    close button unsteady:%s\n", which.c_str());
        ok(flicker == 0, "every tab style: the \u00D7 shows on hover and holds still under the pointer");
        rt.window().move_to(Vec2{-50.0f, -50.0f});
        rt.send(hand::App::Msg{E9{[](hand::App::Model& mm) { mm.cfg.tab_style = hand::TabStyle::pill; mm.cfg.tab_height = 40.0f; hand::App::fit(mm); }}});
        settle();
    }
    rt.press_key(K::w, Mods{.shift = true, .ctrl = true});
    settle();
    ok(rt.model().tab_count() == 2, "ctrl+shift+w closes the tab");
    rt.send(hand::App::Msg{hand::App::CloseTab{hand::TabIndex{1}}});
    settle();
    ok(rt.model().tab_count() == 1 && rt.model().strip_h() == 0.0f && rt.model().tab().term->rows() == rows1,
       "back to one tab: the strip slides away and the grid gets its rows back");
    {
        // ── no system title bar (the default): the strip IS the title area ──
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) {
            mm.cfg = hand::Config{}; mm.cfg.demo = true; hand::App::fit(mm); }}});
        settle();
        ok(rt.model().tab_count() == 1 && rt.model().strip_h() > 20.0f &&
           rt.node_rect(dsl::part_id("tabs", dsl::tab_part_window + 2)).has_value(),
           "undecorated: one tab still has the strip, with \u2212 \u25A1 \u00D7 on its right");
        const auto close = rt.node_rect(dsl::part_id("tabs", dsl::tab_part_window + 2));
        const auto strip = rt.node_rect(dsl::node_id("tabs"));
        ok(close && strip && std::fabs(close->right() - strip->right()) < 1.0f && std::fabs(close->top() - strip->top()) < 1.0f,
           "...flush in the top-right corner");
        const int moves0 = rt.window().moves();
        const Vec2 empty{strip->right() - 300.0f, strip->center().y};
        rt.window().push(mayag::MouseMove{empty, {}, {}});
        rt.window().push(mayag::MouseDown{empty, mayag::MouseButton::left, {}, 1});
        rt.tick();
        rt.window().push(mayag::MouseUp{empty, mayag::MouseButton::left, {}});
        rt.tick();
        ok(rt.window().moves() == moves0 + 1, "pressing the strip's empty space moves the window");
        rt.click(dsl::part_id("tabs", dsl::tab_part_window + 1));
        ok(rt.window().maximized(), "\u25A1 maximises");
        rt.click(dsl::part_id("tabs", dsl::tab_part_window + 1));
        const int mins = rt.window().minimizes();
        rt.click(dsl::part_id("tabs", dsl::tab_part_window + 0));
        ok(!rt.window().maximized() && rt.window().minimizes() == mins + 1, "...and back; \u2212 minimises");
        // tabs.always off: no strip with one tab; the top edge brings the title area
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) { mm.cfg.tabs_always = false; }}});
        settle();
        ok(rt.model().strip_h() == 0.0f && !rt.node_rect(dsl::node_id("tabs")).has_value(), "tabs.always off: one tab, no strip");
        const auto cx = rt.node_rect(dsl::part_id("corner", dsl::tab_part_window + 2));
        const float mm = hand::detail::minimap_w(rt.model());
        ok(cx && std::fabs(cx->right() - (rt.model().win.x - mm)) < 1.0f && cx->top() < 1.0f && cx->height() < 24.0f,
           "...the window's buttons sit small in the top-right corner, left of the scrollbar");
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm2) { mm2.minimap.width = 96.0f; }}});
        rt.tick();
        const auto cx2 = rt.node_rect(dsl::part_id("corner", dsl::tab_part_window + 2));
        ok(!hand::detail::minimap_on(rt.model())
               || (cx2 && cx2->right() <= rt.model().win.x - hand::detail::minimap_w(rt.model()) + 0.5f && cx2->right() < cx->right()),
           "...and move left as the scrollbar widens: never over it");
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm2) { mm2.minimap.width = 14.0f; }}});
        rt.tick();
        const int mins2 = rt.window().minimizes();
        rt.click(dsl::part_id("corner", dsl::tab_part_window + 0));
        ok(rt.window().minimizes() == mins2 + 1, "...and work");
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) { mm.cfg.tabs_always = true; }}});
        settle();
    }
    rt.window().drive_frames(false);
}

}  // namespace hand::cli::checks
