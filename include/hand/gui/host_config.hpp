// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand/gui/host_config.hpp — the small set of HOST-side (hand-only) runtime
// knobs that don't live in toe::Config because they govern hand's own GUI loop
// behaviour, not the terminal engine: drag-selection autoscroll speed, the
// font-zoom step, context-aware pointer shapes, and the command flyout.
//
// These are process-wide (they apply to every tab), so rather than thread them
// through run_tabbed's signature and every backend call site, main() fills this
// single struct after loading the vibe config and the loop reads it. One place,
// no plumbing churn.

#ifndef HAND_GUI_HOST_CONFIG_HPP
#define HAND_GUI_HOST_CONFIG_HPP

#include <cstdint>
#include <string>

namespace hand {

struct HostConfig {
    // Drag-selection autoscroll velocity ramp (rows/sec), min just past the
    // edge -> max when dragged far.
    float autoscroll_min = 3.0f;
    float autoscroll_max = 45.0f;
    // Pixels added/removed per Ctrl+= / Ctrl+- notch.
    int font_zoom_step = 2;
    // Context-aware mouse pointer (I-beam over text, hand over links/rail).
    bool pointer_shapes = true;
    // Command-minimap hover flyout (the command list + click-to-jump).
    bool flyout = true;
    int flyout_rows = 7;    // max command rows shown at once (auto-scrolls on hover)
    int flyout_width = 44;  // max card width in cells
    std::uint32_t flyout_accent = 0x7aa8ff; // 0xRRGGBB
    std::uint32_t flyout_bg = 0x161821;     // card background
    std::uint32_t flyout_border = 0x3a405a; // card frame
    // Tab-bar placement + chrome toggles (GUI layout — host-only).
    std::string tab_position = "top"; // top | bottom | left | right
    int tab_side_width = 18;           // column width (cells) for left/right
    bool tab_controls = true;          // window min/max/close buttons
    bool tab_plus = true;              // the + new-tab button
};

// The single process-wide instance, filled by main() from the vibe config.
inline HostConfig &host_config() {
    static HostConfig cfg;
    return cfg;
}

} // namespace hand

#endif // HAND_GUI_HOST_CONFIG_HPP
