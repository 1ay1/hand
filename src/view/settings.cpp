// hand::view — the theme (cut from the scheme) and the settings panel's props.

#include "view.hpp"

#include <mayag/widget/marks.hpp>
#include <mayag/widget/settings_panel.hpp>

#include <cstring>

namespace hand {

using namespace mayag;
using namespace mayag::dsl;

/// The chrome is cut from the scheme itself: surfaces are the paper lifted
/// toward the ink, text is the ink, status roles are the scheme's ANSI colours.
Theme App::theme(const Model& m) {
    const Palette& p = m.palette;
    const auto paper = rgb_of(p.bg), ink = rgb_of(p.fg);
    Theme t = derive(Seed{.accent = rgb_of(p.ansi[4]), .mode = p.light ? Mode::light : Mode::dark,
                          .roundness = std::max(m.cfg.roundness, 0.0f)});
    t.background = paper;
    t.surface = mix(paper, ink, 0.05f);
    t.surface_raised = mix(paper, ink, 0.09f);
    t.overlay = mix(paper, ink, 0.07f);
    t.border = mix(paper, ink, 0.14f);
    t.border_strong = mix(paper, ink, 0.24f);
    t.text_primary = ink;
    t.text_secondary = mix(ink, paper, 0.30f);
    t.text_disabled = mix(ink, paper, 0.55f);
    t.accent = rgb_of(p.ansi[4]);
    t.danger = rgb_of(p.ansi[1]);
    t.success = rgb_of(p.ansi[2]);
    t.warning = rgb_of(p.ansi[3]);
    t.info = rgb_of(p.ansi[6]);
    t.font_size = 12.5f;
    // Nerd icons only when the lead face carries them; otherwise plain glyphs (no tofu)
    t.icons = !font_names().empty() && font_names().front().find("Nerd") != std::string::npos ? IconSet::nerd : IconSet::letters;
    t.motion_scale = 0.0f;   // a terminal never animates its chrome
    // window.opacity: the window is CLEARED to the paper at this alpha
    t.window_opacity = m.cfg.opacity;
    return t;
}

namespace view {

SettingsPanelProps settings_props(const Ctx& c, const Model& m) {
    std::string cfgp = config_path();
    if (const char* hm = std::getenv("HOME"); hm && *hm && cfgp.rfind(hm, 0) == 0) cfgp = "~" + cfgp.substr(std::strlen(hm));
    return SettingsPanelProps{.title = "hand", .subtitle = "terminal settings", .brand = "h",
                              .logo = hand_mark(c, 44.0f, {.cursor = m.cursor_on, .roundness = m.cfg.roundness}),
                              .footer = cfgp,
                              .width = std::min(900.0f, m.win.x - 48.0f),
                              .height = std::min(640.0f, m.win.y - 48.0f)};
}

}  // namespace view

}  // namespace hand
