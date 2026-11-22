// hand::cli::checks — selection, copy mode, paste and its guard.

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

void selection([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── selection & text ────────────────────────────────────────────────
    using K = mayag::Key;
    using E5 = mayag::Edit<hand::App::Model>;
    rt.send(hand::App::Msg{E5{[](hand::App::Model& m) { m.tab().term->scroll_to_bottom(); m.tab().scroll_d = m.tab().scroll_goal = 0; m.tab().scroll_applied = 0; }}});
    rt.send(hand::App::Msg{hand::App::Output{std::string{
        "\x1b[2J\x1b[H\x1b]133;A\a$ \x1b]133;B\als\r\n\x1b]133;C\aalpha beta\r\ngamma delta\r\n\x1b]133;D;0\a"
        "\x1b]133;A\a$ \x1b]133;B\a"}}});
    rt.tick();
    const auto& mm = rt.model();
    const float ox = mm.metrics.origin_x, oy = mm.metrics.origin_y + mm.strip_h(), cw = mm.metrics.cell_w, chh = mm.metrics.cell_h;
    const auto at = [&](int col, int row) { return Vec2{ox + (static_cast<float>(col) + 0.3f) * cw, oy + (static_cast<float>(row) + 0.5f) * chh}; };
    // alt+drag: a block
    const auto ev = [&](mayag::Event e) { rt.window().push(std::move(e)); rt.tick(); };
    ev(mayag::MouseMove{at(1, 1), {}, {}});
    ev(mayag::MouseDown{at(1, 1), mayag::MouseButton::left, Mods{.alt = true}, 1});
    ev(mayag::MouseMove{at(3, 2), {}, Mods{.alt = true}});
    ev(mayag::MouseUp{at(3, 2), mayag::MouseButton::left, Mods{.alt = true}});
    ok(rt.model().tab().term->selection_info().block && rt.model().tab().term->selection_text() == "lph\namm",
       "alt+drag selects a block (a rectangle of columns)");
    // a plain click (no drag) selects nothing, so no toolbar pops up
    ev(mayag::MouseDown{at(2, 1), mayag::MouseButton::left, {}, 1});
    ev(mayag::MouseUp{at(2, 1), mayag::MouseButton::left, {}});
    for (int i = 0; i < 2; ++i) rt.tick();
    // the toolbar is in the drawn tree, or it is not shown
    const auto chip = [&] { return rt.tree().find(mayag::dsl::node_id("sel")) != nullptr; };
    ok(!rt.model().tab().term->has_selection() && !chip(), "a single click selects nothing and shows no toolbar");
    const auto none = [&] { return !rt.model().tab().term->has_selection() && !chip(); };
    ev(mayag::MouseDown{at(30, 5), mayag::MouseButton::left, {}, 1});   // empty space below the prompt
    ev(mayag::MouseMove{at(36, 5), {}, {}});
    ev(mayag::MouseUp{at(36, 5), mayag::MouseButton::left, {}});
    for (int i = 0; i < 2; ++i) rt.tick();
    ok(none(), "dragging over blank space selects nothing (no '1L 0ch' toolbar)");
    {
        const Vec2 a = at(2, 1), b{a.x + 0.6f * cw, a.y + 1.0f};   // a wobble that crosses a cell edge
        ev(mayag::MouseDown{a, mayag::MouseButton::left, {}, 1});
        ev(mayag::MouseMove{b, {}, {}});
        ev(mayag::MouseUp{b, mayag::MouseButton::left, {}});
        for (int i = 0; i < 2; ++i) rt.tick();
        ok(none(), "a click that wobbles a few pixels selects nothing");
    }
    ev(mayag::MouseDown{at(1, 1), mayag::MouseButton::left, {}, 1});
    ev(mayag::MouseMove{at(4, 1), {}, {}});
    ev(mayag::MouseUp{at(4, 1), mayag::MouseButton::left, {}});
    for (int i = 0; i < 2; ++i) rt.tick();
    ok(rt.model().tab().term->selection_text() == "lpha" && chip(),
       "a drag still selects, with its toolbar");
    rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.tab().term->select_begin(0, 0, 1, true); m2.tab().term->select_extend(3, 2); m2.tab().selecting = false; }}});
    {
        const auto bg2 = rt.model().tab().term->snapshot();
        std::string spans;
        for (const auto& sp : bg2.selection) spans += std::to_string(sp.row) + ":" + std::to_string(sp.col) + "+" + std::to_string(sp.len) + " ";
        (void)spans;
        ok(bg2.selection.size() == 3, "a three-row block is drawn on all three rows");
    }
    // a fourth click: the whole command's output
    rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.tab().term->select_clear(); }}});
    rt.window().multi_click(at(2, 1), 4);
    for (int i = 0; i < 3; ++i) rt.tick();
    ok(rt.model().tab().term->selection_text() == "alpha beta\ngamma delta", "a fourth click selects the command's whole output");
    ok(rt.node_rect(dsl::node_id("sel.copy")).has_value(), "a selection toolbar appears by it");
    rt.click(dsl::node_id("sel.copy"));
    ok(rt.window().get_clipboard() == "alpha beta\ngamma delta" && rt.model().time - rt.model().tab().copy_flash_at < 0.5,
       "its copy button copies, and the selection flashes");
    ok(rt.model().tab().term->has_selection(), "...and the selection stays");
    // shift+click extends
    // (the toolbar sits under the selection: shift+click past it, as a user would)
    rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.tab().term->select_begin(0, 0, 1); m2.tab().selecting = false; }}});
    ev(mayag::MouseDown{at(4, 2), mayag::MouseButton::left, Mods{.shift = true}, 1});
    ev(mayag::MouseUp{at(4, 2), mayag::MouseButton::left, Mods{.shift = true}});
    ok(rt.model().tab().term->selection_text() == "$ ls\nalpha beta\ngamma", "shift+click extends the selection");
    // copy mode
    rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.tab().term->select_clear(); }}});
    rt.press_key(K::m, Mods{.shift = true, .ctrl = true});
    ok(rt.model().tab().copy.has_value(), "ctrl+shift+m enters copy mode");
    rt.type("k"); rt.type("k"); rt.type("0");   // from the prompt up to "alpha beta"
    rt.type("w");
    rt.type("v"); rt.type("e");
    ok(rt.model().tab().term->selection_text() == "beta", "w / v / e select a word with the keyboard");
    rt.type("y");
    ok(!rt.model().tab().copy.has_value() && rt.window().get_clipboard() == "beta", "y yanks it and leaves copy mode");
    rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.tab().term->select_clear(); }}});
    rt.press_key(K::m, Mods{.shift = true, .ctrl = true});
    rt.type("k"); rt.type("V"); rt.type("k");
    ok(rt.model().tab().term->selection_text() == "alpha beta\ngamma delta", "V selects whole lines");
    rt.press_key(K::escape);
    ok(rt.model().tab().copy.has_value() && !rt.model().tab().term->has_selection(), "esc drops the selection first...");
    rt.press_key(K::escape);
    ok(!rt.model().tab().copy.has_value(), "...then copy mode");
    // paste guard
    rt.send(hand::App::Msg{hand::App::Pasted{"rm -rf a\nrm -rf b\n"}});
    ok(rt.model().dialog.ask && !rt.model().dialog.ask->paste.empty(), "a multi-line paste waits for confirmation");
    rt.press_key(K::escape);
    ok(!rt.model().dialog.ask, "esc cancels it");
    rt.send(hand::App::Msg{hand::App::Pasted{"echo one\necho two\n"}});
    rt.press_key(K::enter);
    ok(!rt.model().dialog.ask,
       "enter pastes it, and doesn't ask twice");
    rt.send(hand::App::Msg{hand::App::Pasted{"sudo true"}});
    ok((rt.model().dialog.ask && rt.model().dialog.ask->kind == hand::Ask::Kind::paste), "a single risky line asks too");
    rt.press_key(K::escape);
    rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[?2004h"}}});
    rt.send(hand::App::Msg{hand::App::Pasted{"a\nb"}});
    ok((rt.model().dialog.ask && rt.model().dialog.ask->kind == hand::Ask::Kind::paste), "multiline mode asks even with bracketed paste");
    rt.press_key(K::escape);
    rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.cfg.paste_confirm = hand::PasteConfirm::unbracketed; }}});
    rt.send(hand::App::Msg{hand::App::Pasted{"a\nb"}});
    ok(!rt.model().dialog.ask,
       "unbracketed mode: a program that asked for bracketed paste gets it straight away");
    rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.cfg.paste_confirm = hand::PasteConfirm::multiline; }}});
    rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[?2004l"}}});
}

}  // namespace hand::cli::checks
