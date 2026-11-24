// hand::cli::checks — shell marks, links, search, hints, the minimap, cursor motion.

#include "suites.hpp"
#include "../../app/detail.hpp"   // frames_wanted_by

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

void terminal_features([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── the cool stuff ───────────────────────────────────────────────────
    using K = mayag::Key;
    using E = mayag::Edit<hand::App::Model>;
    const auto feed = [&](std::string s) { rt.send(hand::App::Msg{hand::App::Output{std::move(s)}}); rt.tick(); };
    feed("\x1b[2J\x1b[H");
    // two commands through OSC 133, as the injected script emits them
    feed("\x1b]133;A\a\x1b[32m\u276F\x1b[0m \x1b]133;B\amake\r\n\x1b]133;C\abuilding...\r\nerror: oops\r\n\x1b]133;D;2\a");
    feed("\x1b]133;A\a\u276F \x1b]133;B\agit log -1\r\n\x1b]133;C\acommit 3f9a2c1d see https://toe.dev/x and ~/src/hand/main.cpp\r\n\x1b]133;D;0\a");
    feed("\x1b]133;A\a\u276F \x1b]133;B\a");
    const auto cmds = rt.model().tab().term->commands();
    int ran = 0, failed = 0;
    for (const auto& c : cmds) { ran += c.ran; failed += c.finished && c.exit_code != 0; }
    ok(ran == 2 && failed == 1, "OSC 133 commands are recorded, with exit codes");
    ok(rt.model().tab().term->command_text(cmds[0]) == "make", "the typed command line is recovered");
    const auto sg = rt.model().tab().term->snapshot();
    ok(sg.lines[0].mark == dsl::TermMark::error && sg.lines[3].mark == dsl::TermMark::ok,
       "failed and succeeded prompts get their marks");
    {   // a toast is still: it costs no frames, and its own timer takes it down
        const bool driving = rt.window().driving_frames();
        rt.window().drive_frames(true);
        for (int i = 0; i < 600 && rt.host().wants_frames(); ++i) rt.tick();   // everything else at rest
        rt.window().drive_frames(driving);
        rt.send(hand::App::Msg{hand::App::CopyOutput{}});
        ok(rt.model().toast.card.has_value(), "a toast appears");
        ok(hand::frames_wanted_by(rt.model()).empty() && !rt.host().wants_frames(), "...and asks for no frames while it is up");
        const auto first = rt.model().toast.serial;
        rt.send(hand::App::Msg{hand::App::ToastExpired{first - 1}});
        ok(rt.model().toast.card.has_value(), "an older toast's timer does not take a newer one down");
        rt.send(hand::App::Msg{hand::App::ToastExpired{first}});
        ok(!rt.model().toast.card.has_value(), "its own timer does");
    }
    rt.press_key(K::g, Mods{.shift = true, .ctrl = true});
    ok(rt.window().get_clipboard().find("commit 3f9a2c1d") != std::string::npos, "ctrl+shift+g copies the last output");
    // hints
    rt.press_key(K::space, Mods{.shift = true, .ctrl = true});
    bool url = false, path = false, hash = false;
    for (const auto& h : rt.model().tab().hint->hints) {
        url = url || h.text == "https://toe.dev/x";
        path = path || h.text == "~/src/hand/main.cpp";
        hash = hash || h.text == "3f9a2c1d";
    }
    ok(rt.model().tab().hint.has_value() && url && path && hash, "ctrl+shift+space labels the URL, the path and the hash");
    std::size_t hi = 0;
    while (hi < rt.model().tab().hint->hints.size() && rt.model().tab().hint->hints[hi].text != "3f9a2c1d") ++hi;
    const std::string label = rt.model().tab().hint && hi < rt.model().tab().hint->labels.size() ? rt.model().tab().hint->labels[hi] : "?";
    rt.type(label);
    ok(!rt.model().tab().hint.has_value() && rt.window().get_clipboard() == "3f9a2c1d", "typing its label copies it");
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
    ok(rt.model().tab().out_shift > 0.0f, "Enter at the live prompt glides the screen up a line");
    for (int i = 0; i < 40; ++i) rt.tick();
    ok(rt.model().tab().out_shift == 0.0f, "...and settles");
    rt.window().drive_frames(false);
    {
        // ── motion: every effect fires on a typed move, then clears; spring overshoots
        using E2 = mayag::Edit<hand::App::Model>;
        rt.window().drive_frames(true);
        const char* names[] = {"ripple", "sonicboom", "wireframe", "pixiedust", "railgun", "torpedo", "fireworks"};
        bool all = true;
        for (int fx = 1; fx <= 7; ++fx) {
            rt.send(hand::App::Msg{E2{[fx](hand::App::Model& m) { m.cfg.cursor_fx = static_cast<hand::CursorFx>(fx); m.tab().fx.ps.clear(); }}});
            rt.type("x");                                          // the keystroke...
            if (fx == 7) rt.press_key(K::enter);
            feed(fx == 7 ? "x\r\n" : "x");                       // ...and its echo moves the cursor
            rt.tick();
            const bool fired = rt.model().tab().fx.busy();
            for (int i = 0; i < 90; ++i) rt.tick();
            const bool cleared = !rt.model().tab().fx.busy();
            if (!fired || !cleared) { std::printf("    effect %s: fired=%d cleared=%d\n", names[fx - 1], fired, cleared); all = false; }
        }
        ok(all, "all seven cursor effects fire on a typed move and clear away");
        rt.send(hand::App::Msg{E2{[](hand::App::Model& m) {
            m.cfg.cursor_fx = hand::CursorFx::none; m.cfg.cursor_anim = hand::CursorAnim::spring; }}});
        for (int i = 0; i < 30; ++i) rt.tick();
        const int c0 = rt.model().tab().term->cursor().col;
        feed("\x1b[" + std::to_string(c0 + 11) + "G");      // jump the cursor 10 cells right
        float peak = 0.0f;
        for (int i = 0; i < 60; ++i) { rt.tick(); peak = std::max(peak, rt.model().tab().cur_x); }
        const float goal = static_cast<float>(rt.model().tab().term->cursor().col);
        ok(peak > goal + 0.3f && std::fabs(rt.model().tab().cur_x - goal) < 0.02f, "the spring cursor overshoots, then settles");
        {   // motion.trail: a longer tail lags further behind the head
            const auto lag = [&](float len) {
                rt.send(hand::App::Msg{E2{[len](hand::App::Model& m) {
                    m.cfg.cursor_anim = hand::CursorAnim::glide; m.cfg.cursor_trail = len; }}});
                for (int i = 0; i < 60; ++i) rt.tick();
                const int c1 = rt.model().tab().term->cursor().col;
                feed("\x1b[" + std::to_string(c1 > 20 ? 1 : c1 + 21) + "G");
                for (int i = 0; i < 6; ++i) rt.tick();
                return std::fabs(rt.model().tab().fx.tx - rt.model().tab().cur_x);
            };
            const float short_ = lag(0.3f), long_ = lag(4.0f);
            ok(long_ > short_ * 1.5f, "motion.trail sets how far the tail lags (longer = a bigger comet)");
            for (int i = 0; i < 90; ++i) rt.tick();
            ok(motion_settled(rt.model()), "...and it always catches up");
        }
        rt.send(hand::App::Msg{E2{[](hand::App::Model& m) { m.cfg.cursor_anim = hand::CursorAnim::glide; m.cfg.bell_shake = true; }}});
        feed("\a");
        ok(rt.model().tab().bell_at >= 0.0, "BEL with shake on is seen");
        rt.send(hand::App::Msg{E2{[](hand::App::Model& m) { m.cfg.bell_shake = false; }}});
        rt.window().drive_frames(false);
    }
    // smooth cursor
    rt.send(hand::App::Msg{E{[](hand::App::Model& m) { m.cfg.cursor_anim = hand::CursorAnim::glide; m.tab().cur_known = true; m.tab().cur_x = 0; m.tab().cur_y = 0; }}});
    rt.window().drive_frames(true);   // the frame clock runs, as on screen
    feed("\x1b[10;30H");
    const float mid = rt.model().tab().cur_x;
    for (int i = 0; i < 40; ++i) rt.tick();
    rt.window().drive_frames(false);
    ok(mid < 29.0f && std::fabs(rt.model().tab().cur_x - 29.0f) < 0.05f && std::fabs(rt.model().tab().cur_y - 9.0f) < 0.05f,
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
    const float smid = rt.model().tab().scroll_d;
    settle();
    ok(rt.model().tab().term->scroll_offset() > 0 && rt.model().tab().scroll_d == rt.model().tab().scroll_goal,
       "ctrl+shift+z glides back to earlier prompts");
    ok(smid > 0.0f && smid < rt.model().tab().scroll_goal, "...through every pixel in between");
    ok(rt.node_rect(dsl::node_id("tolive")).has_value(), "scrolled back, a pill offers the way home");
    ok(!rt.node_rect(dsl::node_id("sticky")).has_value(), "on a prompt, no header (the prompt is right there)");
    rt.press_key(K::down, Mods{.shift = true, .ctrl = true});   // one line into its output
    settle();
    ok(rt.model().tab().sticky_t == 1.0f && rt.node_rect(dsl::node_id("sticky")).has_value(),
       "the command you are inside slides in as a sticky header");
    {
        const float before = rt.model().tab().scroll_goal;
        rt.click(dsl::node_id("sticky"));
        settle();
        ok(rt.model().tab().scroll_goal != before, "clicking the header flies to that command's prompt");
    }
    {
        const auto g2 = rt.model().tab().grid;   // mid-glide frames carry the row above
        rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
        rt.tick();
        ok(rt.model().tab().scroll_d != std::floor(rt.model().tab().scroll_d) && rt.model().tab().grid->rows == rt.model().tab().term->rows() + 1,
           "mid-glide the grid is a row taller and slides by sub-row amounts");
        (void)g2;
        settle();
    }
    rt.send(hand::App::Msg{hand::App::Output{std::string{"\r\nnew 1\r\nnew 2\r\nnew 3"}}});
    rt.tick();
    ok(rt.model().tab().new_below >= 3, "output arriving while you read is counted, not shoved at you");
    // the copy toast above sits over the pill's corner; its timer (3 s of real
    // time, longer than this suite's ticks) is what takes it down
    rt.send(hand::App::Msg{hand::App::ToastExpired{rt.model().toast.serial}});
    rt.click(dsl::node_id("tolive"));
    settle();
    ok(rt.model().tab().term->scroll_offset() == 0 && rt.model().tab().new_below == 0, "the pill glides you back to live");
    for (int i = 0; i < 3; ++i) rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
    settle();
    for (int i = 0; i < 5; ++i) rt.press_key(K::x, Mods{.shift = true, .ctrl = true});
    settle();
    ok(rt.model().tab().term->scroll_offset() == 0, "ctrl+shift+x walks forward to the live prompt");
    // the live tail: scrolled back, the last live rows slide in at the bottom
    rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
    rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
    rt.press_key(K::z, Mods{.shift = true, .ctrl = true});
    settle();
    ok(rt.model().tab().tail_t == 1.0f && rt.node_rect(dsl::node_id("tail")).has_value(),
       "scrolled back, the live tail slides in at the bottom");
    rt.click(dsl::node_id("tail"));
    settle();
    ok(rt.model().tab().term->scroll_offset() == 0 && rt.model().tab().tail_t == 0.0f, "clicking it glides home and it slides away");
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
        const int off = rt.model().tab().term->scroll_offset();
        ok(off >= 5, "the wheel scrolls back while output keeps streaming");
        const auto top = [&] {
            std::string s;
            const auto g = rt.model().tab().term->snapshot();
            for (const auto& c : g.lines[0].cells) s += c.text;
            return s;
        };
        const std::string a = top();
        stream(20);
        ok(top() == a, "and what you are reading stays still as more arrives");
        ok(rt.model().tab().new_below > 0, "...counted as new below");
        // drag-select past the top edge: the view scrolls with the selection
        const int off2 = rt.model().tab().term->scroll_offset();
        rt.window().press(Vec2{60.0f, size.y * 0.5f});
        rt.tick();
        rt.window().move_to(Vec2{80.0f, 2.0f});
        for (int k = 0; k < 40; ++k) rt.tick();
        rt.window().release(Vec2{80.0f, 2.0f});
        rt.tick();
        ok(rt.model().tab().term->scroll_offset() > off2 && rt.model().tab().term->has_selection(),
           "dragging a selection past the top edge scrolls history with it");
        rt.press_key(K::up, Mods{.shift = true, .ctrl = true});
        settle();
        rt.press_key(K::end, Mods{.shift = true});
        settle();
        ok(rt.model().tab().term->scroll_offset() == 0, "shift+end lands back on the live view");
    }
    {
        // a full-screen app (alternate screen): no prompt marks, no minimap
        rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[?1049h\x1b[2J\x1b[H"}}});
        for (int r = 0; r < 20; ++r)
            rt.send(hand::App::Msg{hand::App::Output{"\x1b[" + std::to_string(r + 1) + ";1Hmatrix " + std::to_string(r)}});
        rt.tick();
        int marks = 0;
        for (const auto& l : rt.model().tab().grid->lines) marks += l.mark != dsl::TermMark::none ? 1 : 0;
        ok(marks == 0, "a full-screen app (cmatrix, vim) gets no prompt marks");
        ok(!rt.node_rect(dsl::node_id("minimap")).has_value(), "...and no minimap");
        rt.send(hand::App::Msg{hand::App::Output{std::string{"\x1b[?1049l"}}});
        rt.tick();
        ok(rt.node_rect(dsl::node_id("minimap")).has_value(), "back at the shell, both return");
    }
    rt.window().drive_frames(false);
}

}  // namespace hand::cli::checks
