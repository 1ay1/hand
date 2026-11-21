// hand::cli — resize repros: a real shell on a real pty, dragged through
// sizes, printing what toe holds after each step. Each exits non-zero when
// the prompt or the history comes out corrupted.
//
//   hand --repro-narrow-fish   fish, squeezed narrow and back
//   hand --repro-narrow        the same with a canned prompt (no shell needed)
//   hand --repro-resize [fish] a 40-step drag burst

#include "cli.hpp"

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

namespace hand::cli {

using namespace mayag;

std::optional<int> repro(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view a = argv[i];
    if (a == "--repro-narrow-fish") {
        hand::Pty pty;
        const auto si = hand::shell_integration("/usr/bin/fish");
        hand::Term t{100, 30, hand::palette_of(hand::Config{})};
        t.set_prompt_protect(!si.reflow_aware);
        if (!pty.spawn(100, 30, "/usr/bin/fish", si.env, si.args)) return 1;
        const auto pump = [&](int ms) {
            std::string buf;
            for (int k = 0; k < ms / 50 && hand::pty_read(pty.fd(), buf, 50); ++k) {
                const auto fx = t.feed(buf); buf.clear();
                if (!fx.reply.empty()) pty.write(fx.reply);
            }
        };
        const auto dump = [&](const char* w) {
            std::printf("\n=== %s (%dx%d cursor row %d)\n", w, t.cols(), t.rows(), t.cursor().row);
            const auto g = t.snapshot();
            for (int r = 0; r < g.rows; ++r) {
                std::string l;
                for (const auto& c : g.lines[static_cast<std::size_t>(r)].cells) l += c.text;
                while (!l.empty() && l.back() == ' ') l.pop_back();
                const char mk = g.lines[static_cast<std::size_t>(r)].mark == mayag::dsl::TermMark::none ? ' ' : '#';
                std::printf("%c%2d|%s\n", mk, r, l.substr(0, 70).c_str());
            }
        };
        pump(1500);
        pty.write(std::getenv("HAND_CMD") ? std::getenv("HAND_CMD") : "clear; fastfetch\r");
        pump(2500);
        dump("start");
        const auto tail = [&](const char* w) {
            const auto g = t.snapshot();
            std::printf("--- %s %dx%d cur=%d blocks:", w, t.cols(), t.rows(), t.cursor().row);
            for (const auto& c : t.commands()) std::printf(" [p%lld o%lld e%lld]", static_cast<long long>(c.prompt_row - t.view_to_abs(0)),
                                                          static_cast<long long>(c.output_row - t.view_to_abs(0)), static_cast<long long>(c.end_row - t.view_to_abs(0)));
            std::printf("\n");
            for (int r = 12; r < g.rows; ++r) {
                std::string l;
                for (const auto& c : g.lines[static_cast<std::size_t>(r)].cells) l += c.text;
                while (!l.empty() && l.back() == ' ') l.pop_back();
                std::printf("   %2d|%s\n", r, l.substr(0, 60).c_str());
            }
        };
        if (std::getenv("HAND_TRACE")) {
            for (int c = 95; c >= 30; c -= 5) {
                t.resize(c, 30);
                char w[32]; std::snprintf(w, sizeof w, "resized %d", c); tail(w);
                pty.resize(c, 30); pump(300);
                std::snprintf(w, sizeof w, "fish redrew %d", c); tail(w);
            }
            for (int c = 35; c <= 100; c += 5) {
                t.resize(c, 30);
                char w[32]; std::snprintf(w, sizeof w, "resized %d", c); tail(w);
                pty.resize(c, 30); pump(300);
                std::snprintf(w, sizeof w, "fish redrew %d", c); tail(w);
            }
            return 0;
        }
        for (int c = 95; c >= 30; c -= 5) { t.resize(c, 30); pty.resize(c, 30); pump(60); }
        pump(500);
        dump("narrowed to 30");
        for (int c = 35; c <= 100; c += 5) { t.resize(c, 30); pty.resize(c, 30); pump(60); }
        pump(500);
        dump("wide again");
        // a real drag: no pauses at all, then one settle
        for (int k = 0; k < 3; ++k) {
            for (int c = 100; c >= 28; c -= 3) { t.resize(c, 30); pty.resize(c, 30); }
            pump(50);
            for (int c = 28; c <= 100; c += 3) { t.resize(c, 30); pty.resize(c, 30); }
            pump(50);
        }
        t.resize(100, 30); pty.resize(100, 30);
        pump(800);
        dump("after 3 fast drags");
        // verdict: the prompt sits right under the content
        const auto g2 = t.snapshot();
        int last_content = -1, prompt_row = t.cursor().row;
        for (int r = 0; r < prompt_row - 1; ++r) {
            std::string l;
            for (const auto& c : g2.lines[static_cast<std::size_t>(r)].cells) l += c.text;
            if (l.find_first_not_of(' ') != std::string::npos) last_content = r;
        }
        const int gap = prompt_row - 1 - last_content - 1;
        std::printf("\nverdict: %d blank row(s) between the output and the prompt\n", gap);
        pty.write("exit\r"); pump(200);
        return gap <= 1 ? 0 : 1;
    }
    if (a == "--repro-narrow") {
        // wide output, then narrow hard, then wide again (no shell: toe alone)
        hand::Term t{100, 30, hand::palette_of(hand::Config{})};
        std::string text = "\x1b]133;A\a$ \x1b]133;B\afastfetch\r\n\x1b]133;C\a";
        for (int k = 0; k < 14; ++k) text += "      line " + std::to_string(k) + " " + std::string(60, static_cast<char>('a' + k)) + "\r\n";
        text += "\x1b]133;D;0\a\x1b]133;A\a~/hand on main\r\n\x1b]133;B\a> ";
        (void)t.feed(text);
        const auto dump = [&](const char* w) {
            std::printf("\n=== %s (%dx%d cursor row %d)\n", w, t.cols(), t.rows(), t.cursor().row);
            const auto g = t.snapshot();
            for (int r = 0; r < g.rows; ++r) {
                std::string l;
                for (const auto& c : g.lines[static_cast<std::size_t>(r)].cells) l += c.text;
                while (!l.empty() && l.back() == ' ') l.pop_back();
                std::printf("%2d|%s\n", r, l.substr(0, 60).c_str());
            }
        };
        dump("start");
        t.resize(30, 30); dump("narrow 30");
        t.resize(100, 30); dump("wide again");
        return 0;
    }
    if (a == "--repro-resize") {
        // A real shell (fish by default: your config + hand's integration)
        // through a drag of resizes. Prints the screen after each step.
        // Bounded: every read is ≤ 50 ms. `--repro-resize /bin/zsh` for another.
        const std::string shell = [&]() -> std::string {
            if (i + 1 < argc && argv[i + 1][0] != '-') return argv[i + 1];
            if (const char* path = std::getenv("PATH")) {   // fish wherever it lives (/opt/homebrew/bin on macOS)
                std::string_view p = path;
                while (!p.empty()) {
                    const auto c = p.find(':');
                    const std::string cand = std::string{p.substr(0, c)} + "/fish";
                    if (::access(cand.c_str(), X_OK) == 0) return cand;
                    if (c == std::string_view::npos) break;
                    p.remove_prefix(c + 1);
                }
            }
            return hand::Pty::resolve_shell({});
        }();
        std::printf("shell: %s\n", shell.c_str());
        hand::Pty pty;
        const auto si = hand::shell_integration(shell);
        int cols = 80, rows = 24;
        hand::Term t{cols, rows, hand::palette_of(hand::Config{})};
        t.set_prompt_protect(!si.reflow_aware);
        if (!pty.spawn(cols, rows, shell, si.env, si.args)) return 1;
        const bool show_bytes = false;
        const auto pump = [&](int ms) {
            std::string buf;
            for (int k = 0; k < ms / 50 && hand::pty_read(pty.fd(), buf, 50); ++k) {
                if (show_bytes && !buf.empty()) {
                    std::string e;
                    for (unsigned char ch : buf) {
                        if (ch == 0x1b) e += "\\e"; else if (ch == '\r') e += "\\r"; else if (ch == '\n') e += "\\n\n      ";
                        else if (ch == 7) e += "\\a"; else if (ch < 0x20) { char b[8]; std::snprintf(b, 8, "^%c", ch + 64); e += b; }
                        else e += static_cast<char>(ch);
                    }
                    std::printf("  bytes: %s\n", e.c_str());
                }
                const auto fx = t.feed(buf);
                buf.clear();
                if (!fx.reply.empty()) pty.write(fx.reply);
            }
        };
        const auto dump = [&](const char* what) {
            std::printf("\n=== %s  (%dx%d, cursor %d,%d)\n", what, t.cols(), t.rows(), t.cursor().col, t.cursor().row);
            const auto g = t.snapshot();
            for (int r = 0; r < g.rows; ++r) {
                std::string line;
                for (const auto& c : g.lines[static_cast<std::size_t>(r)].cells) line += c.text.empty() ? "" : c.text;
                while (!line.empty() && line.back() == ' ') line.pop_back();
                const char mk = g.lines[static_cast<std::size_t>(r)].mark == mayag::dsl::TermMark::none ? ' ' : '#';
                std::printf("%c%2d|%s\n", mk, r, line.c_str());
            }
        };
        const auto resize = [&](int c, int r) {
            t.resize(c, r);
            pty.resize(c, r);
            pump(400);
        };
        pump(1500);
        pty.write("seq 1 12\r");
        pump(800);
        dump("start");
        const int steps[][2] = {{80, 18}, {80, 12}, {60, 12}, {50, 10}, {70, 20}, {90, 30}, {80, 24}};
        for (const auto& st : steps) {
            char what[32];
            std::snprintf(what, sizeof what, "-> %dx%d", st[0], st[1]);
            resize(st[0], st[1]);
            if (&st == &steps[3] || &st == &steps[6]) dump(what);
        }
        // a real drag: many sizes in a burst, the shell answering whenever it can
        for (int k = 0; k < 40; ++k) {
            const int c = 30 + (k * 37) % 70, r = 6 + (k * 13) % 26;
            t.resize(c, r);
            pty.resize(c, r);
            if (k % 3 == 0) pump(50);
        }
        resize(80, 24);
        pump(600);
        dump("after a 40-step drag burst (down to 30 cols: the prompt wraps)");
        // The verdict, for any shell: every number of `seq` exactly once and
        // in order; the live prompt (the cursor's row) exactly once -- no
        // stale copies left by a reflow; no hole between the output and
        // the prompt. Rows BELOW the prompt are empty by design.
        const auto g = t.snapshot();
        std::vector<std::string> lines;
        for (const auto& l : g.lines) {
            std::string line;
            for (const auto& c : l.cells) line += c.text;
            while (!line.empty() && line.back() == ' ') line.pop_back();
            lines.push_back(line);
        }
        const int cur = std::clamp(t.cursor().row, 0, static_cast<int>(lines.size()) - 1);
        const std::string prompt = lines[static_cast<std::size_t>(cur)];
        int prompts = 0, blank_run = 0, worst_gap = 0, next = 1;
        bool order_ok = true;
        for (int r = 0; r < static_cast<int>(lines.size()); ++r) {
            const std::string& l = lines[static_cast<std::size_t>(r)];
            if (!prompt.empty() && l == prompt) ++prompts;
            if (r < cur) {
                blank_run = l.empty() ? blank_run + 1 : 0;
                worst_gap = std::max(worst_gap, blank_run);
            }
            if (!l.empty() && l.find_first_not_of("0123456789") == std::string::npos) {
                if (std::stoi(l) == next) ++next;
                else order_ok = false;
            }
        }
        const bool all_numbers = order_ok && next == 13;
        std::printf("\nverdict: prompt \"%s\" on screen %d time(s), seq 1..12 %s, longest blank run above the prompt %d\n",
                    prompt.c_str(), prompts, all_numbers ? "intact" : "BROKEN", worst_gap);
        pty.write("exit\r");
        pump(300);
        return prompts == 1 && all_numbers && worst_gap <= 2 ? 0 : 1;
    }
    }
    return std::nullopt;
}

}  // namespace hand::cli
