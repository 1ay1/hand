// SPDX-License-Identifier: LGPL-2.0-or-later
//
// HelpPanel::render — lays out the keybinding cheatsheet.

#include "hand/help_panel.hpp"

namespace hand {

void HelpPanel::render(glyph::Buffer &buf) {
    glyph::Input none{}; // read-only: no interactive focus needed
    glyph::Ctx ui(buf, none);

    ui.begin_panel("hand · keybindings", 60, 28);

    ui.heading("Panels");
    ui.kv_row("Ctrl+Shift+,", "Open settings");
    ui.kv_row("Ctrl+Shift+?", "Open this help");
    ui.kv_row("Esc", "Close a panel");

    ui.heading("Command blocks (OSC 133)");
    ui.kv_row("Ctrl+Shift+Up", "Jump to previous command");
    ui.kv_row("Ctrl+Shift+Down", "Jump to next command");
    ui.kv_row("Ctrl+Shift+E", "Jump to last failed command");

    ui.heading("Scrollback & clipboard");
    ui.kv_row("Shift+PageUp", "Scroll up one page");
    ui.kv_row("Shift+PageDown", "Scroll down one page");
    ui.kv_row("Ctrl+Shift+C", "Copy selection");
    ui.kv_row("Ctrl+Shift+V", "Paste");

    ui.gap();
    ui.note("Full-screen apps (vim, tmux, htop) keep all keys —");
    ui.note("block/scroll shortcuts apply on the shell prompt only.");

    ui.end_panel("esc  close");
}

} // namespace hand
