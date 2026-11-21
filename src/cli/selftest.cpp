// hand::cli — the headless self-test (`hand --headless`, ctest app_headless):
// hand driven like a user would -- keys, clicks, drags, resizes, real shells
// on real ptys -- with every expectation printed as `ok` / `FAIL`. Returns the
// number of failures.

#include "cli.hpp"

#include "../app/detail.hpp"   // checks geometry against the same queries the app uses

#include <hand/app.hpp>
#include <hand/core/shell.hpp>
#include <hand/core/pty.hpp>


#include <filesystem>
#include <thread>
#include <chrono>

#include <mayag/widget/ssh.hpp>
#include <mayag/widget/file_pane.hpp>
#include <hand/config/schemes_gen.hpp>
#include <hand/ssh/sftp.hpp>

#include <mayag/backend/tiled.hpp>
#ifdef MAYAG_WITH_VULKAN
#include <mayag/backend/vulkan.hpp>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <hand/core/platform.hpp>
#include <csignal>

#if defined(_WIN32)
#  include <windows.h>   // --cat-v puts the console in raw mode
#else
#  include <termios.h>
#  include <unistd.h>
#endif

namespace hand::cli {

using namespace mayag;

int selftest(Runtime& rt, Vec2 size) {
    int fails = 0;
    const auto ok = [&](bool c, const char* what) { std::printf("  %s  %s\n", c ? "ok  " : "FAIL", what); fails += !c; };
    const auto& m = rt.model();
    ok(m.term->cols() > 20 && m.term->rows() > 5, "the grid fits the window");
    const auto g = m.term->snapshot();
    ok(g.lines[0].cells[0].text == "~" && !g.lines[0].cells[0].default_fg, "toe parsed colour output");
    ok(g.cursor_row == 6, "the cursor follows the output");
    rt.window().resize(Vec2{size.x / 2.0f, size.y / 2.0f});
    rt.tick();
    ok(rt.model().term->cols() < g.cols, "a resize reflows the grid");
    {
        // a real child on a real pty, through toe (bounded: ≤ 2 s)
        hand::Pty pty;
        hand::Term t{40, 5, hand::palette_of(hand::Config{})};
#if defined(_WIN32)
        // Our own binary as the child: it writes the SGR from its
        // own stdout (the one path conhost passes through intact)
        // and exits 7, so this stays a test of the pty and toe
        // rather than of a shell's escape handling.
        const bool up = pty.spawn(40, 5, hand::self_exe(), {}, {"--emit-red"});
        const char* cmd = "";
        const char* what = "ConPTY starts a child on a real pty";
#else
        const bool up = pty.spawn(40, 5, "/bin/sh");
        const char* cmd = "printf '\\033[31mred\\033[0m'; exit 7\n";
        const char* what = "forkpty starts a shell";
#endif
        ok(up, what);
        if (up) {
            pty.write(cmd);
            std::string buf;
            for (int i = 0; i < 40 && hand::pty_read(pty.fd(), buf, 50); ++i) {}
            (void)t.feed(buf);
            bool red = false;
            const auto sg = t.snapshot();
            for (const auto& l : sg.lines)
                for (const auto& c : l.cells) red = red || (c.text == "r" && c.fg == 0xE06C75);
            ok(red, "shell output reaches the grid in the palette's red");
            int code = -1;
            for (int i = 0; i < 40 && code < 0; ++i) { code = hand::pty_reap(pty.pid()); if (code < 0) std::this_thread::sleep_for(std::chrono::milliseconds(25)); }
            ok(code == 7, "the exit code comes back");
        }
    }
    {
        // ── the settings panel ──────────────────────────────────────────
        using K = mayag::Key;
        rt.window().resize(size);
        rt.tick();
        // ── links + search first, on a known screen ──────────────────
        rt.send(hand::App::Msg{hand::App::Output{std::string{
            "\x1b[2J\x1b[Hsee https://example.com/docs ok\r\n"
            "\x1b]8;;https://toe.dev\x1b\\osc8 link\x1b]8;;\x1b\\ and needle, needle\r\n"}}});
        rt.tick();
        ok(rt.model().term->link_at(8, 0) == "https://example.com/docs", "a bare URL is detected under the pointer");
        ok(rt.model().term->link_at(1, 1) == "https://toe.dev", "an OSC 8 hyperlink resolves to its target");
        ok(rt.model().term->link_at(0, 0).empty(), "plain text is not a link");
        rt.press_key(K::f, Mods{.shift = true, .ctrl = true});
        ok(rt.model().search_open, "ctrl+shift+f opens search");
        rt.type("needle");
        ok(rt.model().term->search_count() == 2, "typing finds every match in the scrollback");
        rt.tick();
        ok(rt.model().grid && rt.model().grid->matches.size() == 2 && rt.model().grid->current_match >= 0,
           "matches are highlighted, the current one marked");
        const auto cur = rt.model().term->search_current();
        rt.press_key(K::enter);
        ok(rt.model().term->search_current() != cur, "enter steps to the next match");
        rt.press_key(K::escape);
        ok(!rt.model().search_open && rt.model().term->search_count() == 0, "esc closes and clears it");
        rt.press_key(K::comma, Mods{.ctrl = true});
        ok(rt.model().settings.open, "ctrl+, opens settings");
        ok(rt.node_rect(dsl::node_id("settings.rows")).has_value(), "the mayag settings_panel is on screen");
        rt.type("j");   // vi keys: font.family -> font.size
        ok(rt.model().settings.ui.selected == 1, "j selects the next setting");
        const float before = rt.model().font_size;
        const int cols = rt.model().term->cols();
        rt.press_key(K::right, Mods{.shift = true});
        ok(rt.model().cfg.font_size == before + 5.0f && rt.model().font_size == before + 5.0f,
           "shift+right steps font.size ×10, live");
        ok(rt.model().term->cols() < cols, "and the grid refits to the bigger cells");
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
            rt.send(hand::App::Msg{hand::App::Blink{}});
            rt.tick();
            const float mid = rt.model().cur_a;
            for (int i = 0; i < 30; ++i) rt.tick();
            ok(mid > 0.0f && mid < 1.0f && (rt.model().cur_a == 0.0f || rt.model().cur_a == 1.0f), "the cursor blink fades, not snaps");
            rt.window().drive_frames(false);
        }
        {
            // ── the dropdown with the mouse ────────────────────────────────────
            rt.press_key(K::comma, Mods{.ctrl = true});
            rt.tick();
            const auto trig = dsl::node_id("settings.colors.scheme.select");
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
    {
        // ── EVERY setting: step it through the panel, check it took effect ──
        using K = mayag::Key;
        using SE = dsl::SettingEdit;
        const auto specs = hand::setting_specs(rt.model().cfg);
        int changed = 0, total = 0;
        std::string stuck;
        for (std::size_t i = 0; i < specs.size(); ++i) {
            const auto& s = specs[i];
            if (s.kind == dsl::SettingSpec::Kind::cycle && s.options.size() < 2) continue;   // one font
            ++total;
            const hand::Config before = rt.model().cfg;
            const bool at_top = s.kind == dsl::SettingSpec::Kind::number && s.value >= s.hi;
            const int delta = (s.kind == dsl::SettingSpec::Kind::choice || s.kind == dsl::SettingSpec::Kind::cycle) ? 1 : at_top ? -3 : 3;
            rt.send(hand::App::Msg{hand::App::Panel{dsl::SettingsOp{[i, delta](dsl::SettingsPanelState& x) {
                x.out.push_back({SE::Op::step, i, 0.0f, delta});
            }}}});
            rt.tick();
            const auto after = hand::setting_specs(rt.model().cfg)[i];
            if (after.value != hand::setting_specs(before)[i].value) ++changed;
            else stuck += " " + s.section + "." + s.key;
        }
        if (!stuck.empty()) std::printf("    did not change:%s\n", stuck.c_str());
        ok(changed == total, "every setting changes through the panel and reads back as modified");
        const auto& mm = rt.model();
        const hand::Config d{};
        ok(mm.font_size == mm.cfg.font_size && mm.font_size != d.font_size, "font.size reaches the grid");
        ok(mm.metrics.cell_h == std::round(mm.font_size * mm.cfg.line_height), "font.line_height sets the row height");
        ok(mm.metrics.origin_x == mm.cfg.padding + (mm.cfg.shell_marks ? 10.0f : 0.0f) && mm.cfg.padding != d.padding, "window.padding moves the grid");
        ok(mm.palette.bg != hand::palette_of(d).bg, "colors.scheme recolours the terminal");
        ok(mm.grid && mm.grid->cursor_shape != dsl::TermCursorShape::block, "cursor.style reaches the cursor");
        ok(!mm.cfg.cursor_blink && !mm.cfg.visual_bell && mm.cfg.copy_on_select && !mm.cfg.links_need_ctrl,
           "the toggles flip");
        // reset all: back to every default
        rt.send(hand::App::Msg{hand::App::Panel{dsl::SettingsOp{[](dsl::SettingsPanelState& x) {
            x.out.push_back({SE::Op::reset_all, 0, 0.0f, 0});
        }}}});
        rt.tick();
        std::size_t mods = 0;
        for (const auto& s : hand::setting_specs(rt.model().cfg)) mods += s.modified;
        ok(mods == 0 && rt.model().font_size == d.font_size && rt.model().palette.bg == hand::palette_of(d).bg,
           "reset all restores every default, live");
        (void)K::escape;
    }
    {
        // ── the visual settings, checked on actual PIXELS ────────────────────
        using E = mayag::Edit<hand::App::Model>;
        const auto px_at = [&](float x, float y) {
            rt.tick();
            const auto px = rt.window().read_pixels();
            const int W = static_cast<int>(rt.window().size().x * rt.window().dpi_scale());
            const auto i = (static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x)) * 4;
            return std::array<int, 4>{px[i], px[i + 1], px[i + 2], px[i + 3]};
        };
        const float bx = size.x - 4.0f, by = size.y - 4.0f;   // bare background, bottom-right
        const auto opaque = px_at(bx, by);
        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.opacity = 0.5f; }}});
        const auto half = px_at(bx, by);
        std::printf("    alpha: opaque=%d  opacity 0.5=%d\n", opaque[3], half[3]);
        ok(opaque[3] == 255 && half[3] > 110 && half[3] < 145, "window.opacity reaches the pixels (alpha ~50%)");
        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.opacity = 1.0f; m.cfg.dim_unfocused = 0.6f; }}});
        const auto lit = px_at(bx, by);
        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.focused = false; }}});
        const auto dim = px_at(bx, by);
        ok(dim[0] + dim[1] + dim[2] < lit[0] + lit[1] + lit[2], "colors.dim_unfocused darkens an unfocused window");
        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.focused = true; m.cfg.dim_unfocused = 0.0f; }}});
        rt.send(hand::App::Msg{hand::App::Output{std::string{"\a"}}});
        const auto flash = px_at(bx, by);
        ok(flash[0] + flash[1] + flash[2] > lit[0] + lit[1] + lit[2], "bell.visual flashes the screen on BEL");
        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.visual_bell = false; m.bell_at = -1.0; }}});
        rt.send(hand::App::Msg{hand::App::Output{std::string{"\a"}}});
        const auto quiet = px_at(bx, by);
        ok(quiet == lit, "and stays dark with it off");
        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.visual_bell = true; }}});
    }
    {
        // ── the cool stuff ───────────────────────────────────────────────────
        using K = mayag::Key;
        using E = mayag::Edit<hand::App::Model>;
        const auto feed = [&](std::string s) { rt.send(hand::App::Msg{hand::App::Output{std::move(s)}}); rt.tick(); };
        feed("\x1b[2J\x1b[H");
        // two commands through OSC 133, as the injected script emits them
        feed("\x1b]133;A\a\x1b[32m\u276F\x1b[0m \x1b]133;B\amake\r\n\x1b]133;C\abuilding...\r\nerror: oops\r\n\x1b]133;D;2\a");
        feed("\x1b]133;A\a\u276F \x1b]133;B\agit log -1\r\n\x1b]133;C\acommit 3f9a2c1d see https://toe.dev/x and ~/src/hand/main.cpp\r\n\x1b]133;D;0\a");
        feed("\x1b]133;A\a\u276F \x1b]133;B\a");
        const auto cmds = rt.model().term->commands();
        int ran = 0, failed = 0;
        for (const auto& c : cmds) { ran += c.ran; failed += c.finished && c.exit_code != 0; }
        ok(ran == 2 && failed == 1, "OSC 133 commands are recorded, with exit codes");
        ok(rt.model().term->command_text(cmds[0]) == "make", "the typed command line is recovered");
        const auto sg = rt.model().term->snapshot();
        ok(sg.lines[0].mark == dsl::TermMark::error && sg.lines[3].mark == dsl::TermMark::ok,
           "failed and succeeded prompts get their marks");
        rt.press_key(K::g, Mods{.shift = true, .ctrl = true});
        ok(rt.window().get_clipboard().find("commit 3f9a2c1d") != std::string::npos, "ctrl+shift+g copies the last output");
        // hints
        rt.press_key(K::space, Mods{.shift = true, .ctrl = true});
        bool url = false, path = false, hash = false;
        for (const auto& h : rt.model().hint->hints) {
            url = url || h.text == "https://toe.dev/x";
            path = path || h.text == "~/src/hand/main.cpp";
            hash = hash || h.text == "3f9a2c1d";
        }
        ok(rt.model().hint.has_value() && url && path && hash, "ctrl+shift+space labels the URL, the path and the hash");
        std::size_t hi = 0;
        while (hi < rt.model().hint->hints.size() && rt.model().hint->hints[hi].text != "3f9a2c1d") ++hi;
        const std::string label = rt.model().hint && hi < rt.model().hint->labels.size() ? rt.model().hint->labels[hi] : "?";
        rt.type(label);
        ok(!rt.model().hint.has_value() && rt.window().get_clipboard() == "3f9a2c1d", "typing its label copies it");
        rt.window().drive_frames(true);
        for (int i = 0; i < 30; ++i) rt.tick();
        feed("\r\n");
        {
            std::string fill;   // fill to the bottom so the next line scrolls
            for (int i = 0; i < 80; ++i) fill += "x\r\n";
            feed(fill);
        }
        for (int i = 0; i < 40; ++i) rt.tick();
        feed("\r\n");
        ok(rt.model().out_shift > 0.0f, "Enter at the live prompt glides the screen up a line");
        for (int i = 0; i < 40; ++i) rt.tick();
        ok(rt.model().out_shift == 0.0f, "...and settles");
        rt.window().drive_frames(false);
        {
            // ── motion: every effect fires on a typed move, then clears; spring overshoots
            using E2 = mayag::Edit<hand::App::Model>;
            rt.window().drive_frames(true);
            const char* names[] = {"ripple", "sonicboom", "wireframe", "pixiedust", "railgun", "torpedo", "fireworks"};
            bool all = true;
            for (int fx = 1; fx <= 7; ++fx) {
                rt.send(hand::App::Msg{E2{[fx](hand::App::Model& m) { m.cfg.cursor_fx = static_cast<hand::CursorFx>(fx); m.fx.ps.clear(); }}});
                rt.type("x");                                          // the keystroke...
                if (fx == 7) rt.press_key(K::enter);
                feed(fx == 7 ? "x\r\n" : "x");                       // ...and its echo moves the cursor
                rt.tick();
                const bool fired = rt.model().fx.busy();
                for (int i = 0; i < 90; ++i) rt.tick();
                const bool cleared = !rt.model().fx.busy();
                if (!fired || !cleared) { std::printf("    effect %s: fired=%d cleared=%d\n", names[fx - 1], fired, cleared); all = false; }
            }
            ok(all, "all seven cursor effects fire on a typed move and clear away");
            rt.send(hand::App::Msg{E2{[](hand::App::Model& m) {
                m.cfg.cursor_fx = hand::CursorFx::none; m.cfg.cursor_anim = hand::CursorAnim::spring; }}});
            for (int i = 0; i < 30; ++i) rt.tick();
            const int c0 = rt.model().term->cursor().col;
            feed("\x1b[" + std::to_string(c0 + 11) + "G");      // jump the cursor 10 cells right
            float peak = 0.0f;
            for (int i = 0; i < 60; ++i) { rt.tick(); peak = std::max(peak, rt.model().cur_x); }
            const float goal = static_cast<float>(rt.model().term->cursor().col);
            ok(peak > goal + 0.3f && std::fabs(rt.model().cur_x - goal) < 0.02f, "the spring cursor overshoots, then settles");
            {   // motion.trail: a longer tail lags further behind the head
                const auto lag = [&](float len) {
                    rt.send(hand::App::Msg{E2{[len](hand::App::Model& m) {
                        m.cfg.cursor_anim = hand::CursorAnim::glide; m.cfg.cursor_trail = len; }}});
                    for (int i = 0; i < 60; ++i) rt.tick();
                    const int c1 = rt.model().term->cursor().col;
                    feed("\x1b[" + std::to_string(c1 > 20 ? 1 : c1 + 21) + "G");
                    for (int i = 0; i < 6; ++i) rt.tick();
                    return std::fabs(rt.model().fx.tx - rt.model().cur_x);
                };
                const float short_ = lag(0.3f), long_ = lag(4.0f);
                ok(long_ > short_ * 1.5f, "motion.trail sets how far the tail lags (longer = a bigger comet)");
                for (int i = 0; i < 90; ++i) rt.tick();
                ok(motion_settled(rt.model()), "...and it always catches up");
            }
            rt.send(hand::App::Msg{E2{[](hand::App::Model& m) { m.cfg.cursor_anim = hand::CursorAnim::glide; m.cfg.bell_shake = true; }}});
            feed("\a");
            ok(rt.model().bell_at >= 0.0, "BEL with shake on is seen");
            rt.send(hand::App::Msg{E2{[](hand::App::Model& m) { m.cfg.bell_shake = false; }}});
            rt.window().drive_frames(false);
        }
        // smooth cursor
        rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.cursor_anim = hand::CursorAnim::glide; m.cur_known = true; m.cur_x = 0; m.cur_y = 0; }}});
        rt.window().drive_frames(true);   // the frame clock runs, as on screen
        feed("\x1b[10;30H");
        const float mid = rt.model().cur_x;
        for (int i = 0; i < 40; ++i) rt.tick();
        rt.window().drive_frames(false);
        ok(mid < 29.0f && std::fabs(rt.model().cur_x - 29.0f) < 0.05f && std::fabs(rt.model().cur_y - 9.0f) < 0.05f,
           "the cursor glides to its cell and settles");
        // minimap + prompt jumps
        std::string many;
        for (int i = 0; i < 200; ++i) many += "\x1b]133;A\a$ \x1b]133;B\aecho " + std::to_string(i) + "\r\n\x1b]133;C\a" + std::to_string(i) + "\r\n\x1b]133;D;0\a";
        feed(many);
        ok(rt.node_rect(dsl::node_id("minimap")).has_value(), "the minimap is on screen");
        rt.window().drive_frames(true);
        const auto settle = [&] { for (int i = 0; i < 60; ++i) rt.tick(); };
        rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
        rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
        rt.tick();
        const float smid = rt.model().scroll_d;
        settle();
        ok(rt.model().term->scroll_offset() > 0 && rt.model().scroll_d == rt.model().scroll_goal,
           "ctrl+shift+z glides back to earlier prompts");
        ok(smid > 0.0f && smid < rt.model().scroll_goal, "...through every pixel in between");
        ok(rt.node_rect(dsl::node_id("tolive")).has_value(), "scrolled back, a pill offers the way home");
        ok(!rt.node_rect(dsl::node_id("sticky")).has_value(), "on a prompt, no header (the prompt is right there)");
        rt.press_key(K::down, Mods{.shift = true, .ctrl = true});   // one line into its output
        settle();
        ok(rt.model().sticky_t == 1.0f && rt.node_rect(dsl::node_id("sticky")).has_value(),
           "the command you are inside slides in as a sticky header");
        {
            const float before = rt.model().scroll_goal;
            rt.click(dsl::node_id("sticky"));
            settle();
            ok(rt.model().scroll_goal != before, "clicking the header flies to that command's prompt");
        }
        {
            const auto g2 = rt.model().grid;   // mid-glide frames carry the row above
            rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
            rt.tick();
            ok(rt.model().scroll_d != std::floor(rt.model().scroll_d) && rt.model().grid->rows == rt.model().term->rows() + 1,
               "mid-glide the grid is a row taller and slides by sub-row amounts");
            (void)g2;
            settle();
        }
        rt.send(hand::App::Msg{hand::App::Output{std::string{"\r\nnew 1\r\nnew 2\r\nnew 3"}}});
        rt.tick();
        ok(rt.model().new_below >= 3, "output arriving while you read is counted, not shoved at you");
        rt.click(dsl::node_id("tolive"));
        settle();
        ok(rt.model().term->scroll_offset() == 0 && rt.model().new_below == 0, "the pill glides you back to live");
        for (int i = 0; i < 3; ++i) rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
        settle();
        for (int i = 0; i < 5; ++i) rt.press_key(K::x, Mods{.shift = true, .ctrl = true});
        settle();
        ok(rt.model().term->scroll_offset() == 0, "ctrl+shift+x walks forward to the live prompt");
        // the live tail: scrolled back, the last live rows slide in at the bottom
        rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
        rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
        rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
        settle();
        ok(rt.model().tail_t == 1.0f && rt.node_rect(dsl::node_id("tail")).has_value(),
           "scrolled back, the live tail slides in at the bottom");
        rt.click(dsl::node_id("tail"));
        settle();
        ok(rt.model().term->scroll_offset() == 0 && rt.model().tail_t == 0.0f, "clicking it glides home and it slides away");
        // the peek: hover the minimap over an old command
        rt.window().move_to(Vec2{size.x - 6.0f, size.y * 0.2f});
        settle();
        ok(rt.model().minimap.hover_y > 0.0f && rt.model().minimap.peek_t == 1.0f, "hovering the minimap brings up the peek");
        ok(rt.node_rect(dsl::node_id("peek.grid")).has_value(), "with the lines from that part of history");
        rt.window().move_to(Vec2{size.x * 0.5f, size.y * 0.5f});
        settle();
        ok(rt.model().minimap.peek_t == 0.0f, "and it fades when the pointer leaves");
        {
            // scrolling back WHILE output streams (a build, `yes`, tail -f)
            int line = 0;
            const auto stream = [&](int n) {
                for (int k = 0; k < n; ++k) {
                    std::string chunk;
                    for (int j = 0; j < 3; ++j) chunk += "stream " + std::to_string(line++) + "\r\n";
                    rt.send(hand::App::Msg{hand::App::Output{std::move(chunk)}});
                    rt.tick();
                }
            };
            stream(10);
            for (int k = 0; k < 30; ++k) {   // a trackpad: small deltas, output between every one
                rt.window().scroll(Vec2{size.x * 0.5f, size.y * 0.5f}, Vec2{0.0f, 3.0f});
                stream(1);
            }
            stream(20);
            const int off = rt.model().term->scroll_offset();
            ok(off >= 5, "the wheel scrolls back while output keeps streaming");
            const auto top = [&] {
                std::string s;
                const auto g = rt.model().term->snapshot();
                for (const auto& c : g.lines[0].cells) s += c.text;
                return s;
            };
            const std::string a = top();
            stream(20);
            ok(top() == a, "and what you are reading stays still as more arrives");
            ok(rt.model().new_below > 0, "...counted as new below");
            // drag-select past the top edge: the view scrolls with the selection
            const int off2 = rt.model().term->scroll_offset();
            rt.window().press(Vec2{60.0f, size.y * 0.5f});
            rt.tick();
            rt.window().move_to(Vec2{80.0f, 2.0f});
            for (int k = 0; k < 40; ++k) rt.tick();
            rt.window().release(Vec2{80.0f, 2.0f});
            rt.tick();
            ok(rt.model().term->scroll_offset() > off2 && rt.model().term->has_selection(),
               "dragging a selection past the top edge scrolls history with it");
            rt.press_key(K::up, Mods{.shift = true, .ctrl = true});
            settle();
            rt.press_key(K::end, Mods{.shift = true});
            settle();
            ok(rt.model().term->scroll_offset() == 0, "shift+end lands back on the live view");
        }
        {
            // a full-screen app (alternate screen): no prompt marks, no minimap
            rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[?1049h\x1b[2J\x1b[H"}}});
            for (int r = 0; r < 20; ++r)
                rt.send(hand::App::Msg{hand::App::Output{"\x1b[" + std::to_string(r + 1) + ";1Hmatrix " + std::to_string(r)}});
            rt.tick();
            int marks = 0;
            for (const auto& l : rt.model().grid->lines) marks += l.mark != dsl::TermMark::none ? 1 : 0;
            ok(marks == 0, "a full-screen app (cmatrix, vim) gets no prompt marks");
            ok(!rt.node_rect(dsl::node_id("minimap")).has_value(), "...and no minimap");
            rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[?1049l"}}});
            rt.tick();
            ok(rt.node_rect(dsl::node_id("minimap")).has_value(), "back at the shell, both return");
        }
        rt.window().drive_frames(false);
    }
    {
        // ── the injected script, in a REAL fish (bounded) ───────────────────
        if (::access("/usr/bin/fish", X_OK) == 0) {
            hand::Pty pty;
            const auto si = hand::shell_integration("/usr/bin/fish");
            hand::Term t{80, 10, hand::palette_of(hand::Config{})};
            t.set_prompt_protect(!si.reflow_aware);
            const bool up = pty.spawn(80, 10, "/usr/bin/fish", si.env, si.args);
            std::string buf;
            for (int i = 0; i < 40 && hand::pty_read(pty.fd(), buf, 50); ++i) {
                const auto fx = t.feed(buf); buf.clear();
                if (!fx.reply.empty()) pty.write(fx.reply);
                if (i == 10) pty.write("false\r");
                if (i == 20) pty.write("exit\r");
            }
            (void)t.feed(buf);
            int ran = 0, failed = 0;
            for (const auto& c : t.commands()) { ran += c.ran; failed += c.finished && c.exit_code == 1; }
            std::printf("    fish: %d command(s) recorded, %d failed\n", ran, failed);
            ok(up && ran >= 1 && failed >= 1, "fish is integrated without touching its config");
        }
    }
    {
        // ── resize never corrupts: real fish, a drag of sizes (bounded) ───────
        if (::access("/usr/bin/fish", X_OK) == 0) {
            hand::Pty pty;
            const auto si = hand::shell_integration("/usr/bin/fish");
            hand::Term t{80, 24, hand::palette_of(hand::Config{})};
            t.set_prompt_protect(!si.reflow_aware);
            const bool up = pty.spawn(80, 24, "/usr/bin/fish", si.env, si.args);
            const auto pump = [&](int ms) {
                std::string buf;
                for (int k = 0; k < ms / 50 && hand::pty_read(pty.fd(), buf, 50); ++k) {
                    const auto fx = t.feed(buf); buf.clear();
                    if (!fx.reply.empty()) pty.write(fx.reply);
                }
            };
            pump(1200);
            pty.write("seq 1 12\r");   // fish redraws its prompt after; then wait for idle
            pump(1500);
            const auto text = [&] {
                std::vector<std::string> v;
                for (const auto& l : t.snapshot().lines) {
                    std::string s;
                    for (const auto& c : l.cells) s += c.text;
                    while (!s.empty() && s.back() == ' ') s.pop_back();
                    v.push_back(s);
                }
                return v;
            };
            const auto before = text();
            for (int k = 0; k < 30; ++k) {   // a drag: grow and shrink in both directions, stay readable (>= 50 cols)
                const int c = 50 + (k * 37) % 50, r = 6 + (k * 13) % 26;
                t.resize(c, r);
                pty.resize(c, r);
                if (k % 3 == 0) pump(50);
            }
            t.resize(80, 24);
            pty.resize(80, 24);
            pump(700);
            const auto after = text();
            // what counts: the 12 numbers once each, ONE prompt, and no blank gap
            // between content (empty screen below the prompt is just screen)
            const auto audit = [](const std::vector<std::string>& v, int& nums, int& prompts, int& gap) {
                nums = prompts = gap = 0;
                std::size_t last = 0;
                for (std::size_t i = 0; i < v.size(); ++i) if (!v[i].empty()) last = i;
                int run = 0;
                for (std::size_t i = 0; i <= last && i < v.size(); ++i) {
                    const auto& l = v[i];
                    bool num = !l.empty();
                    for (char ch : l) num = num && ch >= '0' && ch <= '9';
                    nums += num ? 1 : 0;
                    prompts += l.find("\u276F seq 1 12") != std::string::npos ? 1 : 0;
                    run = l.empty() ? run + 1 : 0;
                    gap = std::max(gap, run);
                }
            };
            int seq_lines = 0, prompt_cmds = 0, worst = 0;
            audit(after, seq_lines, prompt_cmds, worst);
            if (seq_lines != 12 || prompt_cmds != 1 || worst > 2)
                for (std::size_t i = 0; i < after.size(); ++i) std::printf("    %2zu|%s\n", i, after[i].c_str());
            ok(up && seq_lines == 12 && prompt_cmds == 1, "a 30-step resize drag keeps every line, once");
            ok(worst <= 2, "no blank gaps open up between output and prompt");
            // narrower than the prompt: it wraps, then must come back whole
            for (int c : {28, 22, 36, 80}) { t.resize(c, 24); pty.resize(c, 24); pump(250); }
            const auto narrow = text();
            int nseq = 0, nprompt = 0, ngap = 0;
            audit(narrow, nseq, nprompt, ngap);
            if (nseq != 12 || nprompt != 1 || ngap > 2)
                for (std::size_t i = 0; i < narrow.size(); ++i) std::printf("    %2zu|%s\n", i, narrow[i].c_str());
            ok(nseq == 12 && nprompt == 1 && ngap <= 2, "squeezed narrower than the prompt and back: output intact");
            int same = 0;
            for (std::size_t i = 0; i < std::min(before.size(), after.size()); ++i) same += before[i] == after[i] ? 1 : 0;
            if (same < static_cast<int>(before.size()) - 3)
                for (std::size_t i = 0; i < std::min(before.size(), after.size()); ++i)
                    if (before[i] != after[i]) std::printf("    %2zu before|%s\n       after |%s\n", i, before[i].c_str(), after[i].c_str());
            ok(same >= static_cast<int>(before.size()) - 3, "back at the same size, the screen reads the same");
            int marks = 0;
            for (const auto& c : t.commands()) marks += c.ran && c.prompt_row >= 0 ? 1 : 0;
            ok(marks >= 1, "command marks follow the rewrap");
            pty.write("exit\r");
            pump(200);
        }
    }
    {
        // ── wide output, squeezed thin and back, in a real fish: no gap ────────
        if (::access("/usr/bin/fish", X_OK) == 0) {
            hand::Pty pty;
            const auto si = hand::shell_integration("/usr/bin/fish");
            hand::Term t{100, 30, hand::palette_of(hand::Config{})};
            t.set_prompt_protect(!si.reflow_aware);
            if (pty.spawn(100, 30, "/usr/bin/fish", si.env, si.args)) {
                const auto pump = [&](int ms) {
                    std::string buf;
                    for (int k = 0; k < ms / 50 && hand::pty_read(pty.fd(), buf, 50); ++k) {
                        const auto fx = t.feed(buf); buf.clear();
                        if (!fx.reply.empty()) pty.write(fx.reply);
                    }
                };
                pump(1200);
                pty.write("clear; for i in (seq 14); printf '      line %02d %s\\n' $i (string repeat -n 70 x); end\r");
                pump(900);
                for (int k = 0; k < 3; ++k) {
                    for (int c = 100; c >= 28; c -= 3) { t.resize(c, 30); pty.resize(c, 30); }
                    pump(50);
                    for (int c = 28; c <= 100; c += 3) { t.resize(c, 30); pty.resize(c, 30); }
                    pump(50);
                }
                pump(800);
                const auto sg = t.snapshot();
                int last = -1, lines = 0;
                for (int r = 0; r < t.cursor().row; ++r) {
                    std::string l;
                    for (const auto& c : sg.lines[static_cast<std::size_t>(r)].cells) l += c.text;
                    if (l.find("line ") != std::string::npos) { ++lines; last = r; }
                }
                // the prompt may be two lines (info + ❯): at most one row between
                const int gap = t.cursor().row - last - 1;
                std::printf("    thin+wide: %d output lines, %d rows to the prompt\n", lines, gap);
                ok(lines == 14 && gap <= 2, "squeezed thin and back: no gap opens above the prompt");
                pty.write("exit\r");
                pump(200);
            }
        }
    }
    {
        // ── toe: a real space at a wrap point survives a rewrap ─────────────
        hand::Term t{10, 5, hand::palette_of(hand::Config{})};
        (void)t.feed("12345 789 abcdefg");   // wraps right after a space
        t.resize(40, 5);
        std::string row;
        const auto snap = t.snapshot();
        for (const auto& c : snap.lines[0].cells) row += c.text;
        while (!row.empty() && row.back() == ' ') row.pop_back();
        ok(row == "12345 789 abcdefg", "spaces at wrap points are text, not padding");
    }
    {
        // ── selection & text ────────────────────────────────────────────────
        using K = mayag::Key;
        using E5 = mayag::Edit<hand::App::Model>;
        rt.send(hand::App::Msg{E5{[](hand::App::Model& m) { m.term->scroll_to_bottom(); m.scroll_d = m.scroll_goal = 0; m.scroll_applied = 0; }}});
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
        ok(rt.model().term->selection_info().block && rt.model().term->selection_text() == "lph\namm",
           "alt+drag selects a block (a rectangle of columns)");
        // a plain click (no drag) selects nothing, so no toolbar pops up
        ev(mayag::MouseDown{at(2, 1), mayag::MouseButton::left, {}, 1});
        ev(mayag::MouseUp{at(2, 1), mayag::MouseButton::left, {}});
        for (int i = 0; i < 2; ++i) rt.tick();
        ok(!rt.model().term->has_selection() && rt.model().sel_chip.size.x == 0.0f,
           "a single click selects nothing and shows no toolbar");
        const auto none = [&] { return !rt.model().term->has_selection() && rt.model().sel_chip.size.x == 0.0f; };
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
        ok(rt.model().term->selection_text() == "lpha" && rt.model().sel_chip.size.x > 0.0f,
           "a drag still selects, with its toolbar");
        rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.term->select_begin(0, 0, 1, true); m2.term->select_extend(3, 2); m2.selecting = false; }}});
        {
            const auto bg2 = rt.model().term->snapshot();
            std::string spans;
            for (const auto& sp : bg2.selection) spans += std::to_string(sp.row) + ":" + std::to_string(sp.col) + "+" + std::to_string(sp.len) + " ";
            (void)spans;
            ok(bg2.selection.size() == 3, "a three-row block is drawn on all three rows");
        }
        // a fourth click: the whole command's output
        rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.term->select_clear(); }}});
        rt.window().multi_click(at(2, 1), 4);
        for (int i = 0; i < 3; ++i) rt.tick();
        ok(rt.model().term->selection_text() == "alpha beta\ngamma delta", "a fourth click selects the command's whole output");
        ok(rt.node_rect(dsl::node_id("sel.copy")).has_value(), "a selection toolbar appears by it");
        rt.click(dsl::node_id("sel.copy"));
        ok(rt.window().get_clipboard() == "alpha beta\ngamma delta" && rt.model().time - rt.model().copy_flash_at < 0.5,
           "its copy button copies, and the selection flashes");
        ok(rt.model().term->has_selection(), "...and the selection stays");
        // shift+click extends
        // (the toolbar sits under the selection: shift+click past it, as a user would)
        rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.term->select_begin(0, 0, 1); m2.selecting = false; }}});
        ev(mayag::MouseDown{at(4, 2), mayag::MouseButton::left, Mods{.shift = true}, 1});
        ev(mayag::MouseUp{at(4, 2), mayag::MouseButton::left, Mods{.shift = true}});
        ok(rt.model().term->selection_text() == "$ ls\nalpha beta\ngamma", "shift+click extends the selection");
        // copy mode
        rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.term->select_clear(); }}});
        rt.press_key(K::m, Mods{.shift = true, .ctrl = true});
        ok(rt.model().copy.has_value(), "ctrl+shift+m enters copy mode");
        rt.type("k"); rt.type("k"); rt.type("0");   // from the prompt up to "alpha beta"
        rt.type("w");
        rt.type("v"); rt.type("e");
        ok(rt.model().term->selection_text() == "beta", "w / v / e select a word with the keyboard");
        rt.type("y");
        ok(!rt.model().copy.has_value() && rt.window().get_clipboard() == "beta", "y yanks it and leaves copy mode");
        rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.term->select_clear(); }}});
        rt.press_key(K::m, Mods{.shift = true, .ctrl = true});
        rt.type("k"); rt.type("V"); rt.type("k");
        ok(rt.model().term->selection_text() == "alpha beta\ngamma delta", "V selects whole lines");
        rt.press_key(K::escape);
        ok(rt.model().copy.has_value() && !rt.model().term->has_selection(), "esc drops the selection first...");
        rt.press_key(K::escape);
        ok(!rt.model().copy.has_value(), "...then copy mode");
        // paste guard
        rt.send(hand::App::Msg{hand::App::Pasted{"rm -rf a\nrm -rf b\n"}});
        ok(!rt.model().pending_paste.empty(), "a multi-line paste waits for confirmation");
        rt.press_key(K::escape);
        ok(rt.model().pending_paste.empty(), "esc cancels it");
        rt.send(hand::App::Msg{hand::App::Pasted{"echo one\necho two\n"}});
        rt.press_key(K::enter);
        ok(rt.model().pending_paste.empty() && !rt.model().dialog.ask,
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
        ok(rt.model().pending_paste.empty() && !rt.model().dialog.ask,
           "unbracketed mode: a program that asked for bracketed paste gets it straight away");
        rt.send(hand::App::Msg{E5{[](hand::App::Model& m2) { m2.cfg.paste_confirm = hand::PasteConfirm::multiline; }}});
        rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[?2004l"}}});
    }
    {
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
        const int rows1 = rt.model().term->rows();
        rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[2J\x1b[HTAB-ONE"}}});
        rt.press_key(K::t, Mods{.shift = true, .ctrl = true});
        settle();
        ok(rt.model().tab_count() == 2 && rt.model().active == 1, "ctrl+shift+t opens a tab and switches to it");
        ok(rt.model().strip_h() > 30.0f && rt.node_rect(dsl::node_id("tabs")).has_value(), "the tab strip slides in");
        ok(rt.model().term->rows() < rows1, "and every grid refits below it");
        rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[2J\x1b[HTAB-TWO"}}});
        const auto top = [&] {   // the whole visible screen, as one string
            std::string s0;
            const auto g = rt.model().term->snapshot();
            for (const auto& l : g.lines) for (const auto& c : l.cells) s0 += c.text;
            return s0;
        };
        const auto has = [&](const char* w) { return top().find(w) != std::string::npos; };
        ok(has("TAB-TWO") && !has("TAB-ONE"), "the new tab has its own screen");
        rt.press_key(K::tab, Mods{.ctrl = true});
        settle();
        ok(rt.model().active == 0 && has("TAB-ONE") && !has("TAB-TWO"), "ctrl+tab switches back, screen intact");
        // output for a background tab lands there, and marks it
        const hand::SessionId bg = rt.model().tabs[1].sid;
        rt.send(hand::App::Msg{hand::App::Output{std::string{"\r\nbackground!"}, bg}});
        rt.tick();
        ok(rt.model().tabs[1].unread && !has("background!"), "background output goes to its tab and marks it unread");
        rt.press_key(K::n2, Mods{.alt = true});
        settle();
        ok(rt.model().active == 1 && !rt.model().unread, "alt+2 jumps to it, and it is read");
        rt.press_key(K::t, Mods{.shift = true, .ctrl = true});
        settle();
        rt.press_key(K::page_up, Mods{.shift = true, .ctrl = true});
        settle();
        ok(rt.model().tab_count() == 3 && rt.model().active == 1, "ctrl+shift+pgup moves the tab left");
        rt.click(dsl::part_id("tabs", 0));
        settle();
        ok(rt.model().active == 0, "clicking a tab selects it");
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
        ok(rt.model().tab_count() == 1 && rt.model().strip_h() == 0.0f && rt.model().term->rows() == rows1,
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
    {
        // ── the mouse in a TUI: hover (1003) and drags (1002) reach it ──
        // A real program on a real pty: `hand --cat-v` goes raw and shows
        // every byte it is sent, so the test reads what the app received.
        // Using our own binary keeps this identical on every platform.
        auto pty = std::make_shared<hand::Pty>();
        const bool up = pty->spawn(rt.model().term->cols(), rt.model().term->rows(),
                                   hand::self_exe(), {}, {"--cat-v"});
        std::string got;
        // the app's own pty reader feeds the echo into the terminal:
        // read it back from the screen
        const auto scrape = [&] {
            got.clear();
            const auto g = rt.model().term->snapshot();
            for (const auto& l : g.lines) for (const auto& c : l.cells) got += c.text;
        };
        // Wait for what we EXPECT rather than for a fixed time: a
        // pty round trip through a real child is at the mercy of
        // the scheduler, and a loaded machine (a parallel build,
        // CI) makes a fixed sleep flaky. `ms` stays the ceiling.
        const auto drain_until = [&](int ms, std::string_view want) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            do {
                rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(20));
                rt.tick();
                scrape();
                if (!want.empty() && got.find(want) != std::string::npos) return;
            } while (std::chrono::steady_clock::now() < deadline);
        };
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[pty](hand::App::Model& mm) {
            mm.dialog.ask.reset(); mm.dialog.t = 0.0f; mm.settings.open = false; mm.ssh.picker.open = false;
            mm.selecting = false; mm.mouse_button = -1; mm.pty = pty; }}});
        for (int i = 0; i < 30; ++i) rt.tick();   // any closing overlay is gone
        drain_until(2000, "ready");
        ok(up && rt.model().term->mouse_reporting(), "a TUI turns on any-motion mouse tracking");
        const float top = rt.model().strip_h(), cw = rt.model().metrics.cell_w, chh = rt.model().metrics.cell_h;
        const float ox = rt.model().metrics.origin_x, oy = rt.model().metrics.origin_y + top;
        const auto at = [&](int c, int r) {
            return mayag::Vec2{ox + (static_cast<float>(c) + 0.5f) * cw, oy + (static_cast<float>(r) + 0.5f) * chh};
        };
        (void)rt.model().term->feed("\x1b[2J\x1b[H");   // a clean screen: only what the moves send
        rt.window().move_to(at(10, 5));
        rt.tick();
        rt.window().move_to(at(10, 5) + mayag::Vec2{1.0f, 1.0f});   // the same cell: not reported again
        rt.tick();
        rt.window().move_to(at(12, 6));
        rt.tick();
        drain_until(2000, "^[[<35;13;7M");
        const auto count = [&](std::string_view s) { std::size_t n = 0; for (auto p = got.find(s); p != std::string::npos; p = got.find(s, p + 1)) ++n; return n; };
        ok(count("^[[<35;11;6M") == 1 && count("^[[<35;13;7M") == 1,
           "hovering reports buttonless motion, once per cell (SGR 35)");
        (void)rt.model().term->feed("\x1b[2J\x1b[H");
        rt.window().press(at(3, 3));
        rt.tick();
        rt.window().move_to(at(5, 3));
        rt.tick();
        rt.window().release(at(5, 3));
        rt.tick();
        drain_until(2000, "^[[<0;6;4m");   // the release is the last of the three
        ok(got.find("^[[<0;4;4M") != std::string::npos && got.find("^[[<32;6;4M") != std::string::npos &&
           got.find("^[[<0;6;4m") != std::string::npos, "press, drag and release reach the program at the cells under the pointer");
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) {
            if (mm.pty) mm.pty->write("\x04");   // cat exits
            mm.pty.reset(); mm.mouse_cell_col = mm.mouse_cell_row = -1; }}});
        (void)rt.model().term->feed("\x1b[?1003l\x1b[?1006l\x1b[2J\x1b[H");
        rt.tick();
    }
    {
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
    {
        // ── ssh: ~/.ssh/config the way ssh reads it, hosts.vibe, the picker ──
        using K = mayag::Key;
        const std::string dir = (std::filesystem::temp_directory_path() / ("hand-ssh-" + std::to_string(hand::self_pid()))).string();
        std::filesystem::create_directories(dir + "/conf.d");
        const auto put = [](const std::string& p, const std::string& s) { std::FILE* f = std::fopen(p.c_str(), "w"); std::fputs(s.c_str(), f); std::fclose(f); };
        put(dir + "/config",
            "# test\nHost *.internal\n  User ops\nHost prod-db\n  HostName 10.0.3.4\n  User deploy\n  ProxyJump bastion\n"
            "Host bastion gpu-box !nope\n  HostName %h.example.com\nHost gpu-box\n  Port 2222\n  User root\n"
            "Include " + std::string{dir} + "/conf.d/*\nHost *\n  User fallback\n  IdentityFile ~/.ssh/id_ed25519\n");
        put(dir + "/conf.d/extra", "Host api.staging.internal\n");
        put(dir + "/known", "seen.example.org ssh-ed25519 AAAA\n[odd.example.org]:2200 ssh-rsa AAAA\n|1|hashed= ssh-rsa AAAA\n10.0.3.4 ssh-ed25519 AAAA\n");
        const hand::HostSources src{.ssh_config = dir + "/config", .known_hosts = dir + "/known", .store = dir + "/hosts.vibe"};
        auto book = hand::hosts_load(src);
        const auto* db = book.find("prod-db");
        const auto* gpu = book.find("gpu-box");
        const auto* api = book.find("api.staging.internal");
        ok(db && db->hostname == "10.0.3.4" && db->user == "deploy" && db->jump == "bastion" && db->env == hand::HostEnv::prod,
           "ssh_config: HostName / User / ProxyJump, and prod-db reads as prod");
        // first value wins: gpu-box's Port comes from its own block, its User from the first block that sets one
        ok(gpu && gpu->hostname == "gpu-box.example.com" && gpu->port == 2222 && gpu->user == "root" && !book.find("nope") && !book.find("*.internal"),
           "ssh_config: %h, first-wins across blocks, globs and !negations are not hosts");
        ok(api && api->user == "ops" && api->env == hand::HostEnv::staging, "ssh_config: Include, a glob block applies, staging from the name");
        ok(book.find("seen.example.org") && book.find("odd.example.org:2200") && book.find("odd.example.org:2200")->port == 2200 &&
           !book.find("10.0.3.4"), "known_hosts: new machines listed, [host]:port kept, hashed and known ones skipped");
        const auto d = hand::parse_destination("deploy@10.0.0.9:2201");
        const auto s = hand::ssh_destination({"ssh", "-t", "-p", "2022", "-l", "me", "-o", "Foo=bar", "box.lan", "uptime"});
        ok(d && d->user == "deploy" && d->hostname == "10.0.0.9" && d->port == 2201 && !hand::parse_destination("a b") &&
           s && s->hostname == "box.lan" && s->user == "me" && s->port == 2022, "destinations: typed user@host:port, and a running ssh's argv");
        const auto argv = hand::ssh_argv(*gpu, book, {});
        const bool muxed = std::find(argv.begin(), argv.end(), "ControlMaster=auto") != argv.end();
#if defined(_WIN32)
        ok(argv.back() == "gpu-box" && !muxed,
           "a config host runs as `ssh <alias>`; never multiplexed on Windows (its OpenSSH has no ControlMaster)");
#else
        ok(argv.back() == "gpu-box" && muxed, "a config host runs as `ssh <alias>`, multiplexed");
#endif
        hand::hosts_record_use(src, *gpu, 1000);
        hand::hosts_record_use(src, *gpu, 2000);
        hand::hosts_set_pinned(src, *db, true);
        hand::hosts_record_use(src, *d, 1500);
        book = hand::hosts_load(src);
        ok(book.find("gpu-box")->uses == 2 && book.find("gpu-box")->last == 2000 && book.find("prod-db")->pinned &&
           book.find("deploy@10.0.0.9:2201") && book.find("deploy@10.0.0.9:2201")->port == 2201,
           "hosts.vibe: uses, last, pinned, and a typed host remembered");
        std::vector<dsl::SshHost> sh;
        for (const auto& h : book.hosts) { dsl::SshHost x; x.alias = h.alias; x.hostname = h.hostname; x.user = h.user; x.pinned = h.pinned; x.uses = h.uses; x.last_used = h.last; sh.push_back(x); }
        const auto r0 = dsl::rank_hosts(sh, "", 2100), r1 = dsl::rank_hosts(sh, "gpu", 2100);
        ok(!r0.empty() && sh[static_cast<std::size_t>(r0[0])].alias == "prod-db" && sh[static_cast<std::size_t>(r0[1])].alias == "gpu-box" &&
           !r1.empty() && sh[static_cast<std::size_t>(r1[0])].alias == "gpu-box",
           "ranking: pinned, then frecent, before typing; a query finds its host");
        std::filesystem::remove_all(dir);

        // the picker, end to end (the demo has no pty: a connect makes a tab)
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[s = src](hand::App::Model& mm) {
            mm.ssh.book = hand::HostBook{};
            for (const char* a : {"prod-db", "gpu-box", "build-01"}) { hand::Host h; h.alias = a; h.hostname = a; mm.ssh.book.hosts.push_back(h); }
            mm.ssh.book.hosts[0].env = hand::HostEnv::prod;
        }}});
        rt.press_key(K::s, Mods{.shift = true, .ctrl = true});
        rt.tick();
        // OpenHosts reloads the book in a real run; in the demo it keeps the fixture
        ok(rt.model().ssh.picker.open && rt.node_rect(dsl::part_id("hosts", 0)).has_value() && rt.node_rect(dsl::part_id("hosts", 2)).has_value(),
           "ctrl+shift+s opens the host picker, every host listed");
        rt.type("gpu");
        rt.tick();
        ok(rt.node_rect(dsl::part_id("hosts", 0)).has_value() && !rt.node_rect(dsl::part_id("hosts", 1)).has_value(), "typing narrows it to one host");
        const std::size_t tabs0 = rt.model().tab_count();
        rt.press_key(K::enter);
        for (int i = 0; i < 10; ++i) rt.tick();
        ok(!rt.model().ssh.picker.open && rt.model().tab_count() == tabs0 + 1 && rt.model().remote && rt.model().remote->alias == "gpu-box",
           "enter connects in a new tab, beside this one");
        // ssh fails (255): the tab stays, with its scrollback, and offers to reconnect
        rt.send(hand::App::Msg{hand::App::Exited{255, rt.model().sid}});
        rt.tick();
        ok(rt.model().tab_count() == tabs0 + 1 && rt.model().link.dropped && rt.node_rect(dsl::node_id("ssh.reconnect")).has_value(),
           "a dropped connection keeps the tab and offers to reconnect");
        rt.press_key(K::enter);
        rt.tick();
        ok(!rt.model().link.dropped, "enter reconnects");
        rt.send(hand::App::Msg{hand::App::Exited{0, rt.model().sid}});
        for (int i = 0; i < 10; ++i) rt.tick();
        ok(rt.model().tab_count() == tabs0, "a clean logout closes the tab");
        // a typed destination nobody listed
        rt.press_key(K::s, Mods{.shift = true, .ctrl = true});
        rt.type("me@10.1.2.3:2200");
        rt.tick();
        ok(rt.node_rect(dsl::part_id("hosts", dsl::part_inc)).has_value(), "an unknown user@host:port offers to connect as typed");
        rt.press_key(K::escape);
        rt.tick();
        {
            // ── the host manager ─────────────────────────────────────────────
            rt.press_key(K::h, Mods{.shift = true, .ctrl = true});
            rt.tick();
            ok(rt.model().ssh.manager.open && rt.node_rect(dsl::node_id("mgr.connect")).has_value(),
               "ctrl+shift+h opens the host manager on a host, ready to connect");
            const std::size_t n0 = rt.model().ssh.book.hosts.size();
            // a new host: n, fields by typing and tab, a bad tunnel refused
            rt.press_key(K::n);
            rt.tick();
            ok(rt.model().ssh.manager.ui.editing && rt.model().ssh.manager.ui.edit_host < 0, "n starts a new host");
            rt.type("metrics"); rt.press_key(K::tab);
            rt.type("10.0.9.7"); rt.press_key(K::tab);
            rt.type("deploy"); rt.press_key(K::tab);
            rt.type("2222");
            for (int i = 0; i < 6; ++i) rt.press_key(K::tab);   // jump, key, folder, env, tags -> tunnels
            rt.type("L99999");
            rt.press_key(K::enter);
            rt.tick();
            ok(rt.model().ssh.manager.ui.editing && !rt.model().ssh.manager.ui.error.empty() && rt.model().ssh.book.hosts.size() == n0,
               "a bad tunnel (port 99999) is refused, with why");
            for (int i = 0; i < 6; ++i) rt.press_key(K::backspace);
            rt.type("L3000:grafana:3000");
            rt.press_key(K::enter);
            rt.tick();
            const auto& hb = rt.model().ssh.book.hosts;
            const auto* made = hb.size() == n0 + 1 ? &hb.back() : nullptr;
            ok(!rt.model().ssh.manager.ui.editing && made && made->alias == "metrics" && made->hostname == "10.0.9.7" && made->user == "deploy" &&
               made->port == 2222 && made->forwards.size() == 1 && made->forwards[0] == "L3000:grafana:3000" &&
               made->source == hand::HostSource::saved,
               "enter saves the new host, every field and its tunnel");
            if (made) {
                const auto args = hand::ssh_argv(*made, rt.model().ssh.book, {.multiplex = false, .keepalive = false});
                const std::string joined = [&] { std::string j; for (const auto& a : args) j += a + " "; return j; }();
                ok(joined.find("-L 3000:grafana:3000") != std::string::npos && joined.find("-p 2222") != std::string::npos &&
                   joined.find("deploy@10.0.9.7") != std::string::npos,
                   "connecting runs ssh -L 3000:grafana:3000 -p 2222 deploy@10.0.9.7");
            }
            // pin it, then delete it (asks once)
            rt.press_key(K::p);
            rt.tick();
            ok(!rt.model().ssh.book.hosts.empty() && rt.model().ssh.book.hosts.back().pinned, "p pins the selected host");
            rt.press_key(K::del);
            rt.tick();
            ok(rt.model().ssh.manager.ui.confirm_delete && rt.model().ssh.book.hosts.size() == n0 + 1, "delete asks first");
            rt.press_key(K::del);
            rt.tick();
            ok(rt.model().ssh.book.hosts.size() == n0, "and deletes on the second press");
            // filter, then enter on a host connects in a new tab
            rt.type("/gpu");
            rt.press_key(K::enter);
            const std::size_t t0 = rt.model().tab_count();
            rt.press_key(K::enter);
            for (int i = 0; i < 10; ++i) rt.tick();
            ok(!rt.model().ssh.manager.open && rt.model().tab_count() == t0 + 1 && rt.model().remote && rt.model().remote->alias == "gpu-box",
               "filter + enter connects to the host in a new tab");
            rt.send(hand::App::Msg{hand::App::Exited{0, rt.model().sid}});
            for (int i = 0; i < 10; ++i) rt.tick();
            // ── folders, import, files, tunnels ────────────────────────────────────
            rt.press_key(K::h, Mods{.shift = true, .ctrl = true});
            rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) {
                hand::Host h; h.alias = "ci-1"; h.hostname = "ci-1.internal"; h.source = hand::HostSource::saved; h.folder = "Work/CI";
                h.forwards = {"L5432:localhost:5432", "L8080:localhost:80"};
                mm.ssh.book.hosts.push_back(h);
                hand::ImportSource src{"PuTTY", "test", {}};
                hand::Host a; a.alias = "router"; a.hostname = "192.168.1.1"; a.source = hand::HostSource::saved; a.folder = "PuTTY";
                src.hosts = {a, h};   // ci-1 exists already: skipped
                mm.ssh.manager.imports = {src};
                mm.ssh.manager.ui.sel = static_cast<int>(mm.ssh.book.hosts.size()) - 1;
            }}});
            rt.tick();
            // a folder's defaults, through the folder editor
            rt.send(hand::App::Msg{hand::App::Manager{mayag::dsl::SshManagerOp{[](mayag::dsl::SshManagerState& s) {
                mayag::dsl::ssh_manager_ops::begin_folder_edit(s, {}, "Work");
            }}}});
            rt.type("x");   // name field focus is on user for an existing folder
            rt.press_key(K::backspace);
            rt.type("ops"); rt.press_key(K::tab); rt.type("2222"); rt.press_key(K::tab); rt.type("bastion");
            rt.press_key(K::enter);
            rt.tick();
            const hand::Folder* wf = rt.model().ssh.book.folder("Work");
            ok(wf && wf->user == "ops" && wf->port == 2222 && wf->jump == "bastion" && !rt.model().ssh.manager.ui.editing,
               "the folder editor saves what its hosts share");
            const auto& ci = rt.model().ssh.book.hosts.back();
            std::string av; for (const auto& x : hand::ssh_argv(ci, rt.model().ssh.book, {.multiplex = false, .keepalive = false})) av += x + " ";
            ok(av.find("-p 2222") != std::string::npos && av.find("-J bastion") != std::string::npos && av.find("ops@ci-1.internal") != std::string::npos,
               "a host in Work/CI connects with Work's user, port and jump");
            // tunnel status from a port scan
            rt.send(hand::App::Msg{hand::App::TunnelScan{{5432, 8080}, {"ssh", "postgres"}, true}});
            rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) { mm.remote = mm.ssh.book.hosts.back(); }}});
            rt.tick();
            {
                const auto hs = hand::detail::picker_hosts(rt.model());
                const auto& last = hs.back();
                using T = mayag::dsl::SshHost::Tunnel;
                ok(last.tunnel.size() == 2 && last.tunnel[0] == T::up && last.tunnel[1] == T::conflict &&
                       last.tunnel_note[1].find("postgres") != std::string::npos,
                   "tunnels: ssh's port is up, a port taken by another program says by whom");
            }
            rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) { mm.remote.reset(); }}});
            // import: new sessions only, into their folder
            const std::size_t nb = rt.model().ssh.book.hosts.size();
            rt.press_key(K::i);
            rt.tick();
            ok(rt.model().ssh.manager.ui.import_open && rt.node_rect(dsl::part_id("mgr.imp", 0)).has_value(), "i opens the import drawer, one row per client");
            rt.click(dsl::part_id("mgr.imp", 0));
            rt.tick();
            ok(rt.model().ssh.book.hosts.size() == nb + 1 && rt.model().ssh.book.hosts.back().alias == "router" && rt.model().ssh.book.hosts.back().folder == "PuTTY",
               "import adds only the sessions not here yet, in a folder named for the client");
            // the Files tab: f lists, a listing arrives, enter goes into a directory
            rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& mm) { mm.ssh.manager.ui.sel = static_cast<int>(mm.ssh.book.hosts.size()) - 2; }}});
            rt.press_key(K::f);
            rt.tick();
            ok(rt.model().ssh.manager.ui.tab == 1 && rt.model().ssh.manager.files.alias == "ci-1" && rt.model().ssh.manager.files.loading, "f opens the host's files and starts listing");
            rt.send(hand::App::Msg{hand::App::FilesListed{"ci-1", "/home/ci", {{"logs", "Nov 20 09:12", "", true, false, 0}, {"a.txt", "Nov 1 10:00", "", false, false, 12}}, ""}});
            rt.tick();
            ok(!rt.model().ssh.manager.files.loading && rt.model().ssh.manager.files.entries.size() == 2 && rt.node_rect(dsl::part_id("mgr.file", 1)).has_value(),
               "the listing shows, directories first");
            rt.press_key(K::enter);
            rt.tick();
            ok(rt.model().ssh.manager.files.loading, "enter on a directory lists it");
            rt.press_key(K::escape);
            rt.press_key(K::escape);
            rt.tick();
            ok(!rt.model().ssh.manager.open, "esc leaves the files, then the manager");
            {
                const auto l = hand::sftp::parse_listing(
                    "Remote working directory: /srv\n"
                    "-rw-r--r--    1 a a     220145 Nov 18 22:01 my report.pdf\n"
                    "drwxr-xr-x    2 a a       4096 Nov 17 09:12 logs\n"
                    "lrwxrwxrwx    1 a a         11 Nov 10 10:00 current -> releases/42\n");
                ok(l.path == "/srv" && l.entries.size() == 3 && l.entries[0].name == "logs" && l.entries[1].target == "releases/42" &&
                   l.entries[2].name == "my report.pdf" && l.entries[2].size == 220145,
                   "sftp listings: dirs first, names with spaces, links with targets");
                ok(hand::sftp::explain("x: Permission denied (publickey,password).", 255).find("key login") != std::string::npos,
                   "sftp: a password-only host is explained, not a raw error");
                const auto mh = hand::parse_mobaxterm_ini(
                    "[Bookmarks_1]\nSubRep=Work\\EU\nImgNum=41\ndb=#109#0%10.1.2.3%2200%root%%-1%-1#MobaFont#0# #-1\n"
                    "rdp=#91#4%r.example%3389%admin%0#MobaFont#0# #-1\n");
                ok(mh.size() == 1 && mh[0].alias == "db" && mh[0].port == 2200 && mh[0].user == "root" && mh[0].folder == "MobaXterm/Work/EU",
                   "MobaXterm.ini: SSH bookmarks with their folder; RDP skipped");
            }
            {   // the file pane's list: hidden, sort, dirs first, fuzzy filter, crumbs, ages
                namespace fp = mayag::dsl;
                std::vector<fp::FileEntry> es = {{.name = "src", .dir = true, .mtime = 100},
                                                 {.name = ".git", .dir = true},
                                                 {.name = "CMakeLists.txt", .size = 3000, .mtime = 300},
                                                 {.name = "README.md", .size = 9000, .mtime = 200},
                                                 {.name = "build.sh", .size = 10, .mtime = 400}};
                fp::FilePaneState st;
                auto v = fp::file_pane_view(es, st);
                ok(v.size() == 4 && es[v[0].i].name == "src" && es[v[1].i].name == "build.sh",
                   "file pane: dotfiles hidden, folders first, names case-insensitive");
                st.sort = fp::FileSort::size;
                v = fp::file_pane_view(es, st);
                const bool by_size = es[v[1].i].name == "README.md";
                st.sort = fp::FileSort::modified;
                v = fp::file_pane_view(es, st);
                ok(by_size && es[v[1].i].name == "build.sh" && es[v[3].i].name == "README.md", "file pane: sorts by size and by age");
                st.filter = "cmk";
                v = fp::file_pane_view(es, st);
                ok(v.size() == 1 && es[v[0].i].name == "CMakeLists.txt" && v[0].hits.size() == 3, "file pane: fuzzy filter lights its matches");
                st.filter = ".gi";
                ok(fp::file_pane_view(es, st).size() == 1, "file pane: a filter starting with . finds dotfiles");
                const auto cr = fp::file_pane_detail::crumbs("C:/Users/me/src/hand", "C:/Users/me", '/');
                ok(cr.size() == 3 && cr[0].first == "~" && cr[2].second == "C:/Users/me/src/hand", "file pane: crumbs fold home into ~");
                ok(fp::file_pane_detail::age(10000, 10000 - 7200) == "2h" && fp::file_pane_detail::age(10000, 0).empty(),
                   "file pane: ages read 2h, unknown is blank");
                fp::FilePaneState fs2;
                auto k1 = fp::file_pane_key(fs2, fp::FilePaneProps{.entries = es}, mayag::Key::slash, {});
                if (k1) (*k1)(fs2);
                if (auto t1 = fp::file_pane_text(fs2, "/")) (*t1)(fs2);   // the / that began it
                if (auto t2 = fp::file_pane_text(fs2, "rea")) (*t2)(fs2);
                ok(fs2.filtering && fs2.filter == "rea", "file pane: / starts a filter, typing narrows it");
            }
        }
        ok(!rt.model().ssh.picker.open, "esc closes the picker");
    }
    {
        // ── hand.vibe: every row survives save + load ──────────────────────────
        using SE = dsl::SettingEdit;
        hand::Config a;
        a.url_launcher = "firefox";
        const auto n = hand::setting_specs(a).size();
        for (std::size_t i = 0; i < n; ++i) (void)hand::setting_apply(a, {SE::Op::step, i, 0.0f, 2});
        const std::string path = (std::filesystem::temp_directory_path() / ("hand-test-" + std::to_string(hand::self_pid()) + ".vibe")).string();
        const bool saved = hand::config_save(a, path);
        hand::Config b;
        const std::string problems = hand::config_load(b, path);
        ::unlink(path.c_str());
        const auto sa = hand::setting_specs(a), sb = hand::setting_specs(b);
        std::string diff;
        for (std::size_t i = 0; i < n; ++i)
            if (std::fabs(sa[i].value - sb[i].value) > 1e-3f) diff += " " + sa[i].section + "." + sa[i].key;
        if (!problems.empty()) std::printf("    load problems: %s", problems.c_str());
        if (!diff.empty()) std::printf("    differs after reload:%s\n", diff.c_str());
        ok(saved && problems.empty() && diff.empty() && b.url_launcher == "firefox",
           "hand.vibe round-trips every setting");
        // a broken or partial file is reported, never fatal
        const std::string bad = path + ".bad";
        { std::FILE* f = std::fopen(bad.c_str(), "w"); std::fputs("font { size 15.5  nope 3 }\ncolors { scheme \"Nord\" bold_is_bright yes }\n", f); std::fclose(f); }
        hand::Config c;
        const std::string p2 = hand::config_load(c, bad);
        ::unlink(bad.c_str());
        ok(c.font_size == 15.5f && hand::palette_of(c).bg == 0x2E3440 && p2.find("font.nope") != std::string::npos &&
           p2.find("bold_is_bright") != std::string::npos,
           "good keys apply; unknown keys and wrong types are reported");
        { std::FILE* f = std::fopen(bad.c_str(), "w"); std::fputs("font { size ", f); std::fclose(f); }
        hand::Config e;
        const std::string p3 = hand::config_load(e, bad);
        ::unlink(bad.c_str());
        ok(!p3.empty() && e.font_size == hand::Config{}.font_size, "a syntax error keeps the defaults");
    }
    std::printf(fails ? "FAIL\n" : "PASS\n");
    return fails;
}

}  // namespace hand::cli
