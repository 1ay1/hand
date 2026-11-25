// hand::cli::checks — the scrollback survives a resize, with the system
// shell on a real pty (ConPTY on Windows, which repaints the screen itself on
// every resize). Every OS, no fish needed.

#include "suites.hpp"

#include <hand/core/pty.hpp>
#include <hand/core/shell.hpp>
#include <hand/core/term.hpp>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace hand::cli::checks {

using namespace mayag;

void resize_history([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    const int N = std::getenv("HAND_RESIZE_N") ? std::atoi(std::getenv("HAND_RESIZE_N")) : 300;
    const int W0 = std::getenv("HAND_RESIZE_W") ? std::atoi(std::getenv("HAND_RESIZE_W")) : 100;
    const int H0 = std::getenv("HAND_RESIZE_H") ? std::atoi(std::getenv("HAND_RESIZE_H")) : 30;
    hand::Term t{W0, H0, hand::palette_of(hand::Config{})};
    // the system shell: cmd / PowerShell / sh print N numbered lines, each "L<n>."
    const std::string shell = Pty::resolve_shell("");
    const bool is_cmd = shell.find("cmd") != std::string::npos;
    const bool is_ps = shell.find("pwsh") != std::string::npos || shell.find("powershell") != std::string::npos;
    // as the app runs it: hand's shell integration (OSC 133 prompts; prompt
    // protection on resize when the shell is not reflow-aware)
    const auto si = hand::shell_integration(shell);
    t.set_prompt_protect(!si.reflow_aware);
    PtySpec spec{.cols = W0, .rows = H0, .shell = shell, .env = si.env, .args = si.args};
    const hand::Pty pty{spec};
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
    // HAND_RESIZE_LONG=1: lines longer than the narrowest width (they wrap when narrowed)
    const bool longl = std::getenv("HAND_RESIZE_LONG") != nullptr;
    const std::string pad(longl ? 90 : 0, 'x');
    const std::string n1 = std::to_string(N - 1);
    // mixed lengths: every 7th line is long enough to wrap at narrow widths
    pty.write(is_cmd ? "for /L %i in (0,1," + n1 + ") do @echo L%i." + pad + "\r"
              : is_ps ? "0.." + n1 + " | % { \"L$_." + pad + "\" + $(if ($_ % 7 -eq 0) { 'w' * 150 } else { '' }) }\r"
                      : "i=0; while [ $i -lt " + std::to_string(N) + " ]; do echo L$i." + pad + "; i=$((i+1)); done\r");
    // distinct lines seen, and how many extra copies (a line shown twice)
    int dups = 0, prompts = 0;
    const auto count = [&] {
        std::vector<int> seen;
        std::string all;
        for (std::int64_t a = 0; a < t.total_rows(); ++a) {
            for (char32_t c : t.line(a)) all += (c >= 0x20 && c < 0x7f) ? static_cast<char>(c) : ' ';
            if (!t.wrapped(a)) all += '\n';   // rejoin soft-wrapped rows: a split "L12" must still count once
        }
        prompts = 0;
        for (std::size_t i = 0; (i = all.find("PS ", i)) != std::string::npos; ++i) prompts += (i == 0 || all[i - 1] == '\n') ? 1 : 0;
        for (std::size_t i = 0; (i = all.find("$ ", i)) != std::string::npos; ++i) prompts += (i == 0 || all[i - 1] == '\n') ? 1 : 0;
        for (std::size_t i = 0; (i = all.find('L', i)) != std::string::npos; ++i) {
            std::size_t j = i + 1;
            int v = 0;
            bool d = false;
            while (j < all.size() && all[j] >= '0' && all[j] <= '9') { v = v * 10 + (all[j] - '0'); ++j; d = true; }
            if (d && j < all.size() && all[j] == '.') seen.push_back(v);
        }
        int distinct = 0;
        std::vector<int> times(N, 0);
        for (int v : seen) if (v >= 0 && v < N && times[static_cast<std::size_t>(v)]++ == 0) ++distinct;
        dups = 0;
        for (int c : times) dups += c > 1 ? c - 1 : 0;
        return distinct;
    };
    for (int waited = 0; waited < 90000 && count() < N; waited += 250) pump(250);   // PowerShell starts slowly
    pump(500);
    const int before = count();
    const int dups_before = dups, prompts_before = prompts;
    logging = true;
    // HAND_RESIZE_PACE_MS: time between drag steps (a real drag: ~16)
    const int pace = std::getenv("HAND_RESIZE_PACE_MS") ? std::atoi(std::getenv("HAND_RESIZE_PACE_MS")) : 16;
    const char* shape = std::getenv("HAND_RESIZE_SHAPE");
    const char sh = shape ? shape[0] : 'm';
    const int steps = std::getenv("HAND_RESIZE_STEPS") ? std::atoi(std::getenv("HAND_RESIZE_STEPS")) : 24;
    for (int k = 0; k < steps; ++k) {
        // m = jumps narrower/wider/shorter/taller, h = height only, w = width only,
        // f = a real drag: one column / row a step, in and out
        int c = sh == 'h' ? W0 : 70 + (k * 23) % 60, r = sh == 'w' ? H0 : 20 + (k * 7) % 15;
        if (sh == 'f') { const int d = k % 120 < 60 ? k % 120 : 120 - k % 120; c = W0 - d * (W0 - 40) / 60; r = H0 - d * (H0 - 12) / 60; }
        t.resize(c, r);
        pty.resize(c, r);
        pump(pace);
    }
    t.resize(W0, H0);
    pty.resize(W0, H0);
    pump(1000);
    const int after = count();
    if (const char* p = std::getenv("HAND_RESIZE_LOG"); p && *p) {
        if (FILE* f = std::fopen(p, "wb")) { std::fwrite(log.data(), 1, log.size(), f); std::fclose(f); }
    }
    if (after < before || before != N || dups > dups_before || prompts != prompts_before)
        std::printf("    resize_history: %d of %d lines before the drag, %d after, %d duplicated, prompts %d -> %d (pace %d ms, history %d rows)\n", before, N, after,
                    dups - dups_before, prompts_before, prompts, pace, t.history());
    ok(pty.ok() && before == N, "the system shell's lines all reach the scrollback");
    ok(after == before, "a window drag keeps every line of the scrollback (system shell on a real pty)");
    ok(dups == dups_before && prompts == prompts_before, "...and shows none of them (nor the prompt) twice");
    pty.write("exit\r");
    pump(200);
}

}  // namespace hand::cli::checks
