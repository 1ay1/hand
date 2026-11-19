#pragma once
// hand::view — the widget tree, one function per branch. Internal.

#include "../app/detail.hpp"

#include <mayag/mayag.hpp>

namespace hand::view {

/// The tab strip: every session as a TermTabInfo, in the configured style.
[[nodiscard]] mayag::Node tabs(const mayag::Ctx& c, const Model& m, float height);
/// The terminal: the grid, the cursor's motion, and what sits over the cells.
[[nodiscard]] mayag::Node terminal(const mayag::Ctx& c, const Model& m);
/// What floats over the whole window: dialog, toasts, picker, search, HUD,
/// the in-window settings panel. Posted as overlays.
void overlays(const mayag::Ctx& c, const Model& m);
/// The settings panel's props.
[[nodiscard]] mayag::dsl::SettingsPanelProps settings_props(const mayag::Ctx& c, const Model& m);

/// The palette as theme colours.
[[nodiscard]] inline mayag::Color<mayag::Srgb> rgb(std::uint32_t v) { return mayag::dsl::rgb_of(v); }

}  // namespace hand::view
