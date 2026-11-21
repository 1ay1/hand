// hand::cli::checks — the mouse in a TUI: hover (1003) and drags (1002).

#include "suites.hpp"

#include <hand/core/platform.hpp>

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

void tui_mouse([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── the mouse in a TUI: hover (1003) and drags (1002) reach it ──
    // A real program on a real pty: `hand --cat-v` goes raw and shows
    // every byte it is sent, so the test reads what the app received.
    // Using our own binary keeps this identical on every platform.
    const auto pty = hand::PtyRef::make();
    const bool up = pty->spawn(rt.model().tab().term->cols(), rt.model().tab().term->rows(),
                               hand::self_exe(), {}, {"--cat-v"});
    std::string got;
    // the app's own pty reader feeds the echo into the terminal:
    // read it back from the screen
    const auto scrape = [&] {
        got.clear();
        const auto g = rt.model().tab().term->snapshot();
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
        mm.tab().selecting = false; mm.tab().mouse_button = -1; mm.tab().pty = pty; }}});
    for (int i = 0; i < 30; ++i) rt.tick();   // any closing overlay is gone
    drain_until(2000, "ready");
    ok(up && rt.model().tab().term->mouse_reporting(), "a TUI turns on any-motion mouse tracking");
    const float top = rt.model().strip_h(), cw = rt.model().metrics.cell_w, chh = rt.model().metrics.cell_h;
    const float ox = rt.model().metrics.origin_x, oy = rt.model().metrics.origin_y + top;
    const auto at = [&](int c, int r) {
        return mayag::Vec2{ox + (static_cast<float>(c) + 0.5f) * cw, oy + (static_cast<float>(r) + 0.5f) * chh};
    };
    (void)rt.model().tab().term->feed("\x1b[2J\x1b[H");   // a clean screen: only what the moves send
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
    (void)rt.model().tab().term->feed("\x1b[2J\x1b[H");
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
        if (mm.tab().pty) mm.tab().pty->write("\x04");   // cat exits
        mm.tab().pty.reset(); mm.tab().mouse_cell_col = mm.tab().mouse_cell_row = -1; }}});
    (void)rt.model().tab().term->feed("\x1b[?1003l\x1b[?1006l\x1b[2J\x1b[H");
    rt.tick();
}

}  // namespace hand::cli::checks
