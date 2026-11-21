#pragma once
// hand::Palette — the terminal's colours: 16 ANSI plus default ink, paper,
// cursor and selection, as 0xRRGGBB. A core type: the VT engine paints with
// it; config/ only decides which one (the scheme catalogue).

#include <cstdint>

namespace hand {

struct Palette {
    std::uint32_t fg = 0xABB2BF, bg = 0x282C34, cursor = 0x74ADE8, selection = 0x3E4451;
    std::uint32_t ansi[16] = {0x282C34, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xABB2BF,
                              0x5C6370, 0xE06C75, 0x98C379, 0xE5C07B, 0x61AFEF, 0xC678DD, 0x56B6C2, 0xFFFFFF};
    bool light = false;
};

}  // namespace hand
