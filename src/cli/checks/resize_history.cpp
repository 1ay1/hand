// hand::cli::checks — the scrollback survives a resize, with the system
// shell on a real pty (ConPTY on Windows, which repaints the screen itself on
// every resize). Every OS, no fish needed.

#include "suites.hpp"

#include <hand/core/pty.hpp>
#include <hand/core/term.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace hand::cli::checks {

using namespace mayag;

void resize_history([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    constexpr int N = 300;
    hand::Term t{100, 30, hand::palette_of(hand::Config{})};
    // the system shell: cmd / PowerShell / sh print N numbered lines, each "L<n>."
    const std::string shell = Pty::resolve_shell("");
    const bool is_cmd = shell.find("cmd") != std::string::npos;
    const bool is_ps = shell.find("pwsh") != std::string::npos || shell.find("powershell") != std::string::npos;
    const hand::Pty pty{{.cols = 100, .rows = 30, .shell = shell}};
    std::string log;   // every byte after the first resize (HAND_RESIZE_LOG=path to keep it)
    bool logging = false;
    const auto pump = [&](int ms) {
        std::string buf;
        for (int k = 0; k < ms / 25 && hand::pty_read(pty.fd(), buf, 25); ++k) {
            if (logging) log += buf;
            const auto fx = t.feed(buf);
            buf.clear();
            if (!fx.reply.empty()) pty.write(fx.reply);
        }
    };
    pump(1500);
    pty.write(is_cmd ? "for /L %i in (0,1,299) do @echo L%i.\r"
              : is_ps ? "0..299 | % { \"L$_.\" }\r"
                      : "i=0; while [ $i -lt 300 ]; do echo L$i.; i=$((i+1)); done\r");
    const auto count = [&] {
        std::vector<int> seen;
        std::string all;
        for (std::int64_t a = 0; a < t.total_rows(); ++a) {
            for (char32_t c : t.line(a)) all += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : ' ';
            all += '\n';
        }
        for (std::size_t i = 0; (i = all.find('L', i)) != std::string::npos; ++i) {
            std::size_t j = i + 1;
            int v = 0;
            bool d = false;
            while (j < all.size() && all[j] >= '0' && all[j] <= '9') { v = v * 10 + (all[j] - '0'); ++j; d = true; }
            if (d && j < all.size() && all[j] == '.') seen.push_back(v);
        }
        int distinct = 0;
        std::vector<bool> got(N, false);
        for (int v : seen) if (v >= 0 && v < N && !got[static_cast<std::size_t>(v)]) { got[static_cast<std::size_t>(v)] = true; ++distinct; }
        return distinct;
    };
    for (int waited = 0; waited < 20000 && count() < N; waited += 250) pump(250);   // PowerShell starts slowly
    pump(500);
    const int before = count();
    logging = true;
    for (int k = 0; k < 12; ++k) {   // a window drag: narrower, wider, shorter, taller
        const int c = 70 + (k * 23) % 60, r = 20 + (k * 7) % 15;
        t.resize(c, r);
        pty.resize(c, r);
        pump(120);
    }
    t.resize(100, 30);
    pty.resize(100, 30);
    pump(800);
    const int after = count();
    if (const char* p = std::getenv("HAND_RESIZE_LOG"); p && *p) {
        if (FILE* f = std::fopen(p, "wb")) { std::fwrite(log.data(), 1, log.size(), f); std::fclose(f); }
    }
    if (after < before || before != N)
        std::printf("    resize_history: shell %s, %d of %d lines before the drag, %d after (history %d rows, total %lld)\n", shell.c_str(), before, N, after,
                    t.history(), static_cast<long long>(t.total_rows()));
    ok(pty.ok() && before == N, "the system shell's 300 lines all reach the scrollback");
    ok(after == before, "a window drag keeps every line of the scrollback (system shell on a real pty)");
    pty.write("exit\r");
    pump(200);
}

}  // namespace hand::cli::checks
