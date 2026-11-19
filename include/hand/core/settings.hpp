#pragma once
// hand::settings — the configuration, described once, as types.
//
// Each knob is ONE row: the Appearance member it edits, its config key, a
// hint and a section. The member's TYPE picks the mayag control:
//
//     Bounded<T, lo, hi>  -> mayag slider (track = the type's range, readout in `unit`)
//     bool                -> mayag switch_control
//     a Labelled enum     -> mayag segmented
//     FaceIndex           -> segmented over the faces found
//     SchemeId            -> the preset gallery (search + live swatches)
//
// The appearance panel is generated from this table and never names a
// setting. A field of a type no control can edit is a compile error.

#include "appearance.hpp"

#include <array>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace hand::settings {

enum class Section : std::uint8_t { presets, colour, type, chrome, tabs, panes, terminal, editor, motion, count_ };
inline constexpr std::array<std::string_view, 9> section_names{
    "presets", "colour", "type", "chrome", "tabs", "panes", "terminal", "editor", "motion"};
inline constexpr std::array<std::string_view, 9> section_glyphs{
    "\uF53F", "\uE22B", "\uF031", "\uF2D0", "\uF2D2", "\uF009", "\uF120", "\uF121", "\uF061"};

enum class Control : std::uint8_t { slider, toggle, choice, face, scheme };

template <class T> inline constexpr bool is_bounded = false;
template <class T, T Lo, T Hi> inline constexpr bool is_bounded<Bounded<T, Lo, Hi>> = true;

template <class T>
[[nodiscard]] consteval Control control_for() {
    if constexpr (is_bounded<T>) return Control::slider;
    else if constexpr (std::same_as<T, bool>) return Control::toggle;
    else if constexpr (std::same_as<T, FaceIndex>) return Control::face;
    else if constexpr (std::same_as<T, SchemeId>) return Control::scheme;
    else if constexpr (Labelled<T>) return Control::choice;
    else static_assert(sizeof(T) == 0, "hand::settings: no control can edit this field's type");
}

template <auto Member> struct Field;
template <class T, T Appearance::*M>
struct Field<M> {
    using value_type = T;
    static constexpr Control control = control_for<T>();
    static constexpr T Appearance::*member = M;
};

struct Row {
    std::string_view key;       ///< config key: "font.size" -- also the control's id
    std::string_view hint;
    std::string_view unit;      ///< sliders: "pt", "%", "px", "\u00D7", "\u00B0", ""
    Section          section;
};

using S = Section;
inline constexpr auto table = std::make_tuple(
    std::pair{Field<&Appearance::scheme>{},             Row{"colour.scheme", "the terminal palette; the chrome is derived from it", "", S::presets}},

    std::pair{Field<&Appearance::polarity>{},           Row{"colour.polarity", "keep the scheme's, or force dark / light", "", S::colour}},
    std::pair{Field<&Appearance::accent_from>{},        Row{"colour.accent", "where the accent comes from", "", S::colour}},
    std::pair{Field<&Appearance::hue>{},                Row{"colour.accent_hue", "Oklch hue, when accent = hue", "\u00B0", S::colour}},
    std::pair{Field<&Appearance::chroma>{},             Row{"colour.accent_chroma", "Oklch chroma, when accent = hue", "", S::colour}},
    std::pair{Field<&Appearance::accent_slot>{},        Row{"colour.accent_slot", "ANSI slot, when accent = ansi slot", "", S::colour}},
    std::pair{Field<&Appearance::chrome_shade>{},       Row{"colour.chrome_shade", "chrome darker (-) or lighter (+) than the terminal", "", S::colour}},
    std::pair{Field<&Appearance::chrome_tint>{},        Row{"colour.chrome_tint", "tint the chrome toward the accent", "", S::colour}},
    std::pair{Field<&Appearance::border_strength>{},    Row{"colour.borders", "how strong hairlines are", "\u00D7", S::colour}},
    std::pair{Field<&Appearance::opacity>{},            Row{"colour.opacity", "window background opacity", "%", S::colour}},
    std::pair{Field<&Appearance::bold_is_bright>{},     Row{"colour.bold_is_bright", "bold text drawn brighter", "", S::colour}},

    std::pair{Field<&Appearance::mono>{},               Row{"font.family", "monospace faces found on this machine", "", S::type}},
    std::pair{Field<&Appearance::font_size>{},          Row{"font.size", "terminal and editor text", "pt", S::type}},
    std::pair{Field<&Appearance::line_height>{},        Row{"font.line_height", "row pitch, a multiple of the size", "\u00D7", S::type}},
    std::pair{Field<&Appearance::ui_scale>{},           Row{"ui.scale", "chrome text size", "\u00D7", S::type}},
    std::pair{Field<&Appearance::chrome_type>{},        Row{"ui.chrome_type", "tab / status text relative to the ui", "\u00D7", S::type}},
    std::pair{Field<&Appearance::nerd_icons>{},         Row{"ui.nerd_icons", "devicons from a Nerd Font", "", S::type}},

    std::pair{Field<&Appearance::chrome>{},             Row{"ui.look", "the shape of every widget (a mayag look)", "", S::chrome}},
    std::pair{Field<&Appearance::roundness>{},          Row{"ui.roundness", "0 = square corners everywhere", "\u00D7", S::chrome}},
    std::pair{Field<&Appearance::density>{},            Row{"ui.density", "spacing throughout the chrome", "\u00D7", S::chrome}},
    std::pair{Field<&Appearance::depth>{},              Row{"ui.depth", "shadows and elevation; 0 = flat", "\u00D7", S::chrome}},
    std::pair{Field<&Appearance::lines>{},              Row{"ui.lines", "border and ring widths", "\u00D7", S::chrome}},
    std::pair{Field<&Appearance::bar_height>{},         Row{"ui.bar_height", "tab strip height", "px", S::chrome}},
    std::pair{Field<&Appearance::status_height>{},      Row{"ui.status_height", "status bar height", "px", S::chrome}},
    std::pair{Field<&Appearance::status_bar>{},         Row{"ui.status_bar", "show the status bar", "", S::chrome}},
    std::pair{Field<&Appearance::window_buttons>{},     Row{"ui.window_buttons", "minimise / maximise / close", "", S::chrome}},

    std::pair{Field<&Appearance::tabs>{},               Row{"tabs.style", "how the active tab is marked", "", S::tabs}},
    std::pair{Field<&Appearance::tab_width>{},          Row{"tabs.width", "preferred tab width", "px", S::tabs}},
    std::pair{Field<&Appearance::tab_numbers>{},        Row{"tabs.numbers", "1..9 on idle tabs", "", S::tabs}},
    std::pair{Field<&Appearance::tab_close_on_hover>{}, Row{"tabs.close_on_hover", "close button only on the active / hovered tab", "", S::tabs}},

    std::pair{Field<&Appearance::padding>{},            Row{"pane.padding", "inside each pane", "px", S::panes}},
    std::pair{Field<&Appearance::dim_unfocused>{},      Row{"pane.dim_unfocused", "darken panes without focus", "%", S::panes}},
    std::pair{Field<&Appearance::pane_titles>{},        Row{"pane.titles", "title strip on split panes", "", S::panes}},

    std::pair{Field<&Appearance::cursor>{},             Row{"cursor.style", "shape of the text cursor", "", S::terminal}},
    std::pair{Field<&Appearance::cursor_blink>{},       Row{"cursor.blink", "blink while idle", "", S::terminal}},
    std::pair{Field<&Appearance::blink_rate>{},         Row{"cursor.blink_rate", "seconds per blink phase", "s", S::terminal}},
    std::pair{Field<&Appearance::marks>{},              Row{"shell.marks", "prompt / exit-status gutter (OSC 133)", "", S::terminal}},
    std::pair{Field<&Appearance::bell>{},               Row{"bell", "visual bell", "", S::terminal}},

    std::pair{Field<&Appearance::ligatures>{},          Row{"editor.ligatures", "-> != === as single glyphs", "", S::editor}},
    std::pair{Field<&Appearance::line_numbers>{},       Row{"editor.line_numbers", "gutter numbers", "", S::editor}},
    std::pair{Field<&Appearance::relative_numbers>{},   Row{"editor.relative_numbers", "count from the caret line", "", S::editor}},
    std::pair{Field<&Appearance::indent_guides>{},      Row{"editor.indent_guides", "vertical guides per indent level", "", S::editor}},
    std::pair{Field<&Appearance::current_line>{},       Row{"editor.current_line", "highlight the caret's line", "", S::editor}},
    std::pair{Field<&Appearance::ruler>{},              Row{"editor.ruler", "column guide; 0 = none", "col", S::editor}},

    std::pair{Field<&Appearance::motion>{},             Row{"ui.motion", "spring speed; 0 disables animation", "\u00D7", S::motion}},
    std::pair{Field<&Appearance::theme_morph>{},        Row{"ui.theme_morph", "morph the whole window on a theme change", "", S::motion}}
);

inline constexpr std::size_t row_count = std::tuple_size_v<decltype(table)>;

/// Visit every row with its static type: `f(Field<M>{}, row, index)`.
template <class F>
constexpr void each_row(F&& f) {
    [&]<std::size_t... I>(std::index_sequence<I...>) {
        (f(std::get<I>(table).first, std::get<I>(table).second, I), ...);
    }(std::make_index_sequence<row_count>{});
}

template <auto M>
[[nodiscard]] constexpr bool is_default(const Appearance& a, Field<M>) noexcept { return a.*M == Appearance{}.*M; }
template <auto M>
constexpr void reset(Appearance& a, Field<M>) noexcept { a.*M = Appearance{}.*M; }

[[nodiscard]] constexpr std::size_t modified_count(const Appearance& a) noexcept {
    std::size_t n = 0;
    each_row([&](auto f, const Row&, std::size_t) { n += is_default(a, f) ? 0 : 1; });
    return n;
}
[[nodiscard]] constexpr std::size_t modified_in(const Appearance& a, Section s) noexcept {
    std::size_t n = 0;
    each_row([&](auto f, const Row& r, std::size_t) { n += (r.section == s && !is_default(a, f)) ? 1 : 0; });
    return n;
}
constexpr void reset_section(Appearance& a, Section s) noexcept {
    each_row([&](auto f, const Row& r, std::size_t) { if (r.section == s) reset(a, f); });
}

static_assert(modified_count(Appearance{}) == 0);
static_assert(row_count >= 45, "every Appearance field has a row");

}  // namespace hand::settings
