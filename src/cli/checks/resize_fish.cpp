// hand::cli::checks — resize never corrupts: real fish, a drag of sizes.

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

void resize_fish([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── resize never corrupts: real fish, a drag of sizes (bounded) ───────
    if (::access("/usr/bin/fish", X_OK) == 0) {
        hand::Pty pty;
        const auto si = hand::shell_integration("/usr/bin/fish");
        hand::Term t{80, 24, hand::palette_of(hand::Config{})};
        t.set_prompt_protect(!si.reflow_aware);
        const bool up = pty.spawn(80, 24, "/usr/bin/fish", si.env, si.args);
        const auto pump = [&](int ms) {
            std::string buf;
            for (int k = 0; k < ms / 50 && hand::pty_read(pty.fd(), buf, 50); ++k) {
                const auto fx = t.feed(buf); buf.clear();
                if (!fx.reply.empty()) pty.write(fx.reply);
            }
        };
        pump(1200);
        pty.write("seq 1 12\r");   // fish redraws its prompt after; then wait for idle
        pump(1500);
        const auto text = [&] {
            std::vector<std::string> v;
            for (const auto& l : t.snapshot().lines) {
                std::string s;
                for (const auto& c : l.cells) s += c.text;
                while (!s.empty() && s.back() == ' ') s.pop_back();
                v.push_back(s);
            }
            return v;
        };
        const auto before = text();
        for (int k = 0; k < 30; ++k) {   // a drag: grow and shrink in both directions, stay readable (>= 50 cols)
            const int c = 50 + (k * 37) % 50, r = 6 + (k * 13) % 26;
            t.resize(c, r);
            pty.resize(c, r);
            if (k % 3 == 0) pump(50);
        }
        t.resize(80, 24);
        pty.resize(80, 24);
        pump(700);
        const auto after = text();
        // what counts: the 12 numbers once each, ONE prompt, and no blank gap
        // between content (empty screen below the prompt is just screen)
        const auto audit = [](const std::vector<std::string>& v, int& nums, int& prompts, int& gap) {
            nums = prompts = gap = 0;
            std::size_t last = 0;
            for (std::size_t i = 0; i < v.size(); ++i) if (!v[i].empty()) last = i;
            int run = 0;
            for (std::size_t i = 0; i <= last && i < v.size(); ++i) {
                const auto& l = v[i];
                bool num = !l.empty();
                for (char ch : l) num = num && ch >= '0' && ch <= '9';
                nums += num ? 1 : 0;
                prompts += l.find("\u276F seq 1 12") != std::string::npos ? 1 : 0;
                run = l.empty() ? run + 1 : 0;
                gap = std::max(gap, run);
            }
        };
        int seq_lines = 0, prompt_cmds = 0, worst = 0;
        audit(after, seq_lines, prompt_cmds, worst);
        if (seq_lines != 12 || prompt_cmds != 1 || worst > 2)
            for (std::size_t i = 0; i < after.size(); ++i) std::printf("    %2zu|%s\n", i, after[i].c_str());
        ok(up && seq_lines == 12 && prompt_cmds == 1, "a 30-step resize drag keeps every line, once");
        ok(worst <= 2, "no blank gaps open up between output and prompt");
        // narrower than the prompt: it wraps, then must come back whole
        for (int c : {28, 22, 36, 80}) { t.resize(c, 24); pty.resize(c, 24); pump(250); }
        const auto narrow = text();
        int nseq = 0, nprompt = 0, ngap = 0;
        audit(narrow, nseq, nprompt, ngap);
        if (nseq != 12 || nprompt != 1 || ngap > 2)
            for (std::size_t i = 0; i < narrow.size(); ++i) std::printf("    %2zu|%s\n", i, narrow[i].c_str());
        ok(nseq == 12 && nprompt == 1 && ngap <= 2, "squeezed narrower than the prompt and back: output intact");
        int same = 0;
        for (std::size_t i = 0; i < std::min(before.size(), after.size()); ++i) same += before[i] == after[i] ? 1 : 0;
        if (same < static_cast<int>(before.size()) - 3)
            for (std::size_t i = 0; i < std::min(before.size(), after.size()); ++i)
                if (before[i] != after[i]) std::printf("    %2zu before|%s\n       after |%s\n", i, before[i].c_str(), after[i].c_str());
        ok(same >= static_cast<int>(before.size()) - 3, "back at the same size, the screen reads the same");
        int marks = 0;
        for (const auto& c : t.commands()) marks += c.ran && c.prompt_row >= 0 ? 1 : 0;
        ok(marks >= 1, "command marks follow the rewrap");
        pty.write("exit\r");
        pump(200);
    }
}

}  // namespace hand::cli::checks
