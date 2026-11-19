#pragma once
// hand::Scheme — a terminal's colours: the 16 ANSI slots, ink and paper.
//
// A scheme is ONLY the terminal's palette. The chrome around it is a mayag
// Theme (theme.hpp) derived from the scheme, so the two always agree.
//
// The catalogue: hand's own seven, tuned together and contrast-checked at
// compile time, then the 913 presets of iTerm2-Color-Schemes
// (schemes_gen.hpp, generated). A SchemeId is an index into that
// catalogue; the first seven are hand's.

#include "schemes_gen.hpp"

#include <mayag/mayag.hpp>
#include <mayag/widget/terminal_view.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <string_view>

namespace hand {

struct Scheme {
    std::string_view name;
    mayag::Mode      mode = mayag::Mode::dark;
    std::uint32_t    paper = 0, ink = 0;
    mayag::dsl::TermPalette ansi{};   ///< 0-7 normal, 8-15 bright
    std::uint32_t    accent = 0;      ///< the accent that suits it
    std::uint32_t    cursor = 0, selection = 0;
    std::string_view family = "hand"; ///< "hand" or "iterm2-color-schemes"
};

/// Index into the catalogue (hand's own first). A strong type: it is not a
/// count, and arithmetic on it is a picker's business.
struct SchemeId {
    std::uint16_t v = 0;
    friend constexpr auto operator<=>(const SchemeId&, const SchemeId&) = default;
};

namespace own {
inline constexpr std::array<Scheme, 7> schemes{{
    {"obsidian", mayag::Mode::dark, 0x0F1014, 0xD6D9E0,
     {0x23262E, 0xE5737F, 0x8FCB86, 0xE6C07B, 0x7AA7F0, 0xC292E6, 0x6CC9C9, 0xC8CCD4,
      0x6A7080, 0xF08A95, 0xA6DE9C, 0xF2D394, 0x96BDF7, 0xD3ABF1, 0x8ADBDB, 0xF2F4F8}, 0x7AA7F0, 0x7AA7F0, 0x24324D},
    {"ember", mayag::Mode::dark, 0x15110F, 0xE4D8CC,
     {0x2C2521, 0xE07A5F, 0xA7C080, 0xE9B872, 0x7FA6C4, 0xD08BB0, 0x83C0B0, 0xD5C7B8,
      0x7A6B60, 0xF0937A, 0xBCD296, 0xF4CC8E, 0x9BBDD8, 0xE2A5C5, 0x9DD3C4, 0xF5ECE2}, 0xE9B872, 0xE9B872, 0x4A3A28},
    {"glacier", mayag::Mode::dark, 0x0B1219, 0xCFDCE6,
     {0x1B2733, 0xEF7B8A, 0x7FD1A9, 0xE8CF8A, 0x6FB3F2, 0xA99BF0, 0x5FD3DE, 0xC3D2DE,
      0x5A7286, 0xF697A4, 0x9BE2BF, 0xF2DDA5, 0x92C7F7, 0xC0B4F6, 0x84E1E9, 0xEEF4F9}, 0x5FD3DE, 0x5FD3DE, 0x183A48},
    {"moss", mayag::Mode::dark, 0x101410, 0xD3DCCB,
     {0x222A22, 0xDB7C74, 0x9CC77A, 0xD9C27A, 0x79A6B8, 0xB993C2, 0x7FBFA6, 0xC6CFBE,
      0x667462, 0xEA978F, 0xB3D994, 0xE8D497, 0x96BDCD, 0xCDACD5, 0x9AD2BC, 0xF0F4EC}, 0x9CC77A, 0x9CC77A, 0x2E3D24},
    {"dusk", mayag::Mode::dark, 0x13101A, 0xDDD6EA,
     {0x272135, 0xEC7C9A, 0x8FD0A0, 0xEBC88C, 0x8AA2F2, 0xC58CEB, 0x7CCBDD, 0xCFC7DE,
      0x6E6488, 0xF495B0, 0xA7DFB6, 0xF4D9A7, 0xA5B9F7, 0xD6A8F2, 0x99DAE8, 0xF5F1FB}, 0xC58CEB, 0xC58CEB, 0x3A2A4E},
    {"phosphor", mayag::Mode::dark, 0x050806, 0x8CF5B0,
     {0x0F1A12, 0xFF6B6B, 0x3DFF8C, 0xE8F56A, 0x5EC8FF, 0xC792FF, 0x4AF2D6, 0xA8F5C2,
      0x3F6B4D, 0xFF8A8A, 0x7BFFAF, 0xF2FA9A, 0x8AD8FF, 0xD9B3FF, 0x86F7E4, 0xD8FFE6}, 0x3DFF8C, 0x3DFF8C, 0x123D22},
    {"paper", mayag::Mode::light, 0xF6F4EF, 0x2B2A28,
     {0x2B2A28, 0xB8392F, 0x2F7A33, 0x8A5E0E, 0x2659A8, 0x833B9A, 0x1D7474, 0x6A675F,
      0x55524B, 0xA82E25, 0x2C6E30, 0x7F560A, 0x2A5FB3, 0x8A40A3, 0x1B6A6A, 0x3D3A35}, 0x2659A8, 0x2659A8, 0xCFDBEE},
}};
}  // namespace own

inline constexpr std::size_t own_count = own::schemes.size();
inline constexpr std::size_t scheme_count = own_count + presets::all.size();

[[nodiscard]] constexpr Scheme scheme(SchemeId id) noexcept {
    const std::size_t i = id.v;
    if (i < own_count) return own::schemes[i];
    const auto& p = presets::all[std::min(i - own_count, presets::all.size() - 1)];
    return Scheme{p.name, p.light ? mayag::Mode::light : mayag::Mode::dark, p.paper, p.ink, p.ansi, p.accent,
                  p.cursor, p.selection, "iterm2-color-schemes"};
}

/// hand's own, by name (the defaults and the demo shots).
namespace schemes_by_name {
inline constexpr SchemeId obsidian{0}, ember{1}, glacier{2}, moss{3}, dusk{4}, phosphor{5}, paper{6};
}

/// Case-insensitive lookup in the catalogue ("Catppuccin Mocha").
[[nodiscard]] constexpr std::optional<SchemeId> find_scheme(std::string_view name) noexcept {
    const auto eq = [](std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        return true;
    };
    for (std::size_t i = 0; i < scheme_count; ++i)
        if (eq(scheme(SchemeId{static_cast<std::uint16_t>(i)}).name, name)) return SchemeId{static_cast<std::uint16_t>(i)};
    return std::nullopt;
}

[[nodiscard]] constexpr mayag::Color<mayag::Srgb> hex(std::uint32_t v, float a = 1.0f) noexcept {
    return mayag::dsl::rgb_of(v, a);
}

namespace detail {
/// hand's own seven are held to: slot 0 on dark / 7 on light exempt (fills),
/// slot 8 at 3:1, the rest 4.5:1, ink 7:1. The imported presets are the
/// authors' palettes, shown as they made them.
[[nodiscard]] constexpr bool legible(const Scheme& s) noexcept {
    const bool dark = s.mode == mayag::Mode::dark;
    for (std::size_t i = 0; i < 16; ++i) {
        const float floor = ((i == 0 && dark) || (i == 7 && !dark)) ? 0.0f : (i == 8 ? 3.0f : 4.5f);
        if (mayag::contrast_ratio(hex(s.ansi[i]), hex(s.paper)) < floor) return false;
    }
    return mayag::contrast_ratio(hex(s.ink), hex(s.paper)) >= 7.0f;
}
}  // namespace detail

#define HAND_LEGIBLE(I) static_assert(detail::legible(own::schemes[I]), "hand: an own scheme fails contrast")
HAND_LEGIBLE(0); HAND_LEGIBLE(1); HAND_LEGIBLE(2); HAND_LEGIBLE(3); HAND_LEGIBLE(4); HAND_LEGIBLE(5); HAND_LEGIBLE(6);
#undef HAND_LEGIBLE
static_assert(own_count == 7, "hand: a new own scheme needs a HAND_LEGIBLE line");
static_assert(scheme_count > 600, "hand: the preset catalogue is missing (re-run tools/gen_schemes.py)");

}  // namespace hand
