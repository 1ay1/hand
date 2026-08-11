// SPDX-License-Identifier: LGPL-2.0-or-later
//
// HelpPanel::render — the keybinding cheatsheet, laid out from a DATA TABLE so
// there is one rendering loop (no per-row hand-placement). The panel auto-sizes
// to its content and measures the widest key so the two columns align exactly —
// long chords like "Ctrl+Tab / Ctrl+Shift+Tab" can never overrun the
// description column.

#include "hand/help_panel.hpp"

#include <array>
#include <string_view>

namespace hand {

namespace {

// One cheatsheet entry: a section heading (desc empty) OR a key/description row.
struct Entry {
    std::string_view key;  // empty => this Entry is a section heading (in `desc`)
    std::string_view desc;
    [[nodiscard]] bool is_heading() const noexcept { return key.empty(); }
};

// The whole cheatsheet, in order. Adding a binding is one line here; layout is
// automatic. Kept in sync with the shared classify_chord + the terminal's
// block/scroll/clipboard bindings.
constexpr std::array kEntries = std::to_array<Entry>({
    {{}, "Tabs"},
    {"Ctrl+Shift+T", "New tab (same directory)"},
    {"Ctrl+Shift+W", "Close tab"},
    {"Ctrl+Tab", "Next tab"},
    {"Ctrl+Shift+Tab", "Previous tab"},

    {{}, "Panels"},
    {"Ctrl+Shift+,", "Settings"},
    {"Ctrl+Shift+?", "This help"},
    {"Ctrl+Shift+F", "Search scrollback"},
    {"Esc", "Close a panel"},

    {{}, "Search"},
    {"Enter / Ctrl+G", "Next match"},
    {"Shift+Enter / Ctrl+P", "Previous match"},

    {{}, "Command blocks (OSC 133)"},
    {"Ctrl+Shift+Up", "Jump to previous command"},
    {"Ctrl+Shift+Down", "Jump to next command"},
    {"Ctrl+Shift+E", "Jump to last failed command"},

    {{}, "Scrollback & clipboard"},
    {"Shift+PageUp / PageDown", "Scroll one page"},
    {"Ctrl+Shift+C / V", "Copy selection / paste"},
});

} // namespace

void HelpPanel::render(glyph::Buffer &buf) {
    glyph::Input none{}; // read-only: no interactive focus
    glyph::Ctx ui(buf, none);

    // Measure: the widest KEY sets the description column; the total line count
    // (rows + headings, headings cost 2 lines: a gap+rule) sets the panel height.
    int key_w = 0, content_rows = 0;
    for (const auto &e : kEntries) {
        if (e.is_heading()) {
            content_rows += 3; // blank gap + heading + rule
        } else {
            key_w = std::max<int>(key_w, glyph::Buffer::text_width(e.key));
            content_rows += 1;
        }
    }
    const int kv_col = key_w + 3; // key column + a 2-cell gutter before descs

    // Widest content line = key column + longest description.
    int desc_w = 0;
    for (const auto &e : kEntries)
        if (!e.is_heading()) desc_w = std::max<int>(desc_w, glyph::Buffer::text_width(e.desc));
    const int inner_w = std::max(kv_col + desc_w, 30);
    // cols: content + frame(2) + inset(2) + breathing(2); rows: title band(3) +
    // content + the trailing note block (gap + 2 notes) + footer(3) + margins.
    const int cols = inner_w + 8;
    const int rows = content_rows + 3 /*note block*/ + 8;

    ui.begin_panel("hand · keybindings", cols, rows);

    for (const auto &e : kEntries) {
        if (e.is_heading()) ui.heading(e.desc);
        else ui.kv_row(e.key, e.desc, kv_col);
    }

    ui.gap();
    ui.note("Full-screen apps (vim, tmux, htop) keep every key —");
    ui.note("block / scroll shortcuts apply at the shell prompt.");

    ui.end_panel("esc  close");
}

} // namespace hand
