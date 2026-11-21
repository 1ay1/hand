// hand::cli::checks — a real child on a real pty, through toe.

#include "suites.hpp"

#include <hand/core/platform.hpp>   // self_exe (the Windows branch)

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

void pty([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
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

}  // namespace hand::cli::checks
