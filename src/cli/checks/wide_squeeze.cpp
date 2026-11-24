// hand::cli::checks — wide output squeezed thin and back: no gap above the prompt.

#include "suites.hpp"
#include <hand/core/platform.hpp>

#include <hand/core/shell.hpp>

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

void wide_squeeze([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── wide output, squeezed thin and back, in a real fish: no gap ────────
    if (const std::string fish_path = plat::find_program("/usr/bin/fish"); !fish_path.empty()) {
        const auto si = hand::shell_integration("/usr/bin/fish");
        hand::Term t{100, 30, hand::palette_of(hand::Config{})};
        t.set_prompt_protect(!si.reflow_aware);
        const hand::Pty pty{{.cols = 100, .rows = 30, .shell = "/usr/bin/fish", .env = si.env, .args = si.args}};
        if (pty.ok()) {
            const auto pump = [&](int ms) {
                std::string buf;
                for (int k = 0; k < ms / 50 && hand::pty_read(pty.fd(), buf, 50); ++k) {
                    const auto fx = t.feed(buf); buf.clear();
                    if (!fx.reply.empty()) pty.write(fx.reply);
                }
            };
            // the 14 output lines above the cursor, and the rows between the last and the prompt
            struct Shape { int lines = 0, gap = 99; };
            const auto shape = [&] {
                const auto sg = t.snapshot();
                Shape sh;
                int last = -1;
                for (int r = 0; r < t.cursor().row; ++r) {
                    std::string l;
                    for (const auto& c : sg.lines[static_cast<std::size_t>(r)].cells) l += c.text;
                    if (l.find("line ") != std::string::npos) { ++sh.lines; last = r; }
                }
                sh.gap = t.cursor().row - last - 1;
                return sh;
            };
            // wait for what we expect, not for a fixed time: fish redraws when it
            // gets to it, and a loaded machine must not turn that into a failure
            const auto pump_until = [&](auto&& done, int max_ms) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(max_ms);
                while (!done() && std::chrono::steady_clock::now() < deadline) pump(100);
            };
            const auto settled = [&] { const Shape sh = shape(); return sh.lines == 14 && sh.gap <= 2; };
            pump_until([&] { return !t.commands().empty(); }, 4000);   // fish is up: its first prompt is marked
            pty.write("clear; for i in (seq 14); printf '      line %02d %s\\n' $i (string repeat -n 70 x); end\r");
            pump_until(settled, 4000);
            for (int k = 0; k < 3; ++k) {
                for (int c = 100; c >= 28; c -= 3) { t.resize(c, 30); pty.resize(c, 30); }
                pump(50);
                for (int c = 28; c <= 100; c += 3) { t.resize(c, 30); pty.resize(c, 30); }
                pump(50);
            }
            pump_until(settled, 4000);   // a gap that never closes is the bug; one fish has not redrawn yet is not
            const Shape sh = shape();
            // the prompt may be two lines (info + ❯): at most one row between
            std::printf("    thin+wide: %d output lines, %d rows to the prompt\n", sh.lines, sh.gap);
            ok(sh.lines == 14 && sh.gap <= 2, "squeezed thin and back: no gap opens above the prompt");
            pty.write("exit\r");
            pump(200);
        }
    }
}

}  // namespace hand::cli::checks
