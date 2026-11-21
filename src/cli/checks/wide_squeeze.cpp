// hand::cli::checks — wide output squeezed thin and back: no gap above the prompt.

#include "suites.hpp"

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

}  // namespace hand::cli::checks
