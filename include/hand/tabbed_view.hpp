// SPDX-License-Identifier: LGPL-2.0-or-later
//
// TabbedView — composites the active tab's grid + the tab chrome into the
// window. This is the one toe-aware drawing piece; the tab set comes from the
// GUI model (GuiModel's Zipper<TabEntry>), the pixels from toe's renderer, the
// chrome from ChromeBar drawn into an overlay cell buffer.
//
// The active tab's Session grid is rendered by the RUNTIME (it holds the
// render_lock); this class provides render_chrome(), which paints the ChromeBar
// from the GUI model over the just-drawn terminal via Session::render_overlay.

#ifndef HAND_TABBED_VIEW_HPP
#define HAND_TABBED_VIEW_HPP

#include <algorithm>
#include <cstdint>

#include "hand/chrome_bar.hpp"
#include "hand/glyph/buffer.hpp"
#include "hand/gui/model.hpp"
#include "toe/gfx/render_target.hpp"
#include "toe/terminal.hpp"

namespace hand {

class TabbedView {
public:
    // Paint the ChromeBar (built from the GUI model's tabs) as a translucent
    // top-row overlay over the already-rendered terminal grid of `s`.
    void render_chrome(toe::gfx::RenderContext &rc, toe::Session &s, const GuiModel &model,
                       toe::PixelSize px, std::uint32_t frame) {
        const toe::Extent cell = s.cell_size();
        if (cell.cols <= 0 || cell.rows <= 0) return;
        const int cols = std::max(1, px.w / cell.cols);
        const int rows = std::max(1, px.h / cell.rows);
        if (buf_.width() != cols || buf_.height() != rows) buf_.resize(cols, rows);

        buf_.clear(glyph::Style{});
        buf_.clear_alpha(0);
        buf_.set_alpha({0, 0, cols, ChromeBar::kRows}, 255);

        chrome_.render_model(buf_, model, frame);
        cell_w_ = cell.cols;
        cell_h_ = cell.rows;

        s.render_overlay(rc, buf_.data(), buf_.width(), buf_.height(), px, 0, 0, 1.0f,
                         buf_.alpha_data());
    }

    [[nodiscard]] ChromeHit hit_test(int cell_x, int cell_y) const {
        return chrome_.hit_test(cell_x, cell_y);
    }

    // Resolve a PIXEL click to a chrome action using the last-rendered cell
    // size. Returns Kind::None when the click isn't on the chrome row.
    [[nodiscard]] ChromeHit hit_test_px(int px_x, int px_y) const {
        if (cell_w_ <= 0 || cell_h_ <= 0) return {};
        return chrome_.hit_test(px_x / cell_w_, px_y / cell_h_);
    }

private:
    ChromeBar chrome_{};
    glyph::Buffer buf_{};
    int cell_w_ = 0, cell_h_ = 0;
};

} // namespace hand

#endif // HAND_TABBED_VIEW_HPP
