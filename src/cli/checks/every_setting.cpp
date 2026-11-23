// hand::cli::checks — every setting, stepped through the panel, takes effect.

#include "suites.hpp"

#include <algorithm>
#include <string>

namespace hand::cli::checks {

using namespace mayag;

void every_setting([[maybe_unused]] Runtime& rt, [[maybe_unused]] Vec2 size, Check& ok) {
    // ── EVERY setting: step it through the panel, check it took effect ──
    using K = mayag::Key;
    using SE = dsl::SettingEdit;
    const auto specs = hand::setting_specs(rt.model().cfg);
    {   // the hints name the key that works on each keyboard, from one table
        bool clean = true, both = false;
        const auto mac = hand::setting_specs(rt.model().cfg, mayag::Keymap::mac), pc = hand::setting_specs(rt.model().cfg, mayag::Keymap::pc);
        for (std::size_t i = 0; i < mac.size(); ++i) {
            clean = clean && mac[i].hint.find('{') == std::string::npos && pc[i].hint.find('{') == std::string::npos;
            if (mac[i].section == "font" && mac[i].key == "size")
                both = mac[i].hint.find("\u2318 +/-") != std::string::npos && pc[i].hint.find("ctrl +/-") != std::string::npos;
        }
        ok(clean && both, "settings hints name each keyboard's key (\u2318 +/- on the Mac, ctrl +/- on the PC), from one table");
    }
    int changed = 0, total = 0;
    std::string stuck;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const auto& s = specs[i];
        if (s.kind == dsl::SettingSpec::Kind::cycle && s.options.size() < 2) continue;   // one font
        ++total;
        const hand::Config before = rt.model().cfg;
        const bool at_top = s.kind == dsl::SettingSpec::Kind::number && s.value >= s.hi;
        const int delta = (s.kind == dsl::SettingSpec::Kind::choice || s.kind == dsl::SettingSpec::Kind::cycle) ? 1 : at_top ? -3 : 3;
        rt.send(hand::App::Msg{hand::App::Panel{dsl::SettingsOp{[i, delta](dsl::SettingsPanelState& x) {
            x.out.push_back({SE::Op::step, i, 0.0f, delta});
        }}}});
        rt.tick();
        const auto after = hand::setting_specs(rt.model().cfg)[i];
        if (after.value != hand::setting_specs(before)[i].value) ++changed;
        else stuck += " " + s.section + "." + s.key;
    }
    if (!stuck.empty()) std::printf("    did not change:%s\n", stuck.c_str());
    ok(changed == total, "every setting changes through the panel and reads back as modified");
    const auto& mm = rt.model();
    const hand::Config d{};
    ok(mm.font_size == mm.cfg.font_size && mm.font_size != d.font_size, "font.size reaches the grid");
    ok(mm.metrics.cell_h == std::round(mm.font_size * mm.cfg.line_height), "font.line_height sets the row height");
    ok(mm.metrics.origin_x == mm.cfg.padding + (mm.cfg.shell_marks ? 10.0f : 0.0f) && mm.cfg.padding != d.padding, "window.padding moves the grid");
    ok(mm.palette.bg != hand::palette_of(d).bg, "colors.scheme recolours the terminal");
    ok(mm.tab().grid && mm.tab().grid->cursor_shape != dsl::TermCursorShape::block, "cursor.style reaches the cursor");
    ok(!mm.cfg.cursor_blink && !mm.cfg.visual_bell && mm.cfg.copy_on_select && !mm.cfg.links_need_ctrl,
       "the toggles flip");
    // reset all: back to every default
    rt.send(hand::App::Msg{hand::App::Panel{dsl::SettingsOp{[](dsl::SettingsPanelState& x) {
        x.out.push_back({SE::Op::reset_all, 0, 0.0f, 0});
    }}}});
    rt.tick();
    std::size_t mods = 0;
    for (const auto& s : hand::setting_specs(rt.model().cfg)) mods += s.modified;
    ok(mods == 0 && rt.model().font_size == d.font_size && rt.model().palette.bg == hand::palette_of(d).bg,
       "reset all restores every default, live");
    (void)K::escape;
}

}  // namespace hand::cli::checks
