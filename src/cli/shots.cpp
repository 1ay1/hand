// hand::cli — screenshots and benchmarks (`hand --png out.png --shot NAME`,
// `hand --bench`, `hand --flood CMD`): stage the model for one named scene,
// render a frame, write the PNG.

#include "cli.hpp"

#include "../app/detail.hpp"

#include <hand/app.hpp>
#include <hand/core/shell.hpp>
#include <hand/core/pty.hpp>


#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <chrono>

#include <mayag/widget/ssh.hpp>
#include <mayag/widget/file_pane.hpp>
#include <hand/config/schemes_gen.hpp>
#include <hand/ssh/sftp.hpp>


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

int shoot(Runtime& rt, Vec2 size, const std::string& shot, const std::string& out) {
rt.window().resize(size, 2.0f);
if (shot == "bench") {
    // a realistic frame: a screen of colour output, history, the minimap, two tabs
    std::string s;
    for (int i = 0; i < 400; ++i)
        s += "\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\amake -j8\r\n\x1b]133;C\a"
             "\x1b[32m[" + std::to_string(i % 100) + "%]\x1b[0m Building CXX object \x1b[1mCMakeFiles/hand.dir/src/app/"
             + std::to_string(i) + ".cpp.o\x1b[0m \x1b[2m(" + std::to_string(i * 7 % 997) + " ms)\x1b[0m\r\n\x1b]133;D;"
             + std::to_string(i % 13 == 0 ? 2 : 0) + "\a";
    rt.send(hand::App::Msg{hand::App::Output{s}});
    rt.press_key(mayag::Key::t, mayag::Mods{.shift = true, .ctrl = true});
    rt.send(hand::App::Msg{hand::App::Output{s}});
    for (int i = 0; i < 20; ++i) rt.tick();
    using clk = std::chrono::steady_clock;
    const auto run = [&](const char* what, auto&& poke) {
        if (const char* only = std::getenv("HAND_BENCH_ONLY"); only && !std::string_view{what}.starts_with(only)) return;
        static const int N = std::getenv("HAND_BENCH_LOOP") ? std::atoi(std::getenv("HAND_BENCH_LOOP")) : 120;
        double view = 0, cascade = 0, layout = 0, paint = 0, render = 0, fold = 0;
        const auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            poke(i);   // the input (posted: one input, one frame)
            const auto a = clk::now();
            rt.tick();
            fold += std::chrono::duration<double, std::milli>(clk::now() - a).count();
            const auto& f = rt.host().latency().last();
            view += f.view_ms; cascade += f.cascade_ms; layout += f.layout_ms; paint += f.paint_ms; render += f.render_ms;
        }
        const double total = std::chrono::duration<double, std::milli>(clk::now() - t0).count() / N;
        std::printf("  %-26s %6.3f ms/frame   frame %.3f  view %.3f  cascade %.3f  layout %.3f  paint %.3f  raster %.3f\n",
                    what, total, fold / N, view / N, cascade / N, layout / N, paint / N, render / N);
    };
    std::printf("hand frame pipeline (%dx%d, %d cols x %d rows)\n", static_cast<int>(size.x), static_cast<int>(size.y),
                rt.model().tab().term->cols(), rt.model().tab().term->rows());
    run("typing (a char/frame)", [&](int i) { rt.post(hand::App::Msg{hand::App::Output{std::string(1, static_cast<char>('a' + i % 26))}}); });
    run("streaming (a line/frame)", [&](int i) { rt.post(hand::App::Msg{hand::App::Output{"line " + std::to_string(i) + " \x1b[33mwarning\x1b[0m: something\r\n"}}); });
    run("scrolling history", [&](int i) { rt.window().scroll(Vec2{400.0f, 300.0f}, Vec2{0.0f, (i / 30) % 2 ? -48.0f : 48.0f}); });
    run("pointer over tabs", [&](int i) { rt.window().move_to(Vec2{40.0f + static_cast<float>(i % 60) * 6.0f, 14.0f}); });
    {   // the pane edge with a short history: the grid re-wraps live every frame
        rt.send(hand::App::Msg{hand::App::TogglePane{}});
        rt.send(hand::App::Msg{hand::App::PaneResize{true, 0.0f}});
        run("dragging pane, live re-wrap", [&](int i) {
            rt.post(hand::App::Msg{hand::App::PaneResize{false, static_cast<float>((i % 60 < 30 ? i % 60 : 60 - i % 60) * 3)}});
        });
        rt.send(hand::App::Msg{hand::App::PaneResize{false, 0.0f, true}});
        rt.send(hand::App::Msg{hand::App::TogglePane{}});
    }
    {   // the file pane's edge, dragged 3 px a frame over a FULL scrollback (10k lines of 120 chars)
        std::string hist;
        for (int i = 0; i < 10000; ++i)
            hist += "\x1b[32m" + std::to_string(i) + "\x1b[0m " + std::string(110, static_cast<char>('a' + i % 26)) + "\r\n";
        rt.send(hand::App::Msg{hand::App::Output{hist}});
        {
            hand::Term t{170, 42, hand::palette_of(hand::Config{})};
            (void)t.feed(hist);
            const auto a = clk::now();
            for (int i = 0; i < 40; ++i) t.resize(170 - (i % 2), 42);
            std::printf("  toe resize, 10k-line scrollback: %.3f ms per column change\n",
                        std::chrono::duration<double, std::milli>(clk::now() - a).count() / 40.0);
        }
        rt.send(hand::App::Msg{hand::App::TogglePane{}});
        rt.send(hand::App::Msg{hand::App::PaneResize{true, 0.0f}});
        run("pane open, pointer moving", [&](int i) { rt.window().move_to(Vec2{900.0f + static_cast<float>(i % 60) * 6.0f, 300.0f}); });
        run("dragging the pane edge", [&](int i) {
            rt.post(hand::App::Msg{hand::App::PaneResize{false, static_cast<float>((i % 60 < 30 ? i % 60 : 60 - i % 60) * 3)}});
        });
        if (std::getenv("HAND_BENCH_DIAG")) {
            rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.cfg.minimap = false; }}});
            run("  ...minimap off", [&](int i) {
                rt.post(hand::App::Msg{hand::App::PaneResize{false, static_cast<float>((i % 60 < 30 ? i % 60 : 60 - i % 60) * 3)}});
            });
            rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.cfg.minimap = true; m.cfg.files_left = false; }}});
            run("  ...pane on the right", [&](int i) {
                rt.post(hand::App::Msg{hand::App::PaneResize{false, static_cast<float>((i % 60 < 30 ? i % 60 : 60 - i % 60) * 3)}});
            });
        }
        rt.send(hand::App::Msg{hand::App::TogglePane{}});
    }
    std::printf("  damage: %.1f%% of tiles reused, %llu mismatches\n",
                100.0 * static_cast<double>(rt.window().tiles_reused()) / static_cast<double>(std::max<std::uint64_t>(rt.window().tiles_total(), 1)),
                static_cast<unsigned long long>(rt.window().damage_mismatches()));
    {   // `yes`: the pty reader hands over 1 MB of "y\n" per wakeup, as fast as it can
        std::string y;
        y.reserve(1u << 20);
        while (y.size() + 2 <= (1u << 20)) y += "y\r\n";
        double worst = 0, feed = 0;
        constexpr int N = 40;
        const auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            const auto a = clk::now();
            rt.send(hand::App::Msg{hand::App::Output{y}});
            const auto b = clk::now();
            rt.tick();
            const double ms = std::chrono::duration<double, std::milli>(clk::now() - a).count();
            feed += std::chrono::duration<double, std::milli>(b - a).count();
            worst = std::max(worst, ms);
        }
        const double total = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
        std::printf("  %-26s %6.3f ms/MB   (feed %.3f, worst frame %.2f ms, %.0f MB/s, history %d)\n", "yes (1 MB/frame)",
                    total / N, feed / N, worst, N * 1000.0 / total, rt.model().tab().term->history());
    }
    return 0;
}
if (shot == "flood") {   // a real shell running a firehose: frames keep coming, memory stays flat, ctrl-C lands
    using clk = std::chrono::steady_clock;
    const auto rss_mb = [] { return static_cast<double>(hand::plat::peak_rss()) / (1024.0 * 1024.0); };
    const auto t0 = clk::now();
    double worst = 0; int frames = 0; std::uint64_t b0 = rt.model().tab().bytes;
    const double dur = std::getenv("HAND_FLOOD_S") ? std::atof(std::getenv("HAND_FLOOD_S")) : 4.0;
    while (clk::now() - t0 < std::chrono::duration<double>(dur)) {
        const auto a = clk::now();
        rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(16));   // one vsync of real time
        rt.tick();
        worst = std::max(worst, std::chrono::duration<double, std::milli>(clk::now() - a).count());
        ++frames;
    }
    const double secs = std::chrono::duration<double>(clk::now() - t0).count();
    const double mbps = static_cast<double>(rt.model().tab().bytes - b0) / (1024.0 * 1024.0) / secs;
    rt.press_key(mayag::Key::c, mayag::Mods{.ctrl = true});
    const auto c0 = clk::now();
    std::uint64_t last = rt.model().tab().bytes; auto quiet_since = clk::now();
    while (clk::now() - c0 < std::chrono::seconds(5) && clk::now() - quiet_since < std::chrono::milliseconds(300)) {
        rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(16));   // one vsync of real time
        rt.tick();
        if (rt.model().tab().bytes != last) { last = rt.model().tab().bytes; quiet_since = clk::now(); }
    }
    const double stop_ms = std::chrono::duration<double, std::milli>(quiet_since - c0).count();
    if (rt.model().tab().pty) {
        const int pid = static_cast<int>(rt.model().tab().pty->pid());
        std::printf("flood: child %d %s, fg pgrp %d, history %d, offset %d\n", pid, hand::pty_reap(pid) < 0 ? "ALIVE" : "gone",
                    static_cast<int>(hand::pty_foreground(rt.model().tab().pty->fd())), rt.model().tab().term->history(), rt.model().tab().term->scroll_offset());
    }
    std::printf("flood: %d frames in %.1fs (%.0f fps), worst frame %.1f ms, %.0f MB/s through toe, peak rss %.0f MB, ctrl-C quiet after %.0f ms\n",
                frames, secs, frames / secs, worst, mbps, rss_mb(), stop_ms);
    return (worst < 100.0 && rss_mb() < 400.0 && stop_ms < 1000.0) ? 0 : 1;
}
if (shot == "echo") {   // keystroke -> pty -> echo -> parsed -> frame drawn: hand's whole share of input-to-photon
    using clk = std::chrono::steady_clock;
    // wait for the program to come up and go quiet
    rt.settle([](const auto& m) { return static_cast<bool>(m.tab().pty); }, std::chrono::seconds(3));
    for (int i = 0; i < 20; ++i) { rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(5)); rt.tick(); }
    const int n = std::getenv("HAND_ECHO_N") ? std::atoi(std::getenv("HAND_ECHO_N")) : 200;
    std::vector<double> lat;
    lat.reserve(static_cast<std::size_t>(n));
    int lost = 0;
    double trip = 0, frame = 0, keyframe = 0;
    for (int i = 0; i < n; ++i) {
        const std::uint64_t before = rt.model().tab().bytes;
        const auto a = clk::now();
        rt.type(std::string(1, static_cast<char>('a' + i % 26)));   // the key, as the window delivers it
        keyframe += std::chrono::duration<double, std::milli>(clk::now() - a).count();
        if (std::getenv("HAND_ECHO_DIAG") && i < 4) { const auto& f = rt.host().latency().last(); std::printf("  key frame: view %.3f cascade %.3f layout %.3f paint %.3f raster %.3f\n", f.view_ms, f.cascade_ms, f.layout_ms, f.paint_ms, f.render_ms); }
        const bool back = rt.settle([before](const auto& m) { return m.tab().bytes > before; }, std::chrono::milliseconds(500));
        const auto b = clk::now();
        rt.tick();                                                   // the frame that shows it
        if (!back) { ++lost; continue; }
        trip += std::chrono::duration<double, std::milli>(b - a).count();
        frame += std::chrono::duration<double, std::milli>(clk::now() - b).count();
        lat.push_back(std::chrono::duration<double, std::milli>(clk::now() - a).count());
        if (i % 32 == 31) { rt.press_key(mayag::Key::enter); rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(5)); rt.tick(); }
    }
    std::sort(lat.begin(), lat.end());
    const auto pct = [&](double p) { return lat.empty() ? 0.0 : lat[std::min(lat.size() - 1, static_cast<std::size_t>(p * static_cast<double>(lat.size())))]; };
    std::printf("echo: %zu keys  p50 %.2f ms  p95 %.2f ms  max %.2f ms  min %.2f ms  lost %d  (key fold+frame %.2f, ->echo parsed %.2f, echo frame %.2f)\n", lat.size(), pct(0.5), pct(0.95),
                lat.empty() ? 0.0 : lat.back(), lat.empty() ? 0.0 : lat.front(), lost, keyframe / std::max<double>(1, lat.size()), trip / std::max<double>(1, lat.size()), frame / std::max<double>(1, lat.size()));
    // the gate: an order of magnitude over the baseline, so a regression trips it and noise doesn't
    const double p50_max = std::getenv("HAND_ECHO_P50") ? std::atof(std::getenv("HAND_ECHO_P50")) : 8.0;
    return (lost == 0 && pct(0.5) < p50_max) ? 0 : 1;
}
if (shot == "rows") {   // where glyphs sit in their cells: inverse video makes each cell's box visible
    rt.send(hand::App::Msg{hand::App::Output{"\x1b[2J\x1b[H\x1b[7mHxgM\x1b[0m \x1b[7m|_^Ag\x1b[0m\r\n\x1b[7mHHHH\x1b[0m\r\n"}});
    for (int i = 0; i < 4; ++i) rt.tick();
    const auto& mt = rt.model().metrics;
    std::printf("rows: cell %.3f x %.3f  origin %.3f,%.3f  top %.3f  left %.3f  font %.2f  line_height %.2f (scale 2)\n",
                mt.cell_w, mt.cell_h, mt.origin_x, mt.origin_y, hand::detail::term_top(rt.model()),
                hand::detail::term_left(rt.model()), rt.model().font_size, rt.model().cfg.line_height);
}
if (shot == "corner") {   // tabs.always off: the window's buttons alone in the corner
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
        m.cfg.tabs_always = false; m.strip.t = 0.0f; hand::App::fit(m); }}});
    for (int i = 0; i < 4; ++i) rt.tick();
    if (std::getenv("HAND_TABHOVER")) {
        rt.window().move_to(rt.host().center_of(dsl::part_id("corner", dsl::tab_part_window + 2)));
        for (int i = 0; i < 4; ++i) rt.tick();
    }
}
if (shot == "cool" || shot == "hints") {
    std::string s = "\x1b[2J\x1b[H";
    const char* cmds[][3] = {{"cargo build --release", "   Compiling hand v0.1.0\r\n    Finished release in 41.2s", "0"},
                             {"make test", "\x1b[31merror:\x1b[0m 2 tests failed (see build/log.txt)", "2"},
                             {"git log --oneline -2", "\x1b[33m3f9a2c1d\x1b[0m terminal: shell integration\r\n\x1b[33m9b1e07aa\x1b[0m docs: https://github.com/1ay1/toe", "0"},
                             {"ping -c1 192.168.1.1", "64 bytes from 192.168.1.1: icmp_seq=1 ttl=64 time=0.41 ms", "0"}};
    for (auto& c : cmds)
        s += std::string{"\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\a"} + c[0] + "\r\n\x1b]133;C\a" +
             c[1] + "\r\n\x1b]133;D;" + c[2] + "\a";
    s += "\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\a";
    rt.send(hand::App::Msg{hand::App::Output{std::move(s)}});
    rt.tick();
    if (shot == "hints") rt.press_key(mayag::Key::space, mayag::Mods{.shift = true, .ctrl = true});
    else rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
        m.tab().cur_known = true; m.tab().cur_x = 3.0f; m.tab().cur_y = 3.0f; }}});   // the comet, mid-flight
}
if (shot == "scrollback") {
    std::string s = "\x1b[2J\x1b[H";
    s += "\x1b]7;file://host" + std::string{std::getenv("HOME") ? std::getenv("HOME") : ""} + "/src/hand\a";
    s += "\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\amake -j8\r\n\x1b]133;C\a";
    for (int i = 0; i < 90; ++i)
        s += "[" + std::to_string(i + 1) + "/90] \x1b[32mBuilding CXX\x1b[0m object src/" + std::string{"term"} + std::to_string(i) + ".cpp.o\r\n";
    if (!std::getenv("HAND_RUNNING")) {
        s += "\x1b[31merror:\x1b[0m linking failed\r\n\x1b]133;D;1\a";
        s += "\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\a";
    }
    rt.send(hand::App::Msg{hand::App::Output{std::move(s)}});
    rt.tick();
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
        hand::App::scroll_by(m, 40.0f);
        m.tab().scroll_d = 39.5f; m.tab().scroll_applied = 39; m.tab().term->set_scroll_offset(39);   // caught mid-glide
        m.tab().new_below = 0; m.tab().sticky_t = 1.0f; m.clock = 3; }}});
}
if (shot == "minimap") {
    std::string s;
    const auto prompt = [&](const std::string& cmd) {
        s += "\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\a" + cmd + "\r\n\x1b]133;C\a";
    };
    for (int round = 0; round < 6; ++round) {
        prompt("git log --oneline");
        for (int i = 0; i < 14; ++i)
            s += "\x1b[33m" + std::to_string(1000000 + i * 7919 + round * 31) + "a\x1b[0m " +
                 std::string("fix: render the minimap for every line ").substr(0, static_cast<std::size_t>(14 + (i * 7) % 26)) + "\r\n";
        s += "\x1b]133;D;0\a";
        prompt("cargo test");
        for (int i = 0; i < 22; ++i)
            s += std::string("test ") + (i % 7 == 3 ? "\x1b[31mFAILED\x1b[0m" : "\x1b[32mok\x1b[0m") +
                 " tests::case_" + std::to_string(i) + std::string(static_cast<std::size_t>(i * 3 % 30), '.') + "\r\n";
        s += round % 2 ? "\x1b]133;D;101\a" : "\x1b]133;D;0\a";
        prompt("ls -la");
        for (int i = 0; i < 9; ++i)
            s += "drwxr-xr-x  ayush  \x1b[1;34mdir" + std::to_string(i) + "\x1b[0m  \x1b[36m" + std::string(static_cast<std::size_t>(4 + i * 4), '#') + "\x1b[0m\r\n";
        s += "\x1b]133;D;0\a";
    }
    prompt("");
    rt.send(hand::App::Msg{hand::App::Output{std::move(s)}});
    rt.tick();
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
        (void)m.tab().term->search("FAILED");
        m.tab().term->set_scroll_offset(120); m.tab().scroll_d = m.tab().scroll_goal = 120.0f; m.tab().scroll_applied = 120;
        m.minimap.open = true; m.minimap.width = 96.0f;
        m.tab().tail_t = 1.0f; m.tab().new_below = 3; }}});
    if (std::getenv("HAND_PEEK"))
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
            m.minimap.hover_y = std::strtof(std::getenv("HAND_PEEK"), nullptr); m.minimap.peek_t = 1.0f; }}});
}
if (shot == "find") {
    rt.send(hand::App::Msg{hand::App::Output{std::string{"build ok\r\nerror: oops\r\nwarning: unused\r\nerror: again\r\n"}}});
    rt.tick();
    rt.press_key(mayag::Key::f, mayag::Mods{.shift = true, .ctrl = true});
    rt.type("error");
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
        m.toast.card = mayag::dsl::Toast{.title = "\u2713 done in 41.2s", .body = "cargo build --release",
                                    .kind = mayag::dsl::Toast::Kind::success};
        m.toast.at = m.time; }}});
}
if (shot == "minimap-short")
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.minimap.open = true; m.minimap.width = 96.0f; }}});
if (shot.rfind("fx-", 0) == 0) {
    static const char* fxn[] = {"none", "ripple", "sonicboom", "wireframe", "pixiedust", "railgun", "torpedo", "fireworks"};
    int k = 0;
    for (int i = 0; i < 8; ++i) if (shot == std::string{"fx-"} + fxn[i]) k = i;
    const bool smear = shot == "fx-smear" || shot == "fx-comet";
    const bool comet = shot == "fx-comet";
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[k, smear, comet](hand::App::Model& m) {
        m.cfg.cursor_fx = static_cast<hand::CursorFx>(k);
        if (comet) { m.cfg.cursor_anim = hand::CursorAnim::glide; m.cfg.cursor_trail = 4.0f; m.tab().cur_known = true;
                     const auto cc = m.tab().term->cursor(); m.tab().cur_x = static_cast<float>(cc.col) + 1.5f; m.tab().cur_y = static_cast<float>(cc.row);
                     m.tab().fx.tx = 38.0f; m.tab().fx.ty = 1.0f; m.focused = true; return; }
        if (smear) { m.cfg.cursor_anim = hand::CursorAnim::smear; m.tab().cur_known = true; m.tab().fx.tx = 34.0f; m.tab().fx.ty = 2.0f; m.focused = true; return; }
        const float cw = m.metrics.cell_w, ch = m.metrics.cell_h;
        const auto cur = m.tab().term->cursor();
        const float cx = m.metrics.origin_x + (static_cast<float>(cur.col) + 0.5f) * cw;
        const float cy = m.metrics.origin_y + (static_cast<float>(cur.row) + 0.5f) * ch;
        hand::motion::emit(m.tab().fx, m.cfg.cursor_fx, cx, cy, cw, 0.0f, cw, ch, true, m.palette);
        hand::motion::emit(m.tab().fx, m.cfg.cursor_fx, cx, cy, cw, 0.0f, cw, ch, true, m.palette);
        if (k == 6)   // torpedo: the puffs it leaves along a flight from the right
            for (int i = 0; i < 14; ++i) {
                hand::motion::travel(m.tab().fx, m.cfg.cursor_fx, cx + static_cast<float>(i) * cw * 1.6f, cy, ch, m.palette);
                hand::motion::step(m.tab().fx, 0.018f);
            }
        hand::motion::step(m.tab().fx, k == 7 ? 0.28f : 0.12f); }}});
}
if (shot.rfind("files", 0) == 0) {   // the file pane beside a shell in this repo
    std::error_code ec;
    std::string here = std::filesystem::current_path(ec).generic_string();
    if (!here.empty() && here[0] != '/') here = "/" + here;
    if (shot == "files-right") rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.cfg.files_left = false; }}});
    rt.send(hand::App::Msg{hand::App::Output{"\x1b]7;file://localhost" + here + "\a\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\agit status -s\r\n\x1b]133;C\a"
        " M src/app/input_keys.cpp\r\n M src/view/window.cpp\r\n?? src/app/pane.cpp\r\n\x1b]133;D;0\a"
        "\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\a"}});
    rt.tick();
    rt.press_key(mayag::Key::e, mayag::Mods{.shift = true, .ctrl = true});
    for (int i = 0; i < 100 && rt.model().pane.loading; ++i) {   // the listing is a task
        rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(20));
        rt.tick();
    }
    rt.press_key(mayag::Key::down, {});
    rt.press_key(mayag::Key::down, {});
    if (shot == "files-filter") {
        rt.press_key(mayag::Key::slash, {});
        rt.type("cmk");
    } else {
        for (int i = 0; i < 5; ++i) rt.press_key(mayag::Key::down, {});
    }
    for (int i = 0; i < 50; ++i) {   // the preview is a task too
        rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(20));
        rt.tick();
    }
    if (shot == "files-narrow" || shot == "files-hover") {   // a narrow pane: long names, a crowded toolbar
        const float w = shot == "files-hover" ? 190.0f : 240.0f;
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[w](hand::App::Model& m) { m.pane.width = w; m.cfg.files_width = w; }}});
        for (int i = 0; i < 4; ++i) rt.tick();
        if (shot == "files-hover") {   // the pointer on the longest name
            const auto& es = rt.model().pane.entries;
            std::size_t best = 0, at = 0;
            const auto vis = mayag::dsl::file_pane_view(es, rt.model().pane.ui, rt.model().cfg.files_dirs_first);
            for (std::size_t q = 0; q < vis.size(); ++q)
                if (es[vis[q].i].name.size() > best) { best = es[vis[q].i].name.size(); at = q; }
            rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[at](hand::App::Model& m) {
                m.pane.ui.scroll.offset.y = std::max(0.0f, static_cast<float>(at) * 31.0f - 120.0f);
            }}});
            for (int i = 0; i < 4; ++i) rt.tick();
            if (const auto r = rt.node_rect(mayag::dsl::part_id("files.row", at))) rt.window().move_to(r->center());
            for (int i = 0; i < 8; ++i) rt.tick();
        }
    }
}
if (shot == "editor" || shot == "editor-find") {   // a file from the pane, open in the editor
    std::error_code ec;
    const std::string here = std::filesystem::current_path(ec).generic_string();
    const std::string path = here + "/src/app/editor.cpp";
    std::ifstream f(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    rt.send(hand::App::Msg{hand::App::Output{"\x1b]7;file://localhost" + here + "/src/app\a"}});
    rt.press_key(mayag::Key::e, mayag::Mods{.shift = true, .ctrl = true});
    for (int i = 0; i < 40; ++i) { rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(10)); rt.tick(); }
    rt.send(hand::App::Msg{hand::App::OpenFile{path}});
    rt.send(hand::App::Msg{hand::App::FileLoaded{rt.model().tab().sid, path, std::move(text), "", false}});
    for (int i = 0; i < 30; ++i) rt.press_key(mayag::Key::down, {});   // into the code, the caret in an update()
    rt.press_key(mayag::Key::end, {});
    rt.type("  // edited");
    if (shot == "editor-find") {
#if defined(__APPLE__)
        rt.press_key(mayag::Key::f, mayag::Mods{.super = true});
#else
        rt.press_key(mayag::Key::f, mayag::Mods{.ctrl = true});
#endif
        rt.type("Model");
    }
    for (int i = 0; i < 60; ++i) { rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(10)); rt.tick(); }   // the tab switch settles
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.strip.t = 1.0f; m.strip.switch_t = 1.0f; hand::App::fit(m); }}});
    for (int i = 0; i < 4; ++i) rt.tick();
}
if (shot == "tabs") {
    using E3 = mayag::Edit<hand::App::Model>;
    const char* outs[] = {"\x1b]7;file://h/home/u/src/hand\a\x1b]133;A\a\u276F \x1b]133;B\acargo build\r\n\x1b]133;C\a   Compiling hand\r\n",
                          "\x1b]7;file://h/home/u/notes\a\x1b]133;A\a\u276F \x1b]133;B\amake test\r\n\x1b]133;C\aerror\r\n\x1b]133;D;2\a",
                          "\x1b]7;file://h/var/log\a\x1b]133;A\a\u276F \x1b]133;B\atail -f syslog\r\n\x1b]133;C\a"};
    for (int t = 0; t < 3; ++t) {
        if (t > 0) rt.press_key(mayag::Key::t, mayag::Mods{.shift = true, .ctrl = true});
        rt.send(hand::App::Msg{hand::App::Output{std::string{outs[t]}}});
        rt.tick();
    }
    rt.send(hand::App::Msg{E3{[](hand::App::Model& m) {
        m.tabs[0].unread = true; m.tabs[1].bell = true; m.strip.t = 1.0f; m.strip.switch_t = 1.0f;
        if (const char* v = std::getenv("HAND_TABSTYLE")) {   // "style,radius,height,bottom,round"
            int st = 0, bot = 0; float r = -1, hgt = 40, rd = 1;
            std::sscanf(v, "%d,%f,%f,%d,%f", &st, &r, &hgt, &bot, &rd);
            m.cfg.tab_style = static_cast<hand::TabStyle>(st); m.cfg.tab_radius = r; m.cfg.tab_height = hgt;
            m.cfg.tab_bottom = bot != 0; m.cfg.roundness = rd;
        }
        hand::App::fit(m);
        for (int i = 0; i < 12; ++i) m.tabs[0].activity.push_back(0.2f + 0.06f * static_cast<float>(i % 5)); }}});
    for (int i = 0; i < 5; ++i) rt.tick();
    if (const char* hv = std::getenv("HAND_TABHOVER")) {   // hover tab N (screenshots of hover states)
        rt.window().move_to(rt.host().center_of(dsl::part_id("tabs", static_cast<std::size_t>(std::atoi(hv)))));
        for (int i = 0; i < 30; ++i) rt.tick();
    }
}
if (shot == "hosts" || shot == "ssh" || shot == "ssh-drop" || shot.rfind("manager", 0) == 0) {
    using E7 = mayag::Edit<hand::App::Model>;
    rt.send(hand::App::Msg{E7{[](hand::App::Model& m) {
        const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
        const auto add = [&](const char* a, const char* host, const char* user, int port, const char* jump, hand::HostEnv env,
                             bool pin, int uses, std::int64_t ago, std::vector<std::string> tags, hand::HostSource src) {
            hand::Host h; h.alias = a; h.hostname = host; h.user = user; h.port = port; h.jump = jump; h.env = env;
            h.pinned = pin; h.uses = uses; h.last = uses ? now - ago : 0; h.tags = std::move(tags); h.source = src;
            m.ssh.book.hosts.push_back(std::move(h));
        };
        using HE = hand::HostEnv; using HS = hand::HostSource;
        add("prod-db", "10.0.3.4", "deploy", 22, "bastion", HE::prod, true, 31, 3 * 3600, {"db", "eu"}, HS::config);
        add("bastion", "bastion.example.com", "ops", 22, "", HE::none, true, 54, 3 * 3600, {}, HS::config);
        add("gpu-box", "gpu.lan", "ayush", 2222, "", HE::dev, false, 12, 2 * 86400, {"cuda"}, HS::config);
        add("api.staging", "api.staging.example.com", "ops", 22, "bastion", HE::staging, false, 6, 5 * 86400, {}, HS::config);
        add("pi", "raspberrypi.local", "pi", 22, "", HE::none, false, 3, 18 * 86400, {"home"}, HS::config);
        add("build-01", "build-01.ci.internal", "ci", 22, "", HE::none, false, 0, 0, {"ci"}, HS::config);
        add("build-02", "build-02.ci.internal", "ci", 22, "", HE::none, false, 0, 0, {"ci"}, HS::config);
        add("seen.example.org", "seen.example.org", "", 22, "", HE::none, false, 0, 0, {}, HS::known);
        m.ssh.book.config_label = "~/.ssh/config";
    }}});
    rt.send(hand::App::Msg{hand::App::Output{std::string{
        "\x1b]7;file://h/home/u/src/hand\a\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\a"}}});
    rt.tick();
    if (shot == "hosts") {
        rt.send(hand::App::Msg{E7{[](hand::App::Model& m) { m.ssh.picker.open = true; m.ssh.picker.sel = 0; }}});
    } else if (shot.rfind("manager", 0) == 0) {
        // the host manager: a detail view, editing a config host, a new host
        rt.send(hand::App::Msg{E7{[shot](hand::App::Model& m) {
            {   // a reference into hosts dies at the push_back below: use it only here
                auto& db = m.ssh.book.hosts[0];
                db.command = "tmux new -A -s main";
                db.forwards = {"L5432:localhost:5432", "L3000:grafana:3000", "D1080"};
            }
            hand::Host mine; mine.alias = "lab-nas"; mine.hostname = "192.168.1.20"; mine.user = "admin";
            mine.port = 2200; mine.identity = "~/.ssh/id_ed25519"; mine.source = hand::HostSource::saved;
            mine.tags = {"home"}; mine.uses = 4; mine.last = static_cast<std::int64_t>(std::time(nullptr)) - 7200;
            m.ssh.book.hosts.push_back(mine);
            // folders that share settings, live tunnels, other clients' sessions
            for (auto& h : m.ssh.book.hosts) {
                if (h.alias == "build-01" || h.alias == "build-02") {
                    h.folder = "Work/CI"; h.source = hand::HostSource::saved; h.user.clear(); h.jump.clear();
                }
                if (h.alias == "api.staging" || h.alias == "gpu-box") h.folder = "Work";
                if (h.alias == "lab-nas" || h.alias == "pi") h.folder = "Home";
            }
            m.ssh.book.folders = {{"Work", "ops", "bastion", "~/.ssh/work_ed25519", 0}, {"Work/CI", "ci", "", "", 0}};
            m.ssh.manager.ports = {3000, 1080, 5432}; m.ssh.manager.port_owners = {"ssh", "ssh", "postgres"};
            hand::ImportSource ps{"PuTTY", "HKCU\\Software\\SimonTatham\\PuTTY\\Sessions", {}};
            for (const char* a : {"router", "nas-old", "vpn-gw"}) { hand::Host h; h.alias = a; h.hostname = a; ps.hosts.push_back(h); }
            m.ssh.manager.imports = {ps, hand::ImportSource{"MobaXterm", "%APPDATA%\\MobaXterm\\MobaXterm.ini", {m.ssh.book.hosts[1]}}};
            m.ssh.manager.open = true; m.ssh.manager.t = 1.0f; m.ssh.manager.ui.sel = 0;
            const auto hosts = hand::detail::picker_hosts(m);
            if (shot == "manager-import") m.ssh.manager.ui.import_open = true;
            if (shot == "manager-folder") {
                std::vector<mayag::dsl::SshFolder> fl;
                for (const auto& f : m.ssh.book.folders) fl.push_back({f.name, f.user, f.jump, f.identity, f.port});
                mayag::dsl::ssh_manager_ops::begin_folder_edit(m.ssh.manager.ui, fl, "Work");
            }
            if (shot == "manager-files") {
                m.ssh.manager.ui.tab = 1; m.ssh.manager.ui.files_sel = 3;
                m.ssh.manager.files = mayag::dsl::SshFilesView{.alias = "prod-db", .path = "/home/deploy/app"};
                using RE = mayag::dsl::SshRemoteEntry;
                m.ssh.manager.files.entries = {RE{"config", "Nov 18 22:01", "", true, false, 4096}, RE{"logs", "Nov 20 09:12", "", true, false, 4096},
                                   RE{"current", "Nov 10 10:00", "releases/42", false, true, 11}, RE{"backup-2024-11-19.sql.gz", "Nov 19 03:00", "", false, false, 1874329600},
                                   RE{"deploy.sh", "Oct  2 14:20", "", false, false, 2311}, RE{"README.md", "Sep 30 11:05", "", false, false, 8730}};
                m.ssh.manager.files.busy = "Downloading backup-2024-11-19.sql.gz\u2026";
            }
            if (shot == "manager-folder-host") {
                for (std::size_t q = 0; q < m.ssh.book.hosts.size(); ++q) if (m.ssh.book.hosts[q].alias == "build-01") m.ssh.manager.ui.sel = static_cast<int>(q);
            }
            if (shot == "manager-edit") mayag::dsl::ssh_manager_ops::begin_edit(m.ssh.manager.ui, hosts, 0);
            if (shot == "manager-new") {
                mayag::dsl::ssh_manager_ops::begin_edit(m.ssh.manager.ui, hosts, -1, "deploy@10.0.9.7:2222");
                m.ssh.manager.ui.fields[static_cast<std::size_t>(mayag::dsl::SshField::alias)].text = "metrics";
                m.ssh.manager.ui.fields[static_cast<std::size_t>(mayag::dsl::SshField::forwards)].text = "L3000:grafana:3000, 9090, L99999";
                m.ssh.manager.ui.env = 2;
                m.ssh.manager.ui.focus = mayag::dsl::SshField::forwards;
                m.ssh.manager.ui.detail_scroll.offset.y = 330.0f;
            }
        }}});
    } else {
        // connect to gpu-box and prod-db; prod-db is the one in view
        for (const char* a : {"gpu-box", "prod-db"}) {
            rt.send(hand::App::Msg{E7{[a](hand::App::Model& m) { (void)hand::App::update(m, hand::App::ConnectHost{*m.ssh.book.find(a)}); }}});
            rt.send(hand::App::Msg{hand::App::Output{std::string{
                "Welcome to Ubuntu 24.04 LTS (GNU/Linux 6.8.0-41-generic x86_64)\r\n\r\n"
                "  System load:  0.08    Memory usage: 31%    Users logged in: 1\r\n\r\n"
                "Last login: Tue Nov 19 09:12:44 2024 from 10.0.0.2\r\n"
                "\x1b[1;31mdeploy@prod-db\x1b[0m:\x1b[1;34m~\x1b[0m$ systemctl status postgresql\r\n"
                "\x1b[1;32m\u25CF\x1b[0m postgresql.service - PostgreSQL RDBMS\r\n"
                "     Active: \x1b[1;32mactive (exited)\x1b[0m since Mon 2024-11-18 22:01:03 UTC\r\n"
                "\x1b[1;31mdeploy@prod-db\x1b[0m:\x1b[1;34m~\x1b[0m$ "}, hand::kNoSession}});
            rt.tick();
        }
        if (shot == "ssh-drop")
            rt.send(hand::App::Msg{E7{[](hand::App::Model& m) { (void)hand::App::update(m, hand::App::Exited{255, m.tab().sid}); m.tab().link.dropped_at = m.time - 10.0; }}});
        rt.send(hand::App::Msg{E7{[](hand::App::Model& m) { m.strip.t = 1.0f; m.strip.switch_t = 1.0f; hand::App::fit(m); }}});
    }
    for (int i = 0; i < 8; ++i) rt.tick();
}
if (shot == "select" || shot == "copymode" || shot == "paste" || shot == "quit") {
    rt.send(hand::App::Msg{hand::App::Output{std::string{
        "\x1b[2J\x1b[H\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\agit log --oneline -3\r\n\x1b]133;C\a"
        "\x1b[33m3f9a2c1\x1b[0m terminal: shell integration\r\n\x1b[33m9b1e07a\x1b[0m docs: https://github.com/1ay1/toe\r\n"
        "\x1b[33m77c4d10\x1b[0m scrollback: smooth glide\r\n\x1b]133;D;0\a"
        "\x1b]133;A\a\x1b[1;34m~/src/hand\x1b[0m \x1b[35m\u276F\x1b[0m \x1b]133;B\a"}}});
    rt.tick();
    const std::string which = shot;
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[which](hand::App::Model& m) {
        const std::int64_t r0 = m.tab().term->view_to_abs(0);
        if (which == "select") m.tab().term->select_abs(r0 + 2, 20, r0 + 2, 43, 0);
        if (which == "copymode") {
            m.tab().copy = hand::CopyMode{.row = r0 + 3, .col = 6, .visual = hand::CopyMode::Visual::block, .anchor_row = r0 + 1, .anchor_col = 0};
            m.tab().term->select_abs(r0 + 1, 0, r0 + 3, 6, 2);
        }
        if (which == "paste") {
            m.dialog.ask = hand::Ask{.kind = hand::Ask::Kind::paste, .focus = 2, .paste = "sudo pacman -Syu\nrm -rf build\ncmake --build build\n"};
            m.dialog.t = 1.0f;
        }
        if (which == "quit") { m.dialog.ask = hand::Ask{.kind = hand::Ask::Kind::quit, .focus = 1}; m.dialog.t = 1.0f; }
    }}});
    rt.tick();
}
if (shot == "resize")
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.resized_at = m.time; }}});
if (shot == "settings") {
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
        m.settings.open = true; m.settings.t = 1.0f; m.settings.ui.selected = 1;
        if (const char* v = std::getenv("HAND_SETTINGS")) {   // "row[,query]": another page, or a search
            const std::string s = v;
            m.settings.ui.selected = std::atoi(s.c_str());
            if (const auto c = s.find(','); c != std::string::npos) m.settings.ui.query = s.substr(c + 1);
            m.cfg.font_size = 14.0f; m.font_size = 14.0f; m.cfg.cursor = hand::CursorShape::beam;   // two changed values
        }
    }}});
}
if (shot == "settings-panel")
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.settings.open = true; m.settings.t = 1.0f; m.settings.ui.selected = 1; }}});
if (shot == "settings-ui-font") {   // the interface font switched live, its menu open
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
        m.settings.open = true; m.settings.t = 1.0f;
        if (m.cfg.ui_fonts.size() > 1) m.cfg.ui_font = 1;
    }}});
    rt.tick();
    for (int i = 0; i < 4; ++i) rt.press_key(mayag::Key::down);
    rt.press_key(mayag::Key::enter);
}
if (shot == "settings-menu") {
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.settings.open = true; m.settings.t = 1.0f; }}});
    rt.tick();
    for (int i = 0; i < 3; ++i) rt.press_key(mayag::Key::down);
    rt.press_key(mayag::Key::enter);
    rt.press_key(mayag::Key::down);
}
rt.tick();
const auto px = rt.window().read_pixels();
const int W = static_cast<int>(size.x * 2.0f);
const int H = static_cast<int>(px.size() / 4 / static_cast<std::size_t>(W));
if (!imageio::write_png(out, px, W, H)) return 1;
std::printf("hand: wrote %s (%dx%d)\n", out.c_str(), W, H);
return 0;
}

}  // namespace hand::cli
