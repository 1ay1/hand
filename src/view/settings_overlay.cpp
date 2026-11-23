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
    // The panel is ~600 nodes: built once and reused while only its fade
    // moves (the fade is the wrapper's opacity, outside the memo). The key
    // is everything the panel reads; hover inside it is tracked by memo().
    const auto& ui = m.settings.ui;
    const MemoKey key = MemoKey{}
        .add(settings_key(m.cfg)).add(ui.selected).add(ui.query).add(ui.filtering)
        .add(ui.scroll.offset.y).add(ui.menu).add(ui.menu_query).add(ui.menu_hot).add(ui.menu_scroll.offset.y)
        .add(ui.menu_origin).add(m.win.x).add(m.win.y).add(m.cursor_on).add(m.cfg.roundness).add(m.env.config_path).add(static_cast<int>(m.env.keymap));
    Node panel = memo(key, [&] {
        return settings_panel(c, "settings", setting_specs(m.cfg, m.env.keymap), ui, settings_props(c, m),
                              [](SettingsOp op) -> App::Msg { return App::Panel{std::move(op)}; });
    });
    c.overlay(Overlay{
        .content = (node(std::move(panel)) | opacity(e) | translate(0.0f, (1.0f - e) * 16.0f)).build(),
        .id = part_id("settings", part_menu),
        .placement = Placement::centered,
        .dismiss = m.settings.open ? Dismiss::modal : Dismiss::never,
        .layer = 10,
        .scrim = 0.35f * e,
        .passthrough = !m.settings.open});   // closing: already out of the way
}

}  // namespace hand::view
