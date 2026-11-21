// hand::cli::checks — a real space at a wrap point survives a rewrap.

#include "suites.hpp"

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

void wrap_spaces([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── toe: a real space at a wrap point survives a rewrap ─────────────
    hand::Term t{10, 5, hand::palette_of(hand::Config{})};
    (void)t.feed("12345 789 abcdefg");   // wraps right after a space
    t.resize(40, 5);
    std::string row;
    const auto snap = t.snapshot();
    for (const auto& c : snap.lines[0].cells) row += c.text;
    while (!row.empty() && row.back() == ' ') row.pop_back();
    ok(row == "12345 789 abcdefg", "spaces at wrap points are text, not padding");
}

}  // namespace hand::cli::checks
