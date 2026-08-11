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
    int flyout_rows = 12;   // max command rows shown at once
    int flyout_width = 44;  // max card width in cells
    std::uint32_t flyout_accent = 0x7aa8ff; // 0xRRGGBB
};

// The single process-wide instance, filled by main() from the vibe config.
inline HostConfig &host_config() {
    static HostConfig cfg;
    return cfg;
}

} // namespace hand

#endif // HAND_GUI_HOST_CONFIG_HPP
