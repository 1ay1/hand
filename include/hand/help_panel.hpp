// SPDX-License-Identifier: LGPL-2.0-or-later
//
// HelpPanel — a read-only, in-terminal cheatsheet of hand's keybindings, built
// on the same glyph immediate-mode toolkit as the settings panel and composited
// the same way (Session::render_overlay). Opened with Ctrl+Shift+? and closed
// with Escape or the same chord. Unlike the settings panel it edits nothing:
// it just lists the shortcuts a user needs to drive hand's TUI-first features
// (block navigation, settings, scroll, copy/paste).

#ifndef HAND_HELP_PANEL_HPP
#define HAND_HELP_PANEL_HPP

#include "hand/glyph/glyph.hpp"
#include "toe/app.hpp" // toe::win::Event

namespace hand {

class HelpPanel {
public:
    HelpPanel() = default;

    [[nodiscard]] bool active() const noexcept { return active_; }
    void open() { active_ = true; }
    void close() { active_ = false; }
    void toggle() { active_ = !active_; }

    // Feed one window event; returns true if consumed (must not reach the
    // child). Escape closes the pane; any other key is swallowed while open so
    // a stray keystroke doesn't leak to the shell behind it.
    [[nodiscard]] bool handle(const toe::win::Event &ev) {
        if (!active_) return false;
        using namespace toe::win;
        if (const auto *kp = std::get_if<KeyPressed>(&ev)) {
            if (kp->key.kind != toe::KeyEvent::Kind::press) return true; // swallow release
            if (const auto *sk = std::get_if<toe::SpecialKey>(&kp->key.key)) {
                if (*sk == toe::SpecialKey::Escape) { close(); return true; }
            }
            return true; // swallow everything else while the pane is up
        }
        if (std::get_if<TextEntered>(&ev)) return true; // swallow typed text too
        return false;
    }

    // Paint the cheatsheet into `buf` (sized to the terminal grid in cells).
    void render(glyph::Buffer &buf);

private:
    bool active_ = false;
};

} // namespace hand

#endif // HAND_HELP_PANEL_HPP
