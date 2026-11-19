#pragma once
// hand::Scheme — a terminal's colours: the 16 ANSI slots, ink and paper.
//
// A scheme is ONLY the terminal's palette. The chrome around it is a mayag
// Theme (theme.hpp) derived from the scheme, so the two always agree.
// Every slot a program prints in clears its contrast floor on the paper,
// checked at compile time: a scheme that fails does not build.

#include <mayag/mayag.hpp>
#include <mayag/widget/terminal_view.hpp>

#include <array>
#include <cstdint>
#include <string_view>

namespace hand {

enum class SchemeId : std::uint8_t { obsidian, ember, glacier, moss, dusk, phosphor, paper, count_ };
inline constexpr std::size_t scheme_count = static_cast<std::size_t>(SchemeId::count_);

struct Scheme {
    std::string_view name;
    mayag::Mode      mode = mayag::Mode::dark;
    std::uint32_t    paper = 0, ink = 0;
    mayag::dsl::TermPalette ansi{};   ///< 0-7 normal, 8-15 bright
    std::uint32_t    accent = 0;      ///< the accent that suits it
};

inline constexpr std::array<Scheme, scheme_count> schemes{{
    {"obsidian", mayag::Mode::dark, 0x0F1014, 0xD6D9E0,
     {0x23262E, 0xE5737F, 0x8FCB86, 0xE6C07B, 0x7AA7F0, 0xC292E6, 0x6CC9C9, 0xC8CCD4,
      0x6A7080, 0xF08A95, 0xA6DE9C, 0xF2D394, 0x96BDF7, 0xD3ABF1, 0x8ADBDB, 0xF2F4F8}, 0x7AA7F0},
    {"ember", mayag::Mode::dark, 0x15110F, 0xE4D8CC,
     {0x2C2521, 0xE07A5F, 0xA7C080, 0xE9B872, 0x7FA6C4, 0xD08BB0, 0x83C0B0, 0xD5C7B8,
      0x7A6B60, 0xF0937A, 0xBCD296, 0xF4CC8E, 0x9BBDD8, 0xE2A5C5, 0x9DD3C4, 0xF5ECE2}, 0xE9B872},
    {"glacier", mayag::Mode::dark, 0x0B1219, 0xCFDCE6,
     {0x1B2733, 0xEF7B8A, 0x7FD1A9, 0xE8CF8A, 0x6FB3F2, 0xA99BF0, 0x5FD3DE, 0xC3D2DE,
      0x5A7286, 0xF697A4, 0x9BE2BF, 0xF2DDA5, 0x92C7F7, 0xC0B4F6, 0x84E1E9, 0xEEF4F9}, 0x5FD3DE},
    {"moss", mayag::Mode::dark, 0x101410, 0xD3DCCB,
     {0x222A22, 0xDB7C74, 0x9CC77A, 0xD9C27A, 0x79A6B8, 0xB993C2, 0x7FBFA6, 0xC6CFBE,
      0x667462, 0xEA978F, 0xB3D994, 0xE8D497, 0x96BDCD, 0xCDACD5, 0x9AD2BC, 0xF0F4EC}, 0x9CC77A},
    {"dusk", mayag::Mode::dark, 0x13101A, 0xDDD6EA,
     {0x272135, 0xEC7C9A, 0x8FD0A0, 0xEBC88C, 0x8AA2F2, 0xC58CEB, 0x7CCBDD, 0xCFC7DE,
      0x6E6488, 0xF495B0, 0xA7DFB6, 0xF4D9A7, 0xA5B9F7, 0xD6A8F2, 0x99DAE8, 0xF5F1FB}, 0xC58CEB},
    {"phosphor", mayag::Mode::dark, 0x050806, 0x8CF5B0,
     {0x0F1A12, 0xFF6B6B, 0x3DFF8C, 0xE8F56A, 0x5EC8FF, 0xC792FF, 0x4AF2D6, 0xA8F5C2,
      0x3F6B4D, 0xFF8A8A, 0x7BFFAF, 0xF2FA9A, 0x8AD8FF, 0xD9B3FF, 0x86F7E4, 0xD8FFE6}, 0x3DFF8C},
    {"paper", mayag::Mode::light, 0xF6F4EF, 0x2B2A28,
     {0x2B2A28, 0xB8392F, 0x2F7A33, 0x8A5E0E, 0x2659A8, 0x833B9A, 0x1D7474, 0x6A675F,
      0x55524B, 0xA82E25, 0x2C6E30, 0x7F560A, 0x2A5FB3, 0x8A40A3, 0x1B6A6A, 0x3D3A35}, 0x2659A8},
}};

[[nodiscard]] constexpr const Scheme& scheme(SchemeId id) noexcept { return schemes[static_cast<std::size_t>(id)]; }

[[nodiscard]] constexpr mayag::Color<mayag::Srgb> hex(std::uint32_t v, float a = 1.0f) noexcept {
    return mayag::dsl::rgb_of(v, a);
}

namespace detail {
/// Slot 0 on dark paper / slot 7 on light are fills, exempt; slot 8 (bright
/// black) is de-emphasis and held to 3:1; everything else 4.5:1; ink 7:1.
[[nodiscard]] constexpr bool legible(const Scheme& s) noexcept {
    const bool dark = s.mode == mayag::Mode::dark;
    for (std::size_t i = 0; i < 16; ++i) {
        const float floor = ((i == 0 && dark) || (i == 7 && !dark)) ? 0.0f : (i == 8 ? 3.0f : 4.5f);
        if (mayag::contrast_ratio(hex(s.ansi[i]), hex(s.paper)) < floor) return false;
    }
    return mayag::contrast_ratio(hex(s.ink), hex(s.paper)) >= 7.0f;
}
}  // namespace detail

#define HAND_LEGIBLE(S) static_assert(detail::legible(scheme(SchemeId::S)), "hand: scheme " #S " fails contrast")
HAND_LEGIBLE(obsidian); HAND_LEGIBLE(ember); HAND_LEGIBLE(glacier); HAND_LEGIBLE(moss);
HAND_LEGIBLE(dusk); HAND_LEGIBLE(phosphor); HAND_LEGIBLE(paper);
#undef HAND_LEGIBLE
static_assert(scheme_count == 7, "hand: a new scheme needs a HAND_LEGIBLE line");

}  // namespace hand
