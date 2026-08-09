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

    // Type "night" -> filters to Tokyo Night. SELECT-ON-MOVE: the value tracks
    // the sole hit immediately, so typing already committed the change.
    bool moved = false;
    for (char c : std::string("night")) moved |= d.frame(ch(c));
    ck(d.filter == "night", "filter text accumulated");
    ck(moved, "select-on-move committed while filtering");
    ck(d.opts[static_cast<std::size_t>(d.index)] == "Tokyo Night",
       "live value == Tokyo Night while open");

    // Enter confirms + closes (value already applied by select-on-move).
    d.frame(key(Key::Enter));
    ck(d.open == -1, "dropdown closed after enter");
    ck(d.opts[static_cast<std::size_t>(d.index)] == "Tokyo Night", "pick stuck == Tokyo Night");
    ck(d.filter.empty(), "filter cleared on close");

    // Re-open, type a prefix that matches several, arrow down (previews live).
    d.frame(key(Key::Enter));
    ck(d.open == 0, "re-opened");
    d.frame(ch('r')); // matches Gruvbox daRk / noRd / Rose Pine / dRacula / ...
    ck(d.frame(key(Key::Down)), "arrow-down previews (select-on-move)");
    d.frame(key(Key::Enter));
    ck(d.open == -1, "closed again");
    // Whatever it landed on must actually contain 'r' (case-insensitive).
    std::string picked = d.opts[static_cast<std::size_t>(d.index)];
    for (char &c : picked) c = static_cast<char>(std::tolower((unsigned char)c));
    ck(picked.find('r') != std::string::npos, "filtered+arrowed pick matches query");

    // Escape closes the dropdown (keeps the previewed value — no revert, since
    // select-on-move is the whole point) and clears the filter.
    d.frame(key(Key::Enter));
    d.frame(ch('n')); // preview some 'n' theme
    const int previewed = d.index;
    ck(!d.frame(key(Key::Escape)), "escape itself reports no NEW change");
    ck(d.open == -1, "escape closed the dropdown");
    ck(d.index == previewed, "escape keeps the previewed value");
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
