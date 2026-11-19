#pragma once
// hand::appearance_view — the Appearance panel: every knob of the look and feel.
//
//   ┌ appearance ──────────────────────────── 920 presets · 4 modified ─ [reset] [done] ┐
//   │  presets   ▍ ┌ preview ─────────────────────────────────────────────────────────┐ │
//   │  colour      │  tab strip + a terminal + status, IN the edited look             │ │
//   │  type        └──────────────────────────────────────────────────────────────────┘ │
//   │  chrome      ⌕ search presets…                       [all] [dark] [light] [hand]  │
//   │  tabs        ▣ Catppuccin Mocha ▣ Dracula ▣ Gruvbox Dark ▣ Nord ▣ Tokyo Night …    │
//   │  panes       (a virtualised gallery of 920 cards, each its own palette)          │
//   │  terminal                                                                          │
//   │  editor      ── or, in any other section: generated setting rows ──              │
//   │  motion                                                                            │
//   └────────────────────────────────────────────────────────────────────────────────────┘
//
// Everything is mayag: nav_item sidebar, term_tabs + terminal_view +
// status_bar in the preview (themed with the panel's own Theme so it shows
// the look you are building), setting_row / slider / switch / segmented
// rows generated from settings::table, text_field search, a virtual_list
// gallery. Edits are mayag Edit<Model> messages carried by the controls;
// the whole window MORPHS behind the panel as you drag.
//
// Hover a preset card to preview it on the whole window; leave to restore;
// click to keep. That is the fastest way through 900 schemes.

#include "prelude.hpp"
#include "../core/settings.hpp"
#include "../core/theme.hpp"
#include "../demo.hpp"

#include <mayag/app/handlers.hpp>
#include <mayag/widget/button.hpp>
#include <mayag/widget/key_hint.hpp>
#include <mayag/widget/nav_item.hpp>
#include <mayag/widget/segmented.hpp>
#include <mayag/widget/setting_row.hpp>
#include <mayag/widget/slider.hpp>
#include <mayag/widget/status_bar.hpp>
#include <mayag/widget/term_tabs.hpp>
#include <mayag/widget/terminal_view.hpp>
#include <mayag/widget/text_field.hpp>
#include <mayag/widget/toggle.hpp>
#include <mayag/widget/virtual_list.hpp>

#include <cstdio>
#include <span>
#include <string>
#include <vector>

namespace hand {

inline constexpr float settings_track = 200.0f;

/// What the panel needs from the app's model.
template <class Model>
concept HasAppearance = requires(Model& m) {
    { m.look } -> std::same_as<Appearance&>;
    { m.section } -> std::same_as<settings::Section&>;
    { m.settings_open } -> std::same_as<bool&>;
    { m.preset_query } -> std::same_as<mayag::TextEditState&>;
    { m.preset_filter } -> std::same_as<int&>;
    { m.preset_scroll } -> std::same_as<mayag::ScrollState&>;
    { m.preview_scheme } -> std::same_as<std::optional<SchemeId>&>;
    { m.preset_search_focused } -> std::same_as<bool&>;
};

/// The presets matching the gallery's search and filter, in catalogue order
/// (hand's own first). Filter: 0 all, 1 dark, 2 light, 3 hand's own.
[[nodiscard]] inline std::vector<SchemeId> preset_matches(std::string_view query, int filter) {
    std::vector<SchemeId> out;
    std::string q;
    for (char ch : query) q.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
    for (std::size_t i = 0; i < scheme_count; ++i) {
        const SchemeId id{static_cast<std::uint16_t>(i)};
        const Scheme s = scheme(id);
        if (filter == 1 && s.mode != mayag::Mode::dark) continue;
        if (filter == 2 && s.mode != mayag::Mode::light) continue;
        if (filter == 3 && i >= own_count) continue;
        if (!q.empty()) {
            std::string n;
            for (char ch : s.name) n.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(ch))));
            // every query word must appear (order-free: "mocha cat" finds Catppuccin Mocha)
            bool all = true;
            std::size_t p = 0;
            while (p < q.size() && all) {
                const std::size_t e = q.find(' ', p);
                const std::string w = q.substr(p, e == std::string::npos ? std::string::npos : e - p);
                if (!w.empty() && n.find(w) == std::string::npos) all = false;
                p = e == std::string::npos ? q.size() : e + 1;
            }
            if (!all) continue;
        }
        out.push_back(id);
    }
    return out;
}

namespace detail {

[[nodiscard]] inline std::string readout(float v, std::string_view unit, float lo, float hi) {
    char buf[32];
    if (unit == "%") std::snprintf(buf, sizeof buf, "%3d%%", static_cast<int>(std::lround(v * 100.0f)));
    else if (unit == "\u00B0" || unit == "px" || unit == "col") std::snprintf(buf, sizeof buf, "%3d%.*s",
            static_cast<int>(std::lround(v)), static_cast<int>(unit.size()), unit.data());
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
    using E = mayag::Edit<Model>;
    if constexpr (F::control == settings::Control::slider) {
        const T v = a.*M;
        const float lo = static_cast<float>(T::lo), hi = static_cast<float>(T::hi);
        auto s = slider(c, id, v.fraction(), settings_track)
               | on_slide([](mayag::Vec2 p) { return E{[f = p.x / settings_track](Model& m) { m.look.*M = T::from_fraction(f); }}; });
        return (h(std::move(s),
                  text_owned(readout(static_cast<float>(v.get()), row.unit, lo, hi)) | font(t.font_size * 0.9f)
                      | monospace | fg(var::text_secondary) | width(64.0f) | text_right)
                | gap(t.spaced(10)) | align(mayag::Align::center)).build();
    } else if constexpr (F::control == settings::Control::toggle) {
        return (switch_control(c, id, a.*M) | on_click([] { return E{[](Model& m) { m.look.*M = !(m.look.*M); }}; })).build();
    } else if constexpr (F::control == settings::Control::choice) {
        std::vector<std::string_view> opts(labels<T>.begin(), labels<T>.end());
        return segmented(c, id, opts, static_cast<int>(a.*M), [](std::size_t i) {
                   return E{[i](Model& m) { m.look.*M = static_cast<T>(i); }};
               }).build();
    } else if constexpr (F::control == settings::Control::face) {
        std::vector<std::string_view> opts;
        for (std::size_t i = 0; i < faces.size() && i < 5; ++i) opts.push_back(faces[i]);
        if (opts.empty()) opts.push_back("(none found)");
        return segmented(c, id, opts, (a.*M).v, [](std::size_t i) {
                   return E{[i](Model& m) { m.look.*M = FaceIndex{static_cast<std::uint8_t>(i)}; }};
               }).build();
    } else {
        // the scheme row's control is the gallery itself
        const Scheme s = scheme(a.*M);
        return (text_owned(std::string{s.name}) | font(t.font_size) | semibold | fg(var::accent)).build();
    }
}

/// A preset as a card: its paper, its name in its ink, a strip of its eight
/// colours, a mini prompt. Hover previews it on the window, click keeps it.
template <class Model>
[[nodiscard]] Node preset_card(const mayag::Ctx& c, SchemeId id, bool chosen, float w) {
    using E = mayag::Edit<Model>;
    const mayag::Theme& t = c.theme;
    const Scheme s = scheme(id);
    const auto pid = part_id("preset", id.v);
    const bool hov = c.hovered(pid);
    std::vector<Node> chips;
    for (std::size_t i = 1; i < 7; ++i) chips.push_back((box() | size(10, 10) | radius(t.rounded(2)) | bg(hex(s.ansi[i]))).build());
    for (std::size_t i = 9; i < 15; ++i) chips.push_back((box() | size(10, 10) | radius(t.rounded(2)) | bg(hex(s.ansi[i]))).build());
    auto prompt = h(text<"\u276F"> | font(11) | monospace | fg(hex(s.ansi[2])),
                    text<" ls"> | font(11) | monospace | fg(hex(s.ink)),
                    text<" src/"> | font(11) | monospace | fg(hex(s.ansi[4])),
                    text<" *.hpp"> | font(11) | monospace | fg(hex(s.ansi[3])));
    return (v(h(text_owned(std::string{s.name}) | font(t.font_size * 0.88f) | semibold | fg(hex(s.ink)) | ellipsis
                    | shrink() | min_size(0, 0),
                spacer(),
                when(chosen, node((text<"\u25CF"> | font(9) | fg(hex(s.accent))).build())))
                  | align(mayag::Align::center) | width(pct(100)),
              std::move(prompt),
              h_list(std::move(chips), 2.0f) | wrap)
            | gap(6) | pad(9, 10) | size(w, 84.0f) | bg(hex(s.paper)) | radius(t.rounded(6))
            | border(chosen ? 2.0f : 1.0f, chosen ? t.accent : (hov ? t.border_strong : t.border),
                     chosen ? mayag::StrokeAlign::outside : mayag::StrokeAlign::inside)
            | when(hov && !chosen, translate(0.0f, -1.0f)) | transition()
            | pointer_cursor | role(mayag::a11y::Role::radio) | checked(chosen) | label(s.name) | id_of_raw(pid)
            | on_enter([id] { return E{[id](Model& m) { m.preview_scheme = id; }}; })
            | on_leave([id] { return E{[id](Model& m) { if (m.preview_scheme == id) m.preview_scheme.reset(); }}; })
            | on_click([id] { return E{[id](Model& m) { m.look.scheme = id; m.preview_scheme.reset(); }}; })).build();
}

/// The preview: the panel's own (edited) theme, applied to a miniature
/// window built from the same widgets as the real one.
[[nodiscard]] inline Node preview(const mayag::Ctx& c, const Appearance& a) {
    const mayag::Theme& t = c.theme;
    const Scheme s = effective_scheme(a);
    std::vector<TermTab> tabs{{.title = "zsh"}, {.title = "nvim", .process = "nvim"}, {.title = "cargo", .running = true}};
    std::vector<StatusItem> left{{.text = "~/src/hand", .icon = a.nerd_icons ? "\uF07B" : "\u25C6", .interactive = false},
                                 {.text = "main", .icon = a.nerd_icons ? "\uE0A0" : "\u2387", .tone = StatusItem::Tone::accent}};
    std::vector<StatusItem> right{{.text = std::string{s.name}, .tone = StatusItem::Tone::dim, .interactive = false}};
    TermGrid g = demo::screen(demo::Screen::shell, s);
    g.cursor_shape = a.cursor == CursorStyle::beam ? TermCursorShape::beam
                   : a.cursor == CursorStyle::underline ? TermCursorShape::underscore : TermCursorShape::block;
    TermViewOptions o{.font_size = a.font_size.get() * 0.85f, .line_height = a.line_height.get(),
                      .padding = a.padding.get() * 0.7f, .focused = true, .marks = a.marks,
                      .family = mayag::custom_family(a.mono.v)};
    return (v(node(term_tabs(c, "preview.tabs", tabs, 0, 0.0)),
              node(terminal_view(c, "preview.grid", g, term_theme_of(a, t), o)) | grow() | width(pct(100)) | min_size(0, 0),
              node(status_bar(c, "preview.status", left, right)))
            | width(pct(100)) | height(230.0f) | clip | radius(t.radius_small) | border(t.stroke(1), t.border)
            | bg(var::background) | role(mayag::a11y::Role::image) | label("preview")).build();
}

}  // namespace detail

struct AppearanceViewProps {
    std::span<const std::string> faces;
    float width = 1000.0f, height = 660.0f;
};

template <class Model>
    requires HasAppearance<Model>
[[nodiscard]] Node appearance_view(const mayag::Ctx& c, const Model& m, const AppearanceViewProps& p) {
    using E = mayag::Edit<Model>;
    const mayag::Theme& t = c.theme;
    const Appearance& a = m.look;
    const bool nerd = a.nerd_icons;

    // ── sidebar: one nav_item per section, modified counts in the trailing slot
    std::vector<Node> nav;
    for (std::size_t i = 0; i < settings::section_names.size(); ++i) {
        const auto sec = static_cast<settings::Section>(i);
        const auto n = settings::modified_in(a, sec);
        const std::string trail = sec == settings::Section::presets ? std::to_string(scheme_count) : (n ? std::to_string(n) : "");
        nav.push_back((node(nav_item(c, "appearance.nav", i, nerd ? settings::section_glyphs[i] : "\u00B7",
                                     settings::section_names[i], m.section == sec, trail, n > 0))
                       | on_click([sec] { return E{[sec](Model& mm) { mm.section = sec; }}; })).build());
    }
    auto sidebar = v(v_list(std::move(nav), 2.0f), spacer(),
                     text_owned("~/.config/hand/appearance") | font(t.font_size * 0.75f) | monospace
                         | fg(var::text_disabled) | ellipsis)
                 | gap(t.spaced(4)) | pad(t.spaced(10), t.spaced(8)) | width(190.0f) | height(pct(100))
                 | bg(var::surface) | inner_shadow(0.0f, t.border, {-1.0f, 0.0f});

    // ── header
    const auto changed = settings::modified_count(a);
    auto header = h(text<"appearance"> | font(t.font_size * 1.2f) | semibold | fg(var::text_primary),
                    text_owned(std::string{settings::section_names[static_cast<std::size_t>(m.section)]})
                        | font(t.font_size * 0.9f) | monospace | fg(var::accent),
                    spacer(),
                    text_owned(changed ? std::to_string(changed) + " modified" : "defaults") | font(t.font_size * 0.85f)
                        | monospace | fg(changed ? var::accent : var::text_disabled),
                    button<"reset section">(t, ButtonVariant::ghost)
                        | on_click([s = m.section] { return E{[s](Model& mm) {
                              if (s == settings::Section::presets) mm.look.scheme = Appearance{}.scheme;
                              else settings::reset_section(mm.look, s);
                          }}; }),
                    button<"reset all">(t, ButtonVariant::ghost)
                        | on_click([] { return E{[](Model& mm) { const auto f = mm.look.mono; mm.look = {}; mm.look.mono = f; }}; }),
                    button<"done">(t, ButtonVariant::primary)
                        | on_click([] { return E{[](Model& mm) { mm.settings_open = false; mm.preview_scheme.reset(); }}; }))
                | gap(t.spaced(10)) | align(mayag::Align::center) | width(pct(100)) | pad(t.spaced(12), t.spaced(16))
                | inner_shadow(0.0f, t.border, {0.0f, -1.0f});

    // ── body
    Node body;
    if (m.section == settings::Section::presets) {
        const auto matches = preset_matches(m.preset_query.text, m.preset_filter);
        const float card_w = 196.0f, gap_px = 10.0f;
        const float area = p.width - 190.0f - 2.0f * t.spaced(16);
        const int per_row = std::max(1, static_cast<int>((area + gap_px) / (card_w + gap_px)));
        const int rows = (static_cast<int>(matches.size()) + per_row - 1) / per_row;
        const SchemeId current = m.preview_scheme.value_or(a.scheme);
        (void)current;
        auto search = h(text_owned(nerd ? "\uF002" : "\u2315") | font(t.font_size) | fg(var::text_disabled),
                        node(text_field(t, m.preset_query, m.preset_search_focused, node_id("preset.search"),
                                        "search 920 presets \u2014 catppuccin, gruvbox, nord, tokyo\u2026", t.font_size)
                                 .build())
                            | grow() | min_size(0, 0)
                            | on_click([] { return E{[](Model& mm) { mm.preset_search_focused = true; }}; }))
                    | gap(t.spaced(8)) | align(mayag::Align::center) | grow() | min_size(0, 0);
        std::vector<std::string_view> filters{"all", "dark", "light", "hand"};
        auto filter = segmented(c, "preset.filter", filters, m.preset_filter, [](std::size_t i) {
            return E{[i](Model& mm) { mm.preset_filter = static_cast<int>(i); mm.preset_scroll = {}; }};
        });
        auto toolbar = h(std::move(search), std::move(filter),
                         text_owned(std::to_string(matches.size())) | font(t.font_size * 0.85f) | monospace
                             | fg(var::text_disabled))
                     | gap(t.spaced(12)) | align(mayag::Align::center) | width(pct(100));
        auto gallery = virtual_list(m.preset_scroll, rows, 84.0f + gap_px, [&](int r) {
            std::vector<Node> cards;
            for (int k = 0; k < per_row; ++k) {
                const std::size_t i = static_cast<std::size_t>(r * per_row + k);
                if (i >= matches.size()) break;
                cards.push_back(detail::preset_card<Model>(c, matches[i], matches[i] == a.scheme, card_w));
            }
            return (h_list(std::move(cards), gap_px) | height(84.0f + gap_px)).build();
        });
        auto list = v(std::move(gallery)) | scroll(m.preset_scroll) | grow() | width(pct(100)) | min_size(0, 0)
                  | on_scroll([](const mayag::Scrolled& s) { return E{[d = s.delta](Model& mm) { mm.preset_scroll.scroll_by(d); }}; });
        body = (v(node(detail::preview(c, a)), std::move(toolbar),
                  matches.empty() ? node((text<"no preset matches"> | font(t.font_size) | fg(var::text_disabled)).build())
                                  : node(std::move(list).build()))
                | gap(t.spaced(12)) | pad(t.spaced(14), t.spaced(16)) | grow() | height(pct(100)) | min_size(0, 0)).build();
    } else {
        std::vector<Node> rows;
        settings::each_row([&](auto field, const settings::Row& row, std::size_t) {
            if (row.section != m.section) return;
            const bool modified = !settings::is_default(a, field);
            using F = decltype(field);
            auto ctl = node(detail::control<Model>(c, a, field, row, p.faces));
            // a modified row says so, and offers its own reset
            auto title = std::string{row.key} + (modified ? "  \u2022" : "");
            Node r = setting_row(c, title, row.hint, std::move(ctl));
            if (modified)
                r = (h(node(std::move(r)) | grow() | min_size(0, 0),
                       h(text_owned(nerd ? "\uF0E2" : "\u21BA") | font(t.font_size) | fg(c.hovered(node_id(std::string{row.key} + ".reset")) ? t.text_primary : t.text_disabled))
                           | size(28, 28) | center | pointer_cursor | label("reset") | id_of(std::string{row.key} + ".reset")
                           | on_click([] { return E{[](Model& mm) { mm.look.*F::member = Appearance{}.*F::member; }}; }))
                     | align(mayag::Align::center) | width(pct(100))).build();
            rows.push_back(std::move(r));
        });
        std::string sec{settings::section_names[static_cast<std::size_t>(m.section)]};
        body = (v(node(detail::preview(c, a)), node(section(c, sec, std::move(rows))))
                | gap(t.spaced(14)) | pad(t.spaced(14), t.spaced(16)) | grow() | height(pct(100)) | min_size(0, 0)
                | scroll(m.preset_scroll) | clip).build();
    }

    auto footer = h(key_hint(t, "esc", "close"), key_hint(t, "ctrl ,", "toggle"), key_hint(t, "hover", "preview a preset"),
                    spacer(),
                    text<"every change applies live \u2014 the window behind morphs"> | font(t.font_size * 0.8f) | fg(var::text_disabled))
                | gap(t.spaced(16)) | align(mayag::Align::center) | width(pct(100)) | pad(t.spaced(8), t.spaced(16))
                | inner_shadow(0.0f, t.border, {0.0f, 1.0f});

    return (v(std::move(header), h(std::move(sidebar), node(std::move(body))) | grow() | width(pct(100)) | min_size(0, 0),
              std::move(footer))
            | size(p.width, p.height) | clip | look_of(c, &mayag::Sheet::panel) | mayag::dsl::pad(0)
            | elevation(t.lift(24)) | role(mayag::a11y::Role::dialog) | label("appearance")).build();
}

}  // namespace hand
