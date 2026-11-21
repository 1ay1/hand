// hand::view — the settings panel over the terminal (ctrl+,), easing in and out.

#include "view.hpp"

#include <mayag/widget/settings_panel.hpp>

namespace hand::view {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

void settings_overlay(const Ctx& c, const Model& m) {
    if (!m.settings.open && m.settings.t <= 0.0f) return;
    const float e = 1.0f - (1.0f - m.settings.t) * (1.0f - m.settings.t);   // ease-out
    c.overlay(Overlay{
        .content = (node(settings_panel(c, "settings", setting_specs(m.cfg), m.settings.ui, settings_props(c, m),
                                        [](SettingsOp op) -> App::Msg { return App::Panel{std::move(op)}; }))
                    | opacity(e) | translate(0.0f, (1.0f - e) * 16.0f)).build(),
        .id = part_id("settings", part_menu),
        .placement = Placement::centered,
        .dismiss = m.settings.open ? Dismiss::modal : Dismiss::never,
        .layer = 10,
        .scrim = 0.35f * e,
        .passthrough = !m.settings.open});   // closing: already out of the way
}

}  // namespace hand::view
