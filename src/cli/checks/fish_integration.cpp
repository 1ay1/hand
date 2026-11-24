// hand::cli::checks — the injected shell integration in a real fish.

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

void fish_integration([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── the injected script, in a REAL fish (bounded) ───────────────────
    if (const std::string fish_path = plat::find_program("/usr/bin/fish"); !fish_path.empty()) {
        const auto si = hand::shell_integration("/usr/bin/fish");
        hand::Term t{80, 10, hand::palette_of(hand::Config{})};
        t.set_prompt_protect(!si.reflow_aware);
        const hand::Pty pty{{.cols = 80, .rows = 10, .shell = "/usr/bin/fish", .env = si.env, .args = si.args}};
        const bool up = pty.ok();
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

}  // namespace hand::cli::checks
