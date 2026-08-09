// SPDX-License-Identifier: LGPL-2.0-or-later
//
// glyph — a tiny, dependency-free immediate-mode GUI toolkit for the TERMINAL
// GRID. Widgets paint into a Buffer of ordinary terminal cells (the very cells
// toe's renderer already draws), so an in-terminal settings panel costs ZERO
// new rendering code and looks native on every backend (Cocoa/Wayland/X11).
//
// This file is the CANVAS layer: a Buffer of styled cells + painting
// primitives (fill, text, box frames, rules). Widgets and the immediate-mode
// context build on top (glyph.hpp). Nothing here knows about GL, windows, or
// input — it just produces a grid of cells a host composites over the terminal.

#ifndef HAND_GLYPH_BUFFER_HPP
#define HAND_GLYPH_BUFFER_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "toe/core/types.hpp" // Rgb, rgb()
#include "toe/term/cell.hpp"  // Cell, Pen, Color, TrueColor, Attr

namespace glyph {

using toe::Rgb;
using toe::rgb;
using Cell = toe::term::Cell;
using Pen = toe::term::Pen;
using Attr = toe::term::Attr;

// A style is just a foreground/background pair + attributes, as true-color so
// the panel controls its own palette independent of the terminal's.
struct Style {
    Rgb fg{rgb(220, 220, 220)};
    Rgb bg{rgb(30, 30, 38)};
    Attr attr{Attr::None};

    [[nodiscard]] Pen pen() const noexcept {
        return Pen{.fg = toe::term::TrueColor{fg},
                   .bg = toe::term::TrueColor{bg},
                   .attr = attr};
    }
};

// A rectangle in cell coordinates (col x, row y, w wide, h tall).
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    [[nodiscard]] int right() const noexcept { return x + w; }
    [[nodiscard]] int bottom() const noexcept { return y + h; }
    [[nodiscard]] bool contains(int cx, int cy) const noexcept {
        return cx >= x && cx < x + w && cy >= y && cy < y + h;
    }
    [[nodiscard]] Rect inset(int d) const noexcept { return {x + d, y + d, w - 2 * d, h - 2 * d}; }
};

// Box-drawing sets, so a frame can be light / heavy / rounded / double.
enum class BoxStyle { Light, Heavy, Rounded, Double };

// A grid of cells the UI paints into. Fixed size for a frame; cheap to clear
// and reuse. Row-major, cells[y*w + x].
class Buffer {
public:
    Buffer() = default;
    Buffer(int w, int h) { resize(w, h); }

    void resize(int w, int h) {
        w_ = w < 0 ? 0 : w;
        h_ = h < 0 ? 0 : h;
        cells_.assign(static_cast<std::size_t>(w_) * static_cast<std::size_t>(h_), Cell{});
    }
    [[nodiscard]] int width() const noexcept { return w_; }
    [[nodiscard]] int height() const noexcept { return h_; }
    [[nodiscard]] const Cell *data() const noexcept { return cells_.data(); }
    [[nodiscard]] Rect bounds() const noexcept { return {0, 0, w_, h_}; }

    void clear(const Style &s) {
        Cell c;
        c.cp = U' ';
        c.pen = s.pen();
        for (Cell &cell : cells_) cell = c;
    }

    // Write one cell (bounds-checked).
    void put(int x, int y, char32_t cp, const Style &s) {
        if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
        Cell &c = cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) +
                          static_cast<std::size_t>(x)];
        c.cp = cp;
        c.pen = s.pen();
        c.width = 1;
    }

    // Fill a rectangle with `cp` (default space) in style `s`.
    void fill(Rect r, const Style &s, char32_t cp = U' ') {
        for (int y = r.y; y < r.bottom(); ++y)
            for (int x = r.x; x < r.right(); ++x) put(x, y, cp, s);
    }

    // Draw UTF-8 text starting at (x,y), clipped to `max_w` cells (0 = to edge).
    // Returns the number of CELLS advanced (accounting for wide glyphs).
    int text(int x, int y, std::string_view utf8, const Style &s, int max_w = 0) {
        const int limit = max_w > 0 ? x + max_w : w_;
        int cx = x;
        std::size_t i = 0;
        while (i < utf8.size() && cx < limit) {
            char32_t cp = 0;
            i += decode_utf8(utf8, i, cp);
            const int cw = cell_width(cp);
            if (cx + cw > limit) break;
            put(cx, y, cp, s);
            if (cw == 2) {
                // Mark the trailing spacer so the renderer skips it.
                if (cx + 1 < w_ && y >= 0 && y < h_) {
                    Cell &sp = cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(w_) +
                                      static_cast<std::size_t>(cx + 1)];
                    sp.cp = U' ';
                    sp.pen = s.pen();
                    sp.width = 0;
                }
            }
            cx += cw;
        }
        return cx - x;
    }

    // A frame around `r` in the given box style; interior is left untouched.
    void frame(Rect r, const Style &s, BoxStyle bs = BoxStyle::Rounded) {
        if (r.w < 2 || r.h < 2) return;
        const Box b = box_chars(bs);
        put(r.x, r.y, b.tl, s);
        put(r.right() - 1, r.y, b.tr, s);
        put(r.x, r.bottom() - 1, b.bl, s);
        put(r.right() - 1, r.bottom() - 1, b.br, s);
        for (int x = r.x + 1; x < r.right() - 1; ++x) {
            put(x, r.y, b.h, s);
            put(x, r.bottom() - 1, b.h, s);
        }
        for (int y = r.y + 1; y < r.bottom() - 1; ++y) {
            put(r.x, y, b.v, s);
            put(r.right() - 1, y, b.v, s);
        }
    }

    // A horizontal rule across [r.x, r.right()) at row `y`.
    void hrule(int x, int y, int w, const Style &s, BoxStyle bs = BoxStyle::Light) {
        const char32_t h = box_chars(bs).h;
        for (int i = 0; i < w; ++i) put(x + i, y, h, s);
    }

    // --- static text helpers ------------------------------------------------
    // Display width of a codepoint (2 for CJK/emoji fullwidth, else 1).
    [[nodiscard]] static int cell_width(char32_t cp) noexcept {
        if (cp == 0) return 0;
        // Common wide ranges (a compact subset of the East-Asian-Width tables).
        if ((cp >= 0x1100 && cp <= 0x115F) ||   // Hangul Jamo
            (cp >= 0x2E80 && cp <= 0xA4CF) ||   // CJK … Yi
            (cp >= 0xAC00 && cp <= 0xD7A3) ||   // Hangul syllables
            (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK compat
            (cp >= 0xFE30 && cp <= 0xFE4F) ||   // CJK compat forms
            (cp >= 0xFF00 && cp <= 0xFF60) ||   // Fullwidth forms
            (cp >= 0xFFE0 && cp <= 0xFFE6) ||
            (cp >= 0x1F300 && cp <= 0x1FAFF) || // emoji & symbols
            (cp >= 0x20000 && cp <= 0x3FFFD))   // CJK ext B+
            return 2;
        return 1;
    }

    // Display width of a UTF-8 string, in cells.
    [[nodiscard]] static int text_width(std::string_view utf8) noexcept {
        int w = 0;
        std::size_t i = 0;
        while (i < utf8.size()) {
            char32_t cp = 0;
            i += decode_utf8(utf8, i, cp);
            w += cell_width(cp);
        }
        return w;
    }

private:
    struct Box { char32_t tl, tr, bl, br, h, v; };
    [[nodiscard]] static Box box_chars(BoxStyle bs) noexcept {
        switch (bs) {
        case BoxStyle::Heavy:   return {U'┏', U'┓', U'┗', U'┛', U'━', U'┃'};
        case BoxStyle::Double:  return {U'╔', U'╗', U'╚', U'╝', U'═', U'║'};
        case BoxStyle::Rounded: return {U'╭', U'╮', U'╰', U'╯', U'─', U'│'};
        case BoxStyle::Light:
        default:                return {U'┌', U'┐', U'└', U'┘', U'─', U'│'};
        }
    }

    // Decode one UTF-8 scalar at `i`; write to `cp`, return bytes consumed (>=1).
    [[nodiscard]] static std::size_t decode_utf8(std::string_view s, std::size_t i, char32_t &cp) noexcept {
        const unsigned char b0 = static_cast<unsigned char>(s[i]);
        if (b0 < 0x80) { cp = b0; return 1; }
        auto cont = [&](std::size_t k) {
            return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80;
        };
        if ((b0 & 0xE0) == 0xC0 && cont(1)) {
            cp = ((b0 & 0x1F) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3F);
            return 2;
        }
        if ((b0 & 0xF0) == 0xE0 && cont(1) && cont(2)) {
            cp = ((b0 & 0x0F) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3F);
            return 3;
        }
        if ((b0 & 0xF8) == 0xF0 && cont(1) && cont(2) && cont(3)) {
            cp = ((b0 & 0x07) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(s[i + 3]) & 0x3F);
            return 4;
        }
        cp = 0xFFFD;
        return 1;
    }

    int w_ = 0, h_ = 0;
    std::vector<Cell> cells_;
};

} // namespace glyph

#endif // HAND_GLYPH_BUFFER_HPP
