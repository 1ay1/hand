#pragma once
// hand::settings — the configuration, described once, as types.
//
// Each knob is ONE row: the Appearance member it edits, a label, a hint and
// a section. The member's TYPE picks the mayag control that edits it:
//
//     Bounded<float, lo, hi>  -> mayag slider (track = the type's range)
//     bool                    -> mayag switch_control
//     a Labelled enum         -> mayag segmented
//     FaceIndex               -> mayag segmented over the faces found
//
// The settings view (widget/settings_view.hpp) is generated from this
// table; it never names a setting. A field of a type no control can edit is
// a compile error on the row that names it.

#include "appearance.hpp"

#include <array>
#include <string_view>
#include <tuple>
#include <type_traits>

namespace hand::settings {

enum class Section : std::uint8_t { colour, type, layout, terminal, count_ };
inline constexpr std::array<std::string_view, 4> section_names{"colour", "type", "layout", "terminal"};

enum class Control : std::uint8_t { slider, toggle, choice, face };

template <class T> inline constexpr bool is_bounded = false;
template <class T, T Lo, T Hi> inline constexpr bool is_bounded<Bounded<T, Lo, Hi>> = true;

template <class T>
[[nodiscard]] consteval Control control_for() {
    if constexpr (is_bounded<T>) return Control::slider;
    else if constexpr (std::same_as<T, bool>) return Control::toggle;
    else if constexpr (std::same_as<T, FaceIndex>) return Control::face;
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
    std::string_view unit;      ///< sliders: "pt", "%", "px", "\u00D7", "\u00B0"
    Section          section;
};

inline constexpr auto table = std::make_tuple(
    std::pair{Field<&Appearance::scheme>{},        Row{"colour.scheme", "terminal palette; the chrome follows it", "", Section::colour}},
    std::pair{Field<&Appearance::chrome>{},        Row{"colour.chrome", "the shape of every widget (a mayag look)", "", Section::colour}},
    std::pair{Field<&Appearance::scheme_accent>{}, Row{"colour.scheme_accent", "use the scheme's own accent", "", Section::colour}},
    std::pair{Field<&Appearance::hue>{},           Row{"colour.hue", "accent hue when not the scheme's", "\u00B0", Section::colour}},
    std::pair{Field<&Appearance::opacity>{},       Row{"colour.opacity", "window background opacity", "%", Section::colour}},
    std::pair{Field<&Appearance::mono>{},          Row{"font.family", "monospace faces found on this machine", "", Section::type}},
    std::pair{Field<&Appearance::font_size>{},     Row{"font.size", "terminal text", "pt", Section::type}},
    std::pair{Field<&Appearance::line_height>{},   Row{"font.line_height", "row pitch, a multiple of the size", "\u00D7", Section::type}},
    std::pair{Field<&Appearance::ui_scale>{},      Row{"ui.scale", "chrome text size", "\u00D7", Section::type}},
    std::pair{Field<&Appearance::roundness>{},     Row{"ui.roundness", "0 = square corners everywhere", "\u00D7", Section::layout}},
    std::pair{Field<&Appearance::density>{},       Row{"ui.density", "spacing throughout the chrome", "\u00D7", Section::layout}},
    std::pair{Field<&Appearance::padding>{},       Row{"pane.padding", "inside each pane", "px", Section::layout}},
    std::pair{Field<&Appearance::pane_titles>{},   Row{"pane.titles", "title strip on split panes", "", Section::layout}},
    std::pair{Field<&Appearance::motion>{},        Row{"ui.motion", "spring speed; 0 disables animation", "\u00D7", Section::layout}},
    std::pair{Field<&Appearance::cursor>{},        Row{"cursor.style", "shape of the text cursor", "", Section::terminal}},
    std::pair{Field<&Appearance::cursor_blink>{},  Row{"cursor.blink", "blink while idle", "", Section::terminal}},
    std::pair{Field<&Appearance::dim_unfocused>{}, Row{"pane.dim_unfocused", "darken panes without focus", "%", Section::terminal}},
    std::pair{Field<&Appearance::marks>{},         Row{"shell.marks", "prompt / exit-status gutter (OSC 133)", "", Section::terminal}}
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

static_assert(modified_count(Appearance{}) == 0);

}  // namespace hand::settings
