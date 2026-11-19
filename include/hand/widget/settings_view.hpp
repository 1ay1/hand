#pragma once
// hand::settings_view — the Appearance, as a mayag form, generated.
//
//   ┌ appearance ───────────────────── 3 modified ── [reset] [×] ┐
//   │ colour   type   layout   terminal                          │  mayag segmented
//   │ ┌ COLOUR ──────────────────────────────────────────────┐   │  mayag section
//   │ │ colour.scheme   terminal palette …   [obsidian|ember…]│   │  mayag setting_row
//   │ │ colour.chrome   the shape of every…  [terminal|std|gl]│   │    + segmented
//   │ │ colour.opacity  window background …  ━━━━━━━●━━  100% │   │    + slider
//   └──────────────────────────────────────────────────────────────┘
//
// Every row is a mayag setting_row holding a mayag control; the theme's
// Sheet styles them all (under looks::terminal: square, accent-outlined,
// mono). Rows are generated from settings::table, and each control carries
// its edit INLINE as a mayag Edit<Model> -- no ids to decode anywhere.
//
// Labels are the config KEYS (`font.size`): the settings view and the
// config file speak one language.

#include "prelude.hpp"
#include "../core/settings.hpp"
#include "../core/theme.hpp"

#include <mayag/app/handlers.hpp>
#include <mayag/widget/button.hpp>
#include <mayag/widget/kbd.hpp>
#include <mayag/widget/key_hint.hpp>
#include <mayag/widget/segmented.hpp>
#include <mayag/widget/setting_row.hpp>
#include <mayag/widget/slider.hpp>
#include <mayag/widget/toggle.hpp>

#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace hand {

using namespace mayag::dsl;

/// The slider track width every settings slider uses (on_slide maps
/// local.x over it).
inline constexpr float settings_track = 180.0f;

/// How a model with an Appearance at `&Model::look` exposes it to the view.
template <class Model>
concept HasAppearance = requires(Model& m) { { m.look } -> std::same_as<Appearance&>; };

namespace detail {

[[nodiscard]] inline std::string readout(float v, std::string_view unit, float lo, float hi) {
    char buf[32];
    if (unit == "%") std::snprintf(buf, sizeof buf, "%3d%%", static_cast<int>(std::lround(v * 100.0f)));
    else if (unit == "\u00B0" || unit == "px") std::snprintf(buf, sizeof buf, "%3d%.*s", static_cast<int>(std::lround(v)),
                                                             static_cast<int>(unit.size()), unit.data());
    else if (hi - lo >= 10.0f) std::snprintf(buf, sizeof buf, "%4.1f%.*s", static_cast<double>(v),
                                             static_cast<int>(unit.size()), unit.data());
    else std::snprintf(buf, sizeof buf, "%4.2f%.*s", static_cast<double>(v), static_cast<int>(unit.size()), unit.data());
    return buf;
}

/// The mayag control for one row, with its edit attached.
template <class Model, class F>
[[nodiscard]] Node control(const mayag::Ctx& c, const Appearance& a, F, const settings::Row& row,
                           std::span<const std::string> faces) {
    using T = typename F::value_type;
    constexpr auto M = F::member;
    const mayag::Theme& t = c.theme;
    const std::string_view id = row.key;
    if constexpr (F::control == settings::Control::slider) {
        const T v = a.*M;
        const float lo = static_cast<float>(T::lo), hi = static_cast<float>(T::hi);
        auto s = slider(c, id, v.fraction(), settings_track)
               | on_slide([](mayag::Vec2 p) {
                     return mayag::Edit<Model>{[f = p.x / settings_track](Model& m) { m.look.*M = T::from_fraction(f); }};
                 });
        return (h(std::move(s),
                  text_owned(readout(static_cast<float>(v.get()), row.unit, lo, hi)) | font(t.font_size * 0.92f)
                      | monospace | fg(var::text_secondary) | width(64.0f) | text_right)
                | gap(t.spaced(10)) | align(mayag::Align::center)).build();
    } else if constexpr (F::control == settings::Control::toggle) {
        return (switch_control(c, id, a.*M)
                | on_click([] { return mayag::Edit<Model>{[](Model& m) { m.look.*M = !(m.look.*M); }}; })).build();
    } else if constexpr (F::control == settings::Control::choice) {
        std::vector<std::string_view> opts(labels<T>.begin(), labels<T>.end());
        return segmented(c, id, opts, static_cast<int>(a.*M), [](std::size_t i) {
                   return mayag::Edit<Model>{[i](Model& m) { m.look.*M = static_cast<T>(i); }};
               }).build();
    } else {
        std::vector<std::string_view> opts;
        for (std::size_t i = 0; i < faces.size() && i < 6; ++i) opts.push_back(faces[i]);
        if (opts.empty()) opts.push_back("(none found)");
        return segmented(c, id, opts, (a.*M).v, [](std::size_t i) {
                   return mayag::Edit<Model>{[i](Model& m) { m.look.*M = FaceIndex{static_cast<std::uint8_t>(i)}; }};
               }).build();
    }
}

}  // namespace detail

struct SettingsViewProps {
    settings::Section section = settings::Section::colour;
    std::span<const std::string> faces;
};

/// The settings view's content (the app posts it as a modal overlay).
template <class Model>
    requires HasAppearance<Model>
[[nodiscard]] Node settings_view(const mayag::Ctx& c, const Model& m, const SettingsViewProps& p) {
    const mayag::Theme& t = c.theme;
    const Appearance& a = m.look;

    // section switcher: a mayag segmented whose labels carry their counts
    std::vector<std::string> names;
    for (std::size_t s = 0; s < settings::section_names.size(); ++s) {
        const auto n = settings::modified_in(a, static_cast<settings::Section>(s));
        names.push_back(std::string{settings::section_names[s]} + (n ? " \u2022" + std::to_string(n) : ""));
    }
    std::vector<std::string_view> name_views(names.begin(), names.end());
    Node tabs = segmented(c, "settings.section", name_views, static_cast<int>(p.section), [](std::size_t i) {
                    return mayag::Edit<Model>{[i](Model& mm) { mm.section = static_cast<settings::Section>(i); }};
                }).build();

    // the rows of this section, from the table
    std::vector<Node> rows;
    settings::each_row([&](auto field, const settings::Row& row, std::size_t) {
        if (row.section != p.section) return;
        const bool modified = !settings::is_default(a, field);
        auto ctl = node(detail::control<Model>(c, a, field, row, p.faces));
        std::string title = std::string{row.key} + (modified ? "  *" : "");
        rows.push_back(setting_row(c, title, row.hint, std::move(ctl)));
    });
    Node body = section(c, std::string{settings::section_names[static_cast<std::size_t>(p.section)]}, std::move(rows));

    const auto changed = settings::modified_count(a);
    auto header = h(text<"appearance"> | font(t.font_size * 1.15f) | semibold | fg(var::text_primary),
                    text_owned(changed ? std::to_string(changed) + " modified" : "defaults") | font(t.font_size * 0.85f)
                        | monospace | fg(changed ? var::accent : var::text_disabled),
                    spacer(),
                    button<"reset">(t, ButtonVariant::ghost)
                        | on_click([] { return mayag::Edit<Model>{[](Model& mm) {
                              const FaceIndex keep = mm.look.mono;
                              mm.look = Appearance{};
                              mm.look.mono = keep;
                          }}; }),
                    button<"done">(t, ButtonVariant::primary)
                        | on_click([] { return mayag::Edit<Model>{[](Model& mm) { mm.settings_open = false; }}; }))
                | gap(t.spaced(10)) | align(mayag::Align::center) | width(pct(100));

    auto footer = h(key_hint(t, "esc", "close"), key_hint(t, "ctrl ,", "toggle"), spacer(),
                    text<"changes apply live"> | font(t.font_size * 0.8f) | fg(var::text_disabled))
                | gap(t.spaced(16)) | align(mayag::Align::center) | width(pct(100));

    return (v(std::move(header), node(std::move(tabs)), node(std::move(body)), std::move(footer))
            | gap(t.spaced(14)) | pad(t.spaced(18), t.spaced(20)) | width(720.0f)
            | look_of(c, &mayag::Sheet::panel)
            | role(mayag::a11y::Role::dialog) | label("appearance")).build();
}

}  // namespace hand
