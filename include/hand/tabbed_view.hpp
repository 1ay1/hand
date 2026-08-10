// SPDX-License-Identifier: LGPL-2.0-or-later
//
// TabbedView — the bridge where plat (window + GL context + frame) meets toe
// (per-tab Sessions) and hand's tab chrome. This is the ONE toe-aware piece of
// the compositing path; plat itself stays terminal-agnostic.
//
// Given a live plat::RenderPass (context-current + target framebuffer), it:
//   1. adopts it as a toe::RenderContext aimed at that framebuffer,
//   2. renders the ACTIVE tab's Session grid into it (toe's own renderer —
//      unchanged, uncoupled), and
//   3. composites the ChromeBar as a one-row overlay on top (the same
//      render_overlay path the settings/search panes use).
//
// Background tabs are advanced by Workspace::pump() elsewhere in the loop; only
// the active tab is drawn. N terminals, one window, toe's renderer intact.

#ifndef HAND_TABBED_VIEW_HPP
#define HAND_TABBED_VIEW_HPP

#include <algorithm>
#include <cstdint>

#include "hand/chrome_bar.hpp"
#include "hand/glyph/buffer.hpp"
#include "hand/workspace.hpp"
#include "plat/compositor.hpp"
#include "toe/gfx/render_target.hpp"
#include "toe/terminal.hpp"

namespace hand {

class TabbedView {
public:
    // Draw one frame of the workspace into the platform's render pass. `frame`
    // drives the chrome's spinner + attention pulse (a monotone counter the
    // host bumps ~per frame).
    void render(const plat::RenderPass &pass, Workspace &ws, std::uint32_t frame) {
        const toe::PixelSize px{pass.size().w, pass.size().h};

        // 1. The active tab's grid, via toe's renderer, into plat's framebuffer.
        //    Only a LIVE tab is drawn — with_live_focus enforces that by type.
        ws.with_live_focus([&](toe::Session &s, TabModel &) {
            auto rc = toe::gfx::RenderContext::adopt_current(
                toe::gfx::Framebuffer{pass.framebuffer});
            s.render(rc, px);
            draw_chrome(rc, s, ws, px, frame);
        });
    }

    // Expose the chrome's hit-test so the host can route a click on row 0.
    [[nodiscard]] ChromeHit hit_test(int cell_x, int cell_y) const {
        return chrome_.hit_test(cell_x, cell_y);
    }

private:
    // Composite the ChromeBar as a translucent one-row overlay on top of the
    // just-rendered terminal (same mechanism as the search bar: a cell buffer
    // fed to Session::render_overlay, transparent except the chrome row).
    void draw_chrome(toe::gfx::RenderContext &rc, toe::Session &s, Workspace &ws,
                     toe::PixelSize px, std::uint32_t frame) {
        const toe::Extent cell = s.cell_size();
        if (cell.cols <= 0 || cell.rows <= 0) return;
        const int cols = std::max(1, px.w / cell.cols);
        const int rows = std::max(1, px.h / cell.rows);
        if (buf_.width() != cols || buf_.height() != rows) buf_.resize(cols, rows);

        // Transparent everywhere except the chrome's rows (top), so the terminal
        // shows through beneath the bar.
        buf_.clear(glyph::Style{});
        buf_.clear_alpha(0);
        buf_.set_alpha({0, 0, cols, ChromeBar::kRows}, 255);

        chrome_.render(buf_, ws, frame);

        s.render_overlay(rc, buf_.data(), buf_.width(), buf_.height(), px, 0, 0, 1.0f,
                         buf_.alpha_data());
    }

    ChromeBar chrome_{};
    glyph::Buffer buf_{};
};

} // namespace hand

#endif // HAND_TABBED_VIEW_HPP
