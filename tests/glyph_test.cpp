// SPDX-License-Identifier: LGPL-2.0-or-later
//
// glyph_test — headless checks for the glyph IMGUI toolkit (no GL needed; the
// Ctx paints into a plain cell Buffer). Focus: the redesigned DROPDOWN, whose
// type-to-filter + select flow is the core of "easy to configure".
#include "hand/glyph/glyph.hpp"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

using namespace glyph;

namespace {
int fails = 0;
void ck(bool ok, const char *n) {
    if (!ok) { std::printf("FAIL %s\n", n); ++fails; }
}

// Drive one frame: a fresh Ctx (as the real panel does), a single Input.
// Returns whether the dropdown reported a changed selection.
struct DD {
    int index = 0, open = -1, sel = 0, top = 0;
    std::string filter;
    std::vector<std::string> opts;
    int focus = 0;

    bool frame(const Input &in) {
        Buffer buf(80, 24);
        Ctx ui(buf, in, &focus, Theme{});
        ui.begin_panel("test", 60, 20);
        bool changed = ui.dropdown("Pick", &index, opts, &open, &sel, &top, 8, &filter);
        ui.end_panel();
        return changed;
    }
};

Input key(Key k) { Input in; in.key = k; return in; }
Input ch(char c) { Input in; in.key = Key::Char; in.ch = static_cast<char32_t>(c); return in; }
} // namespace

int main() {
    DD d;
    d.opts = {"Catppuccin Mocha", "Gruvbox Dark", "Nord",
              "Tokyo Night", "Rose Pine", "Dracula", "Everforest", "Solarized"};

    // Nothing happens until focused + activated. Focus row 0 is the dropdown
    // (begin_panel adds no focusable rows before it here).
    d.focus = 0;

    // Open with Enter.
    ck(!d.frame(key(Key::Enter)), "open: no change yet");
    ck(d.open == 0, "dropdown is open");

    // Type "night" -> should filter to Tokyo Night and land selection on it.
    for (char c : std::string("night")) d.frame(ch(c));
    ck(d.filter == "night", "filter text accumulated");

    // Enter selects the single filtered hit.
    bool changed = d.frame(key(Key::Enter));
    ck(changed, "enter committed a change");
    ck(d.open == -1, "dropdown closed after select");
    ck(d.opts[static_cast<std::size_t>(d.index)] == "Tokyo Night", "filtered pick == Tokyo Night");
    ck(d.filter.empty(), "filter cleared on close");

    // Re-open, type a prefix that matches several, arrow down, select.
    d.frame(key(Key::Enter));
    ck(d.open == 0, "re-opened");
    d.frame(ch('r')); // matches "Gruvbox daRk", "noRd", "Rose Pine", "dRacula", "everforest", "solaRized"
    const std::string before = d.opts[static_cast<std::size_t>(d.index)];
    d.frame(key(Key::Down));
    d.frame(key(Key::Enter));
    ck(d.open == -1, "closed again");
    // Whatever it landed on must actually contain 'r' (case-insensitive).
    std::string picked = d.opts[static_cast<std::size_t>(d.index)];
    for (char &c : picked) c = static_cast<char>(std::tolower((unsigned char)c));
    ck(picked.find('r') != std::string::npos, "filtered+arrowed pick matches query");

    // Escape cancels without changing the value.
    d.frame(key(Key::Enter));
    const int keep = d.index;
    d.frame(ch('z')); // filter to nothing meaningful
    ck(!d.frame(key(Key::Escape)), "escape: no change");
    ck(d.open == -1, "escape closed");
    ck(d.index == keep, "escape preserved the value");
    ck(d.filter.empty(), "escape cleared filter");

    // Backspace edits the filter.
    d.frame(key(Key::Enter));
    d.frame(ch('a')); d.frame(ch('b'));
    ck(d.filter == "ab", "two chars typed");
    d.frame(key(Key::Backspace));
    ck(d.filter == "a", "backspace removed one char");

    std::printf(fails ? "%d GLYPH CHECK(S) FAILED\n" : "ALL GLYPH CHECKS PASS\n", fails);
    return fails ? 1 : 0;
}
