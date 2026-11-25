// hand::cli — screenshots and benchmarks (`hand --png out.png --shot NAME`,
// `hand --bench`, `hand --flood CMD`): stage the model for one named scene,
// render a frame, write the PNG.

#include <mayag/core/target.hpp>
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
#include <map>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <hand/core/platform.hpp>
#include <hand/core/proc.hpp>
#include <csignal>

#if MAYAG_OS_WINDOWS
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
        // The steady state, not the catch-up: the first frames after a jump
        // pay once for whole-file work (syntax state lexed to the caret,
        // bracket depth, glyphs never drawn before) and were averaged in, so
        // typing at the end of a 100k-line file read 3.2 ms against 2.7 at the
        // top -- over 1500 keys both are 2.4. Those frames are not hidden:
        // `worst` is the slowest frame of warm-up and run together.
        double worst = 0;
        for (int i = 0; i < 10; ++i) {
            poke(N + i);
            const auto a = clk::now();
            rt.tick();
            worst = std::max(worst, std::chrono::duration<double, std::milli>(clk::now() - a).count());
        }
        double view = 0, cascade = 0, layout = 0, paint = 0, render = 0, fold = 0;
        const auto reused0 = rt.window().tiles_reused(), total0 = rt.window().tiles_total();
        const auto t0 = clk::now();
        for (int i = 0; i < N; ++i) {
            poke(i);   // the input (posted: one input, one frame)
            const auto a = clk::now();
            rt.tick();
            const double ms = std::chrono::duration<double, std::milli>(clk::now() - a).count();
            fold += ms;
            worst = std::max(worst, ms);
            const auto& f = rt.host().latency().last();
            view += f.view_ms; cascade += f.cascade_ms; layout += f.layout_ms; paint += f.paint_ms; render += f.render_ms;
        }
        const double total = std::chrono::duration<double, std::milli>(clk::now() - t0).count() / N;
        const auto tiles = rt.window().tiles_total() - total0;
        const double reuse = tiles ? 100.0 * static_cast<double>(rt.window().tiles_reused() - reused0) / static_cast<double>(tiles) : 0.0;
        std::printf("  %-26s %6.3f ms/frame   frame %.3f  view %.3f  cascade %.3f  layout %.3f  paint %.3f  raster %.3f  reuse %4.1f%%  worst %.2f\n",
                    what, total, fold / N, view / N, cascade / N, layout / N, paint / N, render / N, reuse, worst);
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
    {   // the editor on a big file: 100k lines of C++ (~4 MB)
        std::string text;
        text.reserve(5u << 20);
        for (int i = 0; i < 100000; ++i)
            text += i % 9 == 0 ? "// section " + std::to_string(i) + ": what the next lines are for\n"
                  : i % 9 == 1 ? "static int f" + std::to_string(i) + "(const std::vector<int>& v, int k) {\n"
                  : i % 9 == 7 ? "    return static_cast<int>(v.size()) + k * " + std::to_string(i) + ";\n"
                  : i % 9 == 8 ? "}\n"
                  : "    if (v.empty() || k < 0) { std::printf(\"%d\\n\", k); }   // " + std::to_string(i) + "\n";
        const std::string path = "/tmp/hand-bench-big.cpp";
        rt.send(hand::App::Msg{hand::App::OpenFile{path}});
        const auto a = clk::now();
        rt.send(hand::App::Msg{hand::App::FileLoaded{rt.model().tab().sid, path, std::move(text), "", false, 1}});
        rt.tick();
        std::printf("  %-26s %6.3f ms   (load into the buffer + first frame)\n", "editor: open 100k lines",
                    std::chrono::duration<double, std::milli>(clk::now() - a).count());
        for (int i = 0; i < 10; ++i) rt.tick();
        run("editor: typing", [&](int i) { rt.window().type(std::string(1, static_cast<char>('a' + i % 26))); });
        run("editor: scrolling", [&](int i) { rt.window().scroll(Vec2{900.0f, 300.0f}, Vec2{0.0f, (i / 30) % 2 ? -60.0f : 60.0f}); });
        run("editor: page down", [&](int) { rt.window().press_key(mayag::Key::page_down, {}); });
        {
            // what a person does between opening and jumping: nothing, for a
            // moment -- in which the editor lexes ahead (WarmSyntax slices)
            const auto w = clk::now();
            rt.settle([](const hand::App::Model& m) { return !m.tab().file || m.tab().file->st.ed.syntax_warm(); },
                      std::chrono::milliseconds(1000));
            std::printf("  %-26s %6.1f ms   (lexed ahead in the background)\n", "editor: warm the whole file",
                        std::chrono::duration<double, std::milli>(clk::now() - w).count());
            const auto b = clk::now();
            rt.press_key(mayag::Key::end, mayag::Mods{.super = true});
            rt.press_key(mayag::Key::down, mayag::Mods{.ctrl = true});
            std::printf("  %-26s %6.3f ms\n", "editor: jump to the end", std::chrono::duration<double, std::milli>(clk::now() - b).count());
        }
        run("editor: typing at the end", [&](int i) { rt.window().type(std::string(1, static_cast<char>('a' + i % 26))); });
    }
    {   // ⌘P over a 50k-file project (the list's cap): every key re-ranks, every frame draws the list
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
            m.quick = {};
            m.quick.open = true;
            m.quick.root = "/tmp/big";
            auto files = std::make_shared<std::vector<std::string>>();
            for (int i = 0; i < 50000; ++i)
                files->push_back("src/module" + std::to_string(i % 97) + "/sub" + std::to_string(i % 13) + "/file_" + std::to_string(i) + ".cpp");
            m.quick.files = std::move(files);
        }}});
        rt.tick();
        run("quick open: typing, 50k", [&](int i) {
            if (i % 8 < 4) rt.post(hand::App::Msg{hand::App::QuickText{std::string(1, "mfsx"[i % 8])}});
            else rt.post(hand::App::Msg{hand::App::QuickKey{mayag::Key::backspace, {}}});
        });
        run("quick open: pointer moving", [&](int i) { rt.window().move_to(Vec2{500.0f + static_cast<float>(i % 60) * 3.0f, 200.0f}); });
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.quick = {}; }}});
        rt.tick();
    }
    {   // ⌘⇧F with its 2000 hits (the cap)
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) {
            m.search_all = {};
            m.search_all.open = true;
            m.search_all.q.set("needle");
            for (int i = 0; i < 2000; ++i)
                m.search_all.hits.push_back({.file = "src/f" + std::to_string(i / 7) + ".cpp", .line = i, .col = 12, .len = 6,
                                             .text = "    auto x = needle(" + std::to_string(i) + ", v.size(), k * 3);   // what it is for"});
            m.search_all.truncated = true;
        }}});
        rt.tick();
        run("search all: moving, 2000", [&](int i) { rt.post(hand::App::Msg{hand::App::SearchAllKey{(i / 30) % 2 ? mayag::Key::up : mayag::Key::down, {}}}); });
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.search_all = {}; }}});
        rt.tick();
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
if (shot == "motion") {   // every animation on its own: how long it moves, what a frame of it costs, what it repaints
    std::string s;
    for (int i = 0; i < 300; ++i) s += "\x1b[3" + std::to_string(1 + i % 6) + "mline " + std::to_string(i) + "\x1b[0m " + std::string(20 + i % 50, 'x') + "\r\n";
    rt.send(hand::App::Msg{hand::App::Output{s}});
    rt.window().drive_frames(true);
    using Msg = hand::App::Msg;
    const auto quiet = [&] { for (int i = 0; i < 600 && rt.host().wants_frames(); ++i) rt.tick(); };
    int bad = 0;
    const auto run = [&](const char* what, auto&& start) {
        if (const char* only = std::getenv("HAND_MOTION_ONLY"); only && !std::string_view{what}.starts_with(only)) return;
        quiet();
        rt.tick(); rt.tick();   // two idle frames: nothing owed
        const auto t0 = rt.window().tiles_total(), r0 = rt.window().tiles_reused();
        start();
        int frames = 0, still = 0;
        std::string why;   // who asked for the frames that changed nothing
        double cpu = 0, worst = 0;
        for (; frames < 600 && rt.host().wants_frames(); ++frames) {
            const auto d0 = rt.window().tiles_total() - rt.window().tiles_reused();
            std::string reason = rt.host().frame_reason();
            if (reason == "on_frame") reason = hand::frames_wanted_by(rt.model());
            rt.tick();
            if (rt.window().tiles_total() - rt.window().tiles_reused() == d0) {   // drew, but no pixel changed
                ++still;
                if (!reason.empty() && why.find(reason) == std::string::npos) why += (why.empty() ? "" : ",") + reason;
            }
            const auto& f = rt.host().latency().last();
            cpu += f.widget_cpu_ms;
            worst = std::max(worst, f.widget_cpu_ms);
        }
        const double tiles = static_cast<double>(rt.window().tiles_total() - t0);
        const double drawn = tiles - static_cast<double>(rt.window().tiles_reused() - r0);
        const bool settles = frames < 600;
        // a frame that changed no pixel is a frame the display did not need:
        // a fade past visible, a timer counted in frames. A few round off; more
        // is a bug of the kind this shot found (a toast held 193 such frames).
        const bool lean = still <= std::max(1, std::min(2, frames / 10));
        bad += settles && lean ? 0 : 1;
        std::printf("  %-28s %4d frames (%4.0f ms) %3d still  cpu %.3f ms/frame worst %.3f   repaints %5.1f%% a frame%s%s%s\n", what, frames,
                    frames * 1000.0 / 60.0, still, cpu / std::max(frames, 1), worst, 100.0 * drawn / std::max(tiles, 1.0),
                    why.empty() ? "" : "   still from: ", why.c_str(), !settles ? "   NEVER SETTLES" : !lean ? "   WASTES FRAMES" : "");
    };
    std::printf("motion: each animation alone, from rest to rest (headless, 60 Hz steps)\n");
    run("new output line glides in", [&] { rt.send(Msg{hand::App::Output{"one more line\r\n"}}); });
    run("scroll a notch", [&] { rt.window().scroll(Vec2{400.0f, 300.0f}, Vec2{0.0f, 48.0f}); rt.tick(); });
    run("scroll back to live", [&] { rt.window().scroll(Vec2{400.0f, 300.0f}, Vec2{0.0f, -480.0f}); rt.tick(); });
    run("cursor glides", [&] { rt.send(Msg{hand::App::Output{"\x1b[12;40H"}}); });
    run("bell", [&] { rt.send(Msg{hand::App::Output{"\a"}}); });
    run("new tab (strip appears)", [&] { rt.send(Msg{hand::App::NewTab{}}); });
    run("switch tab", [&] { rt.send(Msg{hand::App::StepTab{hand::Step::backward}}); });
    run("settings open", [&] { rt.send(Msg{hand::App::ToggleSettings{}}); });
    run("settings close", [&] { rt.send(Msg{hand::App::ToggleSettings{}}); });
    run("host manager open", [&] { rt.send(Msg{hand::App::ToggleManager{}}); });
    run("host manager close", [&] { rt.send(Msg{hand::App::ToggleManager{}}); });
    run("file pane open", [&] { rt.send(Msg{hand::App::TogglePane{}}); });
    run("file pane close", [&] { rt.send(Msg{hand::App::TogglePane{}}); });
    run("toast (copy output)", [&] { rt.send(Msg{hand::App::CopyOutput{}}); });
    run("window resize (size HUD)", [&] { rt.window().resize(Vec2{size.x - 40.0f, size.y}, 2.0f); rt.tick(); });
    run("close-tab dialog", [&] { rt.send(Msg{hand::App::CloseTab{hand::TabIndex{rt.model().active()}}}); });
    run("dialog dismissed", [&] { rt.press_key(mayag::Key::escape); });
    run("pointer over the tab strip", [&] { rt.window().move_to(Vec2{120.0f, 14.0f}); rt.tick(); });
    run("pointer leaves it", [&] { rt.window().move_to(Vec2{600.0f, 400.0f}); rt.tick(); });
    std::printf("%s\n", bad ? "FAIL: an animation never came to rest, or drew frames that changed nothing"
                             : "PASS: every animation comes to rest, and its frames change pixels");
    return bad ? 1 : 0;
}
if (shot == "idle") {   // a REAL shell, at rest and running a command: how many frames, and who asks
    using clk = std::chrono::steady_clock;
    rt.settle([](const auto& m) { return static_cast<bool>(m.tab().pty); }, std::chrono::seconds(3));
    rt.window().drive_frames(true);   // the frame clock runs, as a display link would
    const auto measure = [&](const char* what, double secs) -> void {
        std::map<std::string, int> why;
        const auto t0 = clk::now();
        while (clk::now() - t0 < std::chrono::duration<double>(secs)) {
            rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(16));
            const bool wanted = rt.host().wants_frames();   // asked BEFORE the tick: only frames something wanted count
            std::string r = rt.host().frame_reason();
            if (r == "on_frame") r = hand::frames_wanted_by(rt.model());
            if (wanted) { rt.tick(); ++why[r]; }
            else rt.tick();
        }
        const double fps = static_cast<double>(why.empty() ? 0 : [&] { int n = 0; for (const auto& [k, v] : why) n += v; return n; }()) / secs;
        std::printf("  %-28s %5.1f frames/s", what, fps);
        for (const auto& [k, n] : why) std::printf("  [%s x%d]", k.c_str(), n);
        std::printf("\n");
    };
    std::printf("idle: a real shell (%s)\n", rt.model().cfg.shell_cli.c_str());
    measure("prompt, just started", 1.0);
    measure("prompt, idle", 2.0);
    rt.send(hand::App::Msg{hand::App::Typed{"sleep 3\r"}});
    measure("running `sleep 3`", 2.0);
    // (the spinner alone, with nothing else drawing, is mayag's own check:
    // test_embedded "a spinner alone keeps its own time", on the Cocoa loop logic)
    std::printf("(frames counted are the ones the model or a widget ASKED for; a fade blink asks ~18/s while it fades)\n");
    return 0;
}
if (shot == "panelag") {   // the file pane open on a real folder: what every pane message costs
    using clk = std::chrono::steady_clock;
    rt.send(hand::App::Msg{hand::App::TogglePane{}});
    // the pane follows the shell: run with `-e "sh -c 'cd DIR; exec cat'"` for a big folder
    const auto t0 = clk::now();
    while (clk::now() - t0 < std::chrono::seconds(2)) {
        rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(16));
        rt.tick();
    }
    // scroll the list like a trackpad: 120 wheel steps, one a frame
    double worst = 0, sum = 0;
    const float x = rt.model().cfg.files_left ? 60.0f : rt.model().win.x - 60.0f;
    const int steps = std::getenv("HAND_PANE_STEPS") ? std::atoi(std::getenv("HAND_PANE_STEPS")) : 120;
    for (int i = 0; i < steps; ++i) {
        const auto a = clk::now();
        rt.window().scroll(Vec2{x, 300.0f}, Vec2{0.0f, (i / 40) % 2 ? 30.0f : -30.0f});
        rt.tick();
        const double ms = std::chrono::duration<double, std::milli>(clk::now() - a).count();
        worst = std::max(worst, ms); sum += ms;
    }
    std::printf("panelag: %zu entries, path %s, slow folds %llu, scroll %.2f ms/frame worst %.2f\n", rt.model().pane.entries.size(),
                rt.model().pane.path.c_str(), static_cast<unsigned long long>(rt.slow_folds()), sum / steps, worst);
    return 0;
}
if (shot == "echo") {   // keystroke -> pty -> echo -> parsed -> frame drawn: hand's whole share of input-to-photon
    using clk = std::chrono::steady_clock;
    // wait for the program to come up and go quiet
    rt.settle([](const auto& m) { return static_cast<bool>(m.tab().pty); }, std::chrono::seconds(3));
    // HAND_ECHO_LOAD=cmd: the same measurement with a firehose running in a
    // SECOND tab ("yes", "cat /dev/urandom | base64"): a key must still echo
    // in a frame or two while another shell floods the loop.
    if (const char* load = std::getenv("HAND_ECHO_LOAD"); load && *load) {
        const std::string echo_cli = rt.model().cfg.shell_cli;
        const std::vector<std::string> echo_args = rt.model().cfg.shell_args;
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[l = std::string{load}](hand::App::Model& m) {
            if (l == "yes") { m.cfg.shell_cli = hand::self_exe(); m.cfg.shell_args = {"--yes"}; }   // hand's own, on every OS
            else { m.cfg.shell_cli = l; m.cfg.shell_args.clear(); }
        }}});
        rt.send(hand::App::Msg{hand::App::NewTab{}});
        rt.settle([](const auto& m) { return static_cast<bool>(m.tab().pty); }, std::chrono::seconds(3));
        rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[echo_cli, echo_args](hand::App::Model& m) { m.cfg.shell_cli = echo_cli; m.cfg.shell_args = echo_args; }}});
        rt.send(hand::App::Msg{hand::App::SelectTab{hand::TabIndex{0}}});   // back to the echo tab; the flood runs behind it
        for (int i = 0; i < 30; ++i) { rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(5)); rt.tick(); }
    }
    for (int i = 0; i < 20; ++i) { rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(5)); rt.tick(); }
    const int n = std::getenv("HAND_ECHO_N") ? std::atoi(std::getenv("HAND_ECHO_N")) : 200;
    // HAND_ECHO_GAP_MS: idle between keys, as a person types (~60-120 ms).
    // Back to back (0, the default) measures a different thing on Windows:
    // conhost's renderer rests one 16 ms frame after every paint, so a key
    // sent the instant the last echo landed always waits out that rest.
    const int gap_ms = std::getenv("HAND_ECHO_GAP_MS") ? std::atoi(std::getenv("HAND_ECHO_GAP_MS")) : 0;
    std::vector<double> lat;
    lat.reserve(static_cast<std::size_t>(n));
    int lost = 0;
    double trip = 0, frame = 0, keyframe = 0;
    for (int i = 0; i < n; ++i) {
        if (gap_ms > 0) {   // idle, but keep the loop turning (a real window would)
            const auto until = clk::now() + std::chrono::milliseconds(gap_ms);
            while (clk::now() < until) { rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(4)); rt.tick(); }
        }
        const std::uint64_t before = rt.model().tab().bytes;
        const auto a = clk::now();
        rt.type(std::string(1, static_cast<char>('a' + i % 26)));   // the key, as the window delivers it
        keyframe += std::chrono::duration<double, std::milli>(clk::now() - a).count();
        if (std::getenv("HAND_ECHO_DIAG") && i < 4) { const auto& f = rt.host().latency().last(); std::printf("  key frame: view %.3f cascade %.3f layout %.3f paint %.3f raster %.3f\n", f.view_ms, f.cascade_ms, f.layout_ms, f.paint_ms, f.render_ms); }
        const bool back = rt.settle([before](const auto& m) { return m.tab().bytes > before; }, std::chrono::milliseconds(500));
        const auto b = clk::now();
        rt.tick();                                                   // the frame that shows it
        if (!back) {
            ++lost;
            std::printf("  key %d ('%c') got no echo in 500 ms (bytes %llu -> %llu)\n", i, static_cast<char>('a' + i % 26),
                        static_cast<unsigned long long>(before), static_cast<unsigned long long>(rt.model().tab().bytes));
            continue;
        }
        trip += std::chrono::duration<double, std::milli>(b - a).count();
        frame += std::chrono::duration<double, std::milli>(clk::now() - b).count();
        lat.push_back(std::chrono::duration<double, std::milli>(clk::now() - a).count());
        if (std::getenv("HAND_ECHO_DIAG") && lat.back() > 10.0)
            std::printf("  slow key %d: %.2f ms (to echo %.2f)\n", i, lat.back(), std::chrono::duration<double, std::milli>(b - a).count());
        if (i % 32 == 31) {
            // enter runs the line: wait for the shell to finish it and redraw
            // its prompt (quiet for 30 ms), or the next key measures the
            // shell's prompt hooks, not hand
            rt.press_key(mayag::Key::enter);
            const auto until = clk::now() + std::chrono::seconds(2);
            for (std::uint64_t seen = rt.model().tab().bytes; clk::now() < until;) {
                rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(30));
                rt.tick();
                if (rt.model().tab().bytes == seen) break;
                seen = rt.model().tab().bytes;
            }
        }
    }
    std::sort(lat.begin(), lat.end());
    const auto pct = [&](double p) { return lat.empty() ? 0.0 : lat[std::min(lat.size() - 1, static_cast<std::size_t>(p * static_cast<double>(lat.size())))]; };
    std::printf("echo: %zu keys  p50 %.2f ms  p95 %.2f ms  max %.2f ms  min %.2f ms  lost %d  (key fold+frame %.2f, ->echo parsed %.2f, echo frame %.2f)\n", lat.size(), pct(0.5), pct(0.95),
                lat.empty() ? 0.0 : lat.back(), lat.empty() ? 0.0 : lat.front(), lost, keyframe / std::max<double>(1, lat.size()), trip / std::max<double>(1, lat.size()), frame / std::max<double>(1, lat.size()));
    // the gate: an order of magnitude over the baseline, so a regression trips it and noise doesn't
    const double p50_max = std::getenv("HAND_ECHO_P50") ? std::atof(std::getenv("HAND_ECHO_P50")) : 8.0;
    return (lost == 0 && pct(0.5) < p50_max) ? 0 : 1;
}
if (shot == "drag") {   // the scrollback through a window drag, the whole app: real shell, frames, the real resize path
    using clk = std::chrono::steady_clock;
    const auto idle = [&](int ms) {
        const auto until = clk::now() + std::chrono::milliseconds(ms);
        while (clk::now() < until) { rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(8)); rt.tick(); }
    };
    rt.settle([](const auto& m) { return static_cast<bool>(m.tab().pty); }, std::chrono::seconds(5));
    idle(2500);   // the shell's banner and first prompt
    const int N = std::getenv("HAND_DRAG_N") ? std::atoi(std::getenv("HAND_DRAG_N")) : 400;
    const std::string sh = hand::Pty::resolve_shell("");
    const bool ps = sh.find("pwsh") != std::string::npos || sh.find("powershell") != std::string::npos;
    const bool cmd = sh.find("cmd") != std::string::npos;
    const std::string n1 = std::to_string(N - 1);
    // mixed lengths: every 7th line wraps at narrow widths
    rt.type(ps ? "0.." + n1 + " | % { \"L$_.\" + $(if ($_ % 7 -eq 0) { 'w' * 150 } else { 'x' * ($_ % 40) }) }"
               : cmd ? "for /L %i in (0,1," + n1 + ") do @echo L%i.xxxxxxxxxxxxxxxx"
                     : "i=0; while [ $i -lt " + std::to_string(N) + " ]; do echo L$i.xxxx; i=$((i+1)); done");
    rt.press_key(mayag::Key::enter);
    const auto& term = [&]() -> const hand::Term& { return *rt.model().tab().term; };
    int dups = 0;
    const auto count = [&] {
        std::string all;
        for (std::int64_t a = 0; a < term().total_rows(); ++a) {
            for (char32_t c : term().line(a)) all += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : ' ';
            if (!term().wrapped(a)) all += '\n';
        }
        std::vector<int> times(static_cast<std::size_t>(N), 0);
        int distinct = 0;
        for (std::size_t i = 0; (i = all.find('L', i)) != std::string::npos; ++i) {
            std::size_t j = i + 1;
            int v = 0;
            bool d = false;
            while (j < all.size() && all[j] >= '0' && all[j] <= '9') { v = v * 10 + (all[j] - '0'); ++j; d = true; }
            if (d && j < all.size() && all[j] == '.' && v < N && (i == 0 || all[i - 1] == '\n'))
                if (times[static_cast<std::size_t>(v)]++ == 0) ++distinct;
        }
        dups = 0;
        for (int c : times) dups += c > 1 ? c - 1 : 0;
        return distinct;
    };
    for (int w = 0; w < 60000 && count() < N; w += 250) idle(250);
    idle(800);
    const int before = count(), dups0 = dups;
    const mayag::Vec2 s0 = size;
    // the drag: HAND_DRAG_SHAPE w(idth) / h(eight) / b(oth); steps of HAND_DRAG_PX px a frame, out and back
    const char* shp = std::getenv("HAND_DRAG_SHAPE");
    const char shape = shp ? shp[0] : 'b';
    const float px = std::getenv("HAND_DRAG_PX") ? static_cast<float>(std::atof(std::getenv("HAND_DRAG_PX"))) : 6.0f;
    const int steps = std::getenv("HAND_DRAG_STEPS") ? std::atoi(std::getenv("HAND_DRAG_STEPS")) : 160;
    const int frame_ms = std::getenv("HAND_DRAG_FRAME_MS") ? std::atoi(std::getenv("HAND_DRAG_FRAME_MS")) : 16;
    for (int k = 0; k < steps; ++k) {
        const int d = k % 120 < 60 ? k % 120 : 120 - k % 120;
        const float dx = shape == 'h' ? 0.0f : static_cast<float>(d) * px;
        const float dy = shape == 'w' ? 0.0f : static_cast<float>(d) * px * 0.5f;
        rt.window().resize(mayag::Vec2{std::max(s0.x - dx, 300.0f), std::max(s0.y - dy, 200.0f)});
        rt.tick();
        if (frame_ms > 0) idle(frame_ms);   // one frame of a real drag
    }
    rt.window().resize(s0);
    rt.tick();
    idle(1500);
    const int after = count();
    const int lost = before - after, doubled = dups - dups0;
    std::printf("drag: %s, %d lines; after the drag %d (lost %d), doubled %d; history %d rows, %dx%d\n", sh.c_str(), before, after, lost,
                doubled, term().history(), term().cols(), term().rows());
    if (const char* dump = std::getenv("HAND_DRAG_DUMP"); dump && *dump) {
        if (FILE* f = std::fopen(dump, "wb")) {
            for (std::int64_t a = 0; a < term().total_rows(); ++a) {
                std::string row;
                for (char32_t c : term().line(a)) row += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : ' ';
                while (!row.empty() && row.back() == ' ') row.pop_back();
                std::fprintf(f, "%5lld%c%s\n", static_cast<long long>(a), term().wrapped(a) ? '+' : '|', row.c_str());
            }
            std::fclose(f);
        }
    }
    return (before == N && lost == 0 && doubled == 0) ? 0 : 1;
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
        ++m.toast.serial; }}});
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
if (shot == "editor" || shot == "editor-find" || shot == "editor-deep" || shot == "editor-complete" || shot == "editor-symbols" ||
    shot == "editor-changed" || shot == "quick-open" || shot == "search-all") {   // a file from the pane, open in the editor
    std::error_code ec;
    const std::string here = std::filesystem::current_path(ec).generic_string();
    const std::string path = here + (shot == "editor-deep" ? "/external/mayag/include/mayag/widget/text_editor.hpp" : "/src/app/editor.cpp");
    std::ifstream f(path, std::ios::binary);
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    rt.send(hand::App::Msg{hand::App::Output{"\x1b]7;file://localhost" + here + "/src/app\a"}});
    rt.press_key(mayag::Key::e, mayag::Mods{.shift = true, .ctrl = true});
    for (int i = 0; i < 40; ++i) { rt.settle([](const auto&) { return false; }, std::chrono::milliseconds(10)); rt.tick(); }
    rt.send(hand::App::Msg{hand::App::OpenFile{path}});
    rt.send(hand::App::Msg{hand::App::FileLoaded{rt.model().tab().sid, path, std::move(text), "", false}});
    for (int i = 0; i < 30; ++i) rt.press_key(mayag::Key::down, {});   // into the code, the caret in an update()
    if (shot == "editor-deep") for (int i = 0; i < 230; ++i) rt.press_key(mayag::Key::down, {});   // deep inside text_editor_ops::key
    rt.press_key(mayag::Key::end, {});
    rt.type("  // edited");
    if (shot == "editor-find") {
        rt.press_key(mayag::Key::f, mayag::Mods::cmd());
        rt.type("Model");
    }
    if (shot == "editor-complete") {   // completion at the caret
        rt.press_key(mayag::Key::enter, {});
        rt.type("    up");
    }
    if (shot == "editor-symbols") rt.press_key(mayag::Key::o, mayag::Mods::cmd(true));
    if (shot == "editor-changed") rt.send(hand::App::Msg{hand::App::FileStat{rt.model().tab().sid, 1}});
    if (shot == "search-all") {   // the real search over this repo, fed back as its answer
        rt.press_key(mayag::Key::f, mayag::Mods::cmd(true));
        rt.type("cmd::task");
        hand::App::SearchAllDone done{.token = rt.model().search_all.token};
        {   // the same command fx::search_project runs
            const auto g = hand::proc::run({"git", "-C", here, "grep", "-n", "-I", "--no-color", "-F", "-i", "-e", "cmd::task", "--"}, {}, 4000);
            done.root = here;
            std::size_t a = 0;
            for (std::size_t i = 0; i <= g.out.size(); ++i) {
                if (i < g.out.size() && g.out[i] != '\n') continue;
                const std::string l = g.out.substr(a, i - a); a = i + 1;
                const auto c1 = l.find(':'), c2 = c1 == std::string::npos ? c1 : l.find(':', c1 + 1);
                if (c2 == std::string::npos) continue;
                hand::SearchHit h{.file = l.substr(0, c1), .line = std::atoi(l.substr(c1 + 1, c2 - c1 - 1).c_str()) - 1, .text = l.substr(c2 + 1)};
                std::string low = h.text; for (char& ch : low) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                const auto at = low.find("cmd::task");
                h.col = at == std::string::npos ? 0 : static_cast<int>(at); h.len = 9;
                done.hits.push_back(std::move(h));
            }
        }
        rt.send(hand::App::Msg{std::move(done)});
        rt.press_key(mayag::Key::down, {});
        rt.press_key(mayag::Key::down, {});
    }
    if (shot == "quick-open") {
        rt.press_key(mayag::Key::p, mayag::Mods::cmd());
        { // the demo never lists (no task): list this repo the way fx::list_project does
            const auto g = hand::proc::run({"git", "-C", here, "ls-files"}, {}, 4000);
            std::vector<std::string> files; std::string l;
            for (char ch : g.out) { if (ch == '\n') { if (!l.empty()) files.push_back(l); l.clear(); } else l += ch; }
            rt.send(hand::App::Msg{hand::App::QuickListed{here, std::move(files), false}});
        }
        rt.type("edit");
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
if (shot == "hosts" || shot == "ssh" || shot == "ssh-drop" || shot == "ssh-retry" || shot == "ssh-edit" || shot == "ssh-files" || shot.rfind("manager", 0) == 0) {
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
        if (shot == "ssh-drop" || shot == "ssh-retry")
            rt.send(hand::App::Msg{E7{[shot](hand::App::Model& m) {
                m.tab().link.connected_at = m.time - 120.0;   // it was up for a while
                (void)hand::App::update(m, hand::App::Exited{255, m.tab().sid});
                m.tab().link.dropped_at = m.time - 10.0;
                if (shot == "ssh-retry") { m.tab().link.attempts = 2; m.tab().link.retry_at = m.time + 3.6; }   // the third try, 4 s away
                else m.tab().link.retry_at = -1.0;   // waiting for you
            }}});
        if (shot == "ssh-files") {   // the file pane on prod-db: a folder listed over the shared connection
            rt.send(hand::App::Msg{E7{[](hand::App::Model& m) {
                using FE = mayag::dsl::FileEntry;
                const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
                m.pane.open = true; m.pane.follow = false;
                m.pane.where = "prod-db"; m.pane.host = *m.ssh.book.find("prod-db");
                m.pane.path = "/etc/nginx"; m.pane.loading = false;
                m.pane.entries = {FE{.name = "conf.d", .dir = true, .mtime = now - 86400 * 9},
                                  FE{.name = "sites-available", .dir = true, .mtime = now - 3600 * 5},
                                  FE{.name = "sites-enabled", .dir = true, .mtime = now - 3600 * 5},
                                  FE{.name = "snippets", .dir = true, .mtime = now - 86400 * 40},
                                  FE{.name = "fastcgi_params", .size = 1077, .mtime = now - 86400 * 200},
                                  FE{.name = "mime.types", .size = 5349, .mtime = now - 86400 * 200},
                                  FE{.name = "nginx.conf", .size = 1482, .mtime = now - 60 * 7},
                                  FE{.name = "proxy_params", .size = 222, .mtime = now - 86400 * 200}};
                m.pane.preview = mayag::dsl::FilePreview{"nginx.conf", {"user www-data;", "worker_processes auto;", "pid /run/nginx.pid;",
                    "include /etc/nginx/modules-enabled/*.conf;", "", "events {", "    worker_connections 768;", "}"}, "1.4 KB"};
                m.pane.preview_path = "/etc/nginx/nginx.conf";
                m.pane.ui.sel = 6;   // nginx.conf (folders first)
                hand::App::fit(m);
            }}});
            for (int i = 0; i < 8; ++i) rt.tick();
        }
        if (shot == "ssh-edit") {   // a file on the remote host, open in the editor
            rt.send(hand::App::Msg{hand::App::OpenFile{.path = "/etc/nginx/sites-available/app.conf", .host = rt.model().tab().remote}});
            rt.send(hand::App::Msg{hand::App::FileLoaded{rt.model().tab().sid, "/etc/nginx/sites-available/app.conf",
                "server {\n    listen 443 ssl http2;\n    server_name app.example.com;\n\n    location / {\n"
                "        proxy_pass http://127.0.0.1:8080;\n        proxy_set_header Host $host;\n    }\n}\n", "", false, 1}});
            rt.press_key(mayag::Key::down, {});
            rt.press_key(mayag::Key::end, {});
            rt.type(" # edited");
        }
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
    rt.send(hand::App::Msg{mayag::Edit<hand::App::Model>{[](hand::App::Model& m) { m.hud_up = true; }}});
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
