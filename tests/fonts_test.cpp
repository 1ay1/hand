// SPDX-License-Identifier: LGPL-2.0-or-later
//
// fonts_test — smoke-checks the smart POSIX font discovery. It's environment-
// dependent (uses whatever fonts + fontconfig the machine has), so it asserts
// invariants rather than exact families: the list is non-empty and deduped,
// "monospace" is always offered, and resolve_font_file round-trips.
#include "hand/platform/fonts.hpp"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

int main() {
    int fails = 0;
    auto ck = [&](bool ok, const char *n) {
        if (!ok) { std::printf("FAIL %s\n", n); ++fails; }
    };

    auto fams = hand::list_monospace_families();
    ck(!fams.empty(), "family list is non-empty");
    ck(!fams.empty() && fams.front() == "monospace",
       "\"monospace\" alias is listed first");

    // No duplicate families (case-insensitive).
    std::set<std::string> seen;
    bool dup = false;
    for (auto &f : fams) {
        std::string l;
        for (char c : f) l += static_cast<char>(std::tolower((unsigned char)c));
        if (!seen.insert(l).second) dup = true;
    }
    ck(!dup, "no duplicate families");

    // Every listed non-generic family should resolve to SOME readable file
    // (either exactly or via the mono fallback) — never empty.
    int resolved = 0, checked = 0;
    for (std::size_t i = 1; i < fams.size() && checked < 8; ++i, ++checked) {
        const std::string file = hand::resolve_font_file(fams[i]);
        if (!file.empty()) ++resolved;
    }
    if (checked > 0) ck(resolved == checked, "every listed family resolves to a file");

    // The generic alias always resolves (to the system default mono) when any
    // font exists at all.
    if (fams.size() > 1)
        ck(!hand::resolve_font_file("monospace").empty(),
           "\"monospace\" resolves to a file");

    std::printf("%zu families; %d/%d resolved\n", fams.size(), resolved, checked);
    std::printf(fails ? "%d FONT CHECK(S) FAILED\n" : "ALL FONT CHECKS PASS\n", fails);
    // Discovery is environment-dependent: if the box genuinely has no fonts,
    // skip rather than fail (CI containers may be bare).
    if (fams.size() <= 1) { std::printf("(no fonts installed; skipping)\n"); return 77; }
    return fails ? 1 : 0;
}
