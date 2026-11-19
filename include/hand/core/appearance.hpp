#pragma once
// hand::Appearance — everything about how hand looks, as one value.
//
// This struct IS the user's configuration: the settings view edits it,
// theme.hpp derives the mayag Theme from it, saving it is serialising it.
// Illegal configurations are unrepresentable: sizes are Bounded, choices
// are enums with display names.

#include "bounded.hpp"
#include "scheme.hpp"

#include <array>
#include <cstdint>
#include <string_view>

namespace hand {

/// Which of mayag's looks (widget/sheet.hpp) dresses the chrome.
enum class Chrome : std::uint8_t { terminal, standard, glass, count_ };
enum class CursorStyle : std::uint8_t { block, beam, underline, count_ };

/// Display names for an enum, in declaration order.
template <class E> inline constexpr std::array<std::string_view, 0> labels{};
template <> inline constexpr std::array<std::string_view, 3> labels<Chrome>{"terminal", "standard", "glass"};
template <> inline constexpr std::array<std::string_view, 3> labels<CursorStyle>{"block", "beam", "underline"};
template <> inline constexpr std::array<std::string_view, scheme_count> labels<SchemeId>{
    "obsidian", "ember", "glacier", "moss", "dusk", "phosphor", "paper"};

/// An enum whose every value has a display name: what `segmented` edits.
template <class E>
concept Labelled = std::is_scoped_enum_v<E> && requires { E::count_; } &&
                   labels<E>.size() == static_cast<std::size_t>(E::count_);

/// Index into the monospace faces found on this machine.
struct FaceIndex {
    std::uint8_t v = 0;
    friend constexpr auto operator<=>(const FaceIndex&, const FaceIndex&) = default;
};

struct Appearance {
    // colour
    SchemeId scheme = SchemeId::obsidian;
    Chrome   chrome = Chrome::terminal;
    bool     scheme_accent = true;                      ///< accent follows the scheme
    Bounded<float, 0.0f, 360.0f> hue{262.0f};           ///< otherwise this Oklch hue
    Bounded<float, 0.6f, 1.0f>   opacity{1.0f};

    // type
    FaceIndex                    mono{};
    Bounded<float, 9.0f, 22.0f>  font_size{13.0f};
    Bounded<float, 1.0f, 1.7f>   line_height{1.3f};
    Bounded<float, 0.7f, 1.3f>   ui_scale{1.0f};        ///< mayag Seed::type
    bool                         nerd_icons = true;     ///< Nerd Font devicons (mayag IconSet::nerd)

    // shape
    Bounded<float, 0.0f, 1.6f>   roundness{0.0f};       ///< mayag Seed::roundness
    Bounded<float, 0.7f, 1.3f>   density{0.9f};         ///< mayag Seed::density
    Bounded<float, 0.0f, 24.0f>  padding{10.0f};        ///< inside each pane

    // terminal
    CursorStyle                  cursor = CursorStyle::block;
    bool                         cursor_blink = true;
    Bounded<float, 0.0f, 0.6f>   dim_unfocused{0.25f};
    bool                         marks = true;          ///< shell-integration gutter
    bool                         pane_titles = true;

    // motion
    Bounded<float, 0.0f, 1.6f>   motion{1.0f};          ///< mayag Seed::motion

    friend constexpr bool operator==(const Appearance&, const Appearance&) = default;
};

}  // namespace hand
