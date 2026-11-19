#pragma once
// hand::Config — everything configurable, and the one table describing it.
//
// Every setting is a row in src/settings.cpp: section, key, hint, kind,
// range, and a get/set pair over Config. That single table generates the
// settings panel (mayag settings_panel), reads and writes ~/.config/hand/
// hand.vibe, and validates input. Adding a setting is adding a row.

#include <mayag/widget/settings_panel.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace hand {

/// The 16 ANSI colours plus default ink/paper/cursor/selection, as 0xRRGGBB.
struct Palette {
    std::uint32_t fg = 0xABB2BF, bg = 0x282C34, cursor = 0x74ADE8, selection = 0x3E4451;
    std::uint32_t ansi[16] = {0x282C34, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xABB2BF,
                              0x5C6370, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xFFFFFF};
    bool light = false;
};

enum class CursorShape : std::uint8_t { block, beam, underline };

struct Config {
    // font
    int   font_family = 0;          ///< index into font_names()
    float font_size = 13.0f;
    float line_height = 1.2f;
    // colors
    int   scheme = -1;              ///< index into the catalogue; -1 = default (One Dark)
    bool  bold_is_bright = true;
    float dim_unfocused = 0.0f;
    // cursor
    CursorShape cursor = CursorShape::block;
    bool  cursor_blink = true;
    float blink_ms = 500.0f;
    bool  cursor_smooth = true;     ///< glide between cells with a short trail
    bool  cursor_glow = false;       ///< a soft light under the cursor
    // window
    float padding = 8.0f;
    float opacity = 1.0f;
    // scrollback
    float scrollback = 10000.0f;
    float scroll_lines = 3.0f;
    bool  minimap = true;
    bool  smooth_scroll = true;     ///< glide through history, pixel by pixel
    float live_tail = 4.0f;         ///< rows of live output kept at the bottom while scrolled back (0 = off)
    bool  peek = true;              ///< hovering the minimap previews the command and lines there           ///< scrollbar map: prompts, failures, search hits
    // shell
    bool  shell_integration = true; ///< inject OSC 133/7 into fish/zsh/bash
    bool  shell_marks = true;       ///< prompt marks in the gutter
    float notify_after = 10.0f;     ///< seconds; notify when a longer command ends unfocused (0 = off)
    // behaviour
    bool  copy_on_select = false;
    bool  visual_bell = true;
    bool  links_need_ctrl = true;   ///< ctrl+click opens links (plain click selects)

    // not settings: launch options
    std::string shell;          ///< [main] shell (from the file)
    std::string shell_cli;      ///< -e CMD: this run only, never saved
    std::string url_launcher = "xdg-open";   ///< [main] url_launcher=
    bool        demo = false;
};

// ── the table ──────────────────────────────────────────────────────────────
[[nodiscard]] std::size_t setting_count();
[[nodiscard]] std::vector<mayag::dsl::SettingSpec> setting_specs(const Config& c);
/// Apply a panel edit. Returns true when the config changed.
bool setting_apply(Config& c, const mayag::dsl::SettingEdit& e);

// ── derived values ─────────────────────────────────────────────────────────
[[nodiscard]] Palette palette_of(const Config& c);
[[nodiscard]] std::size_t scheme_count();
/// Monospace faces found at startup (main sets them; family index → name).
void set_font_names(std::vector<std::string> names);
[[nodiscard]] const std::vector<std::string>& font_names();

// ── the file ───────────────────────────────────────────────────────────────
[[nodiscard]] std::string config_path();   ///< $HAND_CONFIG or ~/.config/hand/hand.vibe
/// Read the file over `c`. Unknown keys/bad values are skipped; returns a
/// short message for each, empty when clean.
std::string config_load(Config& c, const std::string& path);
bool config_save(const Config& c, const std::string& path);

}  // namespace hand
