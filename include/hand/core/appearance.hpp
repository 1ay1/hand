#pragma once
// hand::Appearance — EVERYTHING about how hand looks and feels, as one value.
//
// This struct IS the user's configuration: the appearance panel edits it,
// theme.hpp turns it into a mayag Theme (+ Sheet + chrome geometry), saving
// it is serialising it. Illegal configurations are unrepresentable: sizes
// are Bounded, choices are enums with display names, colours are optional
// overrides on top of the scheme.
//
// Grouped as the panel shows it: colour, type, chrome, tabs, panes,
// terminal, editor, motion. Every field has a row in settings.hpp.

#include "bounded.hpp"
#include "scheme.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace hand {

/// Which of mayag's looks (widget/sheet.hpp) shapes every widget.
enum class Chrome : std::uint8_t { refined, terminal, standard, glass, brutal, count_ };
enum class CursorStyle : std::uint8_t { block, beam, underline, count_ };
/// mayag::TabShape, mirrored so the settings table can label it.
enum class Tabs : std::uint8_t { underline, overline, block, pill, slant, count_ };
/// Where the accent comes from.
enum class AccentFrom : std::uint8_t { scheme, hue, ansi, count_ };
/// Light/dark: the scheme's own, or forced (mayag derives the other polarity).
enum class Polarity : std::uint8_t { scheme, dark, light, count_ };
enum class Bell : std::uint8_t { off, flash, count_ };

/// Display names for an enum, in declaration order.
template <class E> inline constexpr std::array<std::string_view, 0> labels{};
template <> inline constexpr std::array<std::string_view, 5> labels<Chrome>{"refined", "terminal", "standard", "glass", "brutal"};
template <> inline constexpr std::array<std::string_view, 3> labels<CursorStyle>{"block", "beam", "underline"};
template <> inline constexpr std::array<std::string_view, 5> labels<Tabs>{"underline", "overline", "block", "pill", "slant"};
template <> inline constexpr std::array<std::string_view, 3> labels<AccentFrom>{"scheme", "hue", "ansi slot"};
template <> inline constexpr std::array<std::string_view, 3> labels<Polarity>{"scheme", "dark", "light"};
template <> inline constexpr std::array<std::string_view, 2> labels<Bell>{"off", "flash"};

/// An enum whose every value has a display name: what `segmented` edits.
template <class E>
concept Labelled = std::is_scoped_enum_v<E> && requires { E::count_; } &&
                   labels<E>.size() == static_cast<std::size_t>(E::count_);

/// Index into the monospace faces found on this machine.
struct FaceIndex {
    std::uint8_t v = 0;
    friend constexpr auto operator<=>(const FaceIndex&, const FaceIndex&) = default;
};

/// An ANSI slot 0..15 (for "accent = bright blue").
using AnsiSlot = Bounded<int, 0, 15>;

struct Appearance {
    // ── colour ─────────────────────────────────────────────────────────
    SchemeId   scheme = schemes_by_name::obsidian;
    Polarity   polarity = Polarity::scheme;
    AccentFrom accent_from = AccentFrom::scheme;
    Bounded<float, 0.0f, 360.0f> hue{262.0f};          ///< accent_from = hue
    Bounded<float, 0.0f, 0.37f>  chroma{0.15f};        ///< accent_from = hue
    AnsiSlot                     accent_slot{12};      ///< accent_from = ansi
    Bounded<float, -0.12f, 0.12f> chrome_shade{0.0f};  ///< chrome darker (-) / lighter (+) than the paper
    Bounded<float, 0.0f, 0.06f>  chrome_tint{0.0f};    ///< chrome tinted toward the accent
    Bounded<float, 0.5f, 2.0f>   border_strength{1.0f};
    Bounded<float, 0.6f, 1.0f>   opacity{1.0f};
    bool                         bold_is_bright = false;

    // ── type ───────────────────────────────────────────────────────────
    FaceIndex                    mono{};
    Bounded<float, 8.0f, 24.0f>  font_size{13.0f};
    Bounded<float, 1.0f, 1.8f>   line_height{1.3f};
    Bounded<float, 0.7f, 1.4f>   ui_scale{1.0f};       ///< mayag Seed::type
    Bounded<float, 0.75f, 1.1f>  chrome_type{0.92f};   ///< chrome text vs ui text
    bool                         nerd_icons = true;

    // ── chrome ─────────────────────────────────────────────────────────
    Chrome                       chrome = Chrome::refined;
    Bounded<float, 0.0f, 1.8f>   roundness{0.6f};      ///< mayag Seed::roundness
    Bounded<float, 0.6f, 1.5f>   density{0.9f};        ///< mayag Seed::density
    Bounded<float, 0.0f, 2.0f>   depth{0.0f};          ///< mayag Seed::depth (shadows)
    Bounded<float, 0.5f, 2.5f>   lines{1.0f};          ///< mayag Seed::lines (stroke widths)
    Bounded<float, 24.0f, 48.0f> bar_height{34.0f};
    Bounded<float, 18.0f, 36.0f> status_height{26.0f};
    bool                         status_bar = true;
    bool                         window_buttons = true;

    // ── tabs ───────────────────────────────────────────────────────────
    Tabs                         tabs = Tabs::block;
    Bounded<float, 110.0f, 320.0f> tab_width{200.0f};
    bool                         tab_numbers = true;
    bool                         tab_close_on_hover = true;

    // ── panes ──────────────────────────────────────────────────────────
    Bounded<float, 0.0f, 32.0f>  padding{10.0f};       ///< inside each pane
    Bounded<float, 0.0f, 0.7f>   dim_unfocused{0.25f};
    bool                         pane_titles = true;

    // ── terminal ───────────────────────────────────────────────────────
    CursorStyle                  cursor = CursorStyle::block;
    bool                         cursor_blink = true;
    Bounded<float, 0.3f, 1.5f>   blink_rate{0.55f};    ///< seconds per phase
    bool                         marks = true;         ///< shell-integration gutter
    Bell                         bell = Bell::flash;

    // ── editor ─────────────────────────────────────────────────────────
    bool                         ligatures = true;
    bool                         line_numbers = true;
    bool                         relative_numbers = false;
    bool                         indent_guides = true;
    bool                         current_line = true;
    Bounded<int, 0, 160>         ruler{100};           ///< 0 = none

    // ── motion ─────────────────────────────────────────────────────────
    Bounded<float, 0.0f, 2.0f>   motion{1.0f};         ///< mayag Seed::motion
    bool                         theme_morph = true;   ///< animate theme changes

    friend constexpr bool operator==(const Appearance&, const Appearance&) = default;
};

}  // namespace hand
