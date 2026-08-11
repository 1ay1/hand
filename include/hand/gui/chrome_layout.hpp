// SPDX-License-Identifier: LGPL-2.0-or-later
//
// ChromeLayout — the ONE place that knows where the tab bar sits and how the
// terminal viewport is inset from it. Everything geometric (the GL viewport for
// the grid, translating a window pixel into terminal-grid space, and the chrome
// rect in cells) is derived here, so run_tabbed doesn't sprinkle edge math for
// each of the four placements (top / bottom / left / right).
//
// Cell coordinates: the chrome is painted into the FULL-window overlay buffer
// (cols x rows), so its rect is in cells. The terminal viewport is in PIXELS
// (GL, origin bottom-left).

#ifndef HAND_GUI_CHROME_LAYOUT_HPP
#define HAND_GUI_CHROME_LAYOUT_HPP

#include <algorithm>
#include <string_view>

namespace hand {

enum class ChromeSide { Top, Bottom, Left, Right };

inline ChromeSide chrome_side_from(std::string_view s) {
    if (s == "bottom") return ChromeSide::Bottom;
    if (s == "left") return ChromeSide::Left;
    if (s == "right") return ChromeSide::Right;
    return ChromeSide::Top;
}

// A rect in cells (chrome) or pixels (viewport). x,y is top-left.
struct RectC { int x = 0, y = 0, w = 0, h = 0; };

class ChromeLayout {
public:
    // Recompute from the current window size + cell size. `thickness_cells` is
    // the tab-bar thickness: rows for top/bottom, columns for left/right (auto,
    // clamped by side_width for vertical). Safe with zero cell size.
    void set(int win_w_px, int win_h_px, int cell_w_px, int cell_h_px, ChromeSide side,
             int side_width_cells, bool hidden = false) {
        win_w_px_ = std::max(1, win_w_px);
        win_h_px_ = std::max(1, win_h_px);
        cw_ = std::max(1, cell_w_px);
        ch_ = std::max(1, cell_h_px);
        side_ = side;
        hidden_ = hidden;
        cols_ = std::max(1, win_w_px_ / cw_);
        rows_ = std::max(1, win_h_px_ / ch_);

        if (hidden_) {
            // Auto-hide: the terminal gets the WHOLE window; no strip reserved.
            // Only a small window-controls patch floats in the top-right corner
            // ("– □ ✕" = ~7 cells). The tab bar itself isn't drawn.
            thick_cells_ = 0;
            term_px_w_ = win_w_px_;
            term_px_h_ = win_h_px_;
            term_origin_x_px_ = 0;
            term_origin_y_px_ = 0;
            ctrl_ = {std::max(0, cols_ - 8), 0, 8, 1};
            chrome_ = {0, 0, 0, 0};
            return;
        }
        ctrl_ = {0, 0, 0, 0};

        if (side_ == ChromeSide::Top || side_ == ChromeSide::Bottom) {
            thick_cells_ = 1; // one row
            const int strip_px = thick_cells_ * ch_;
            term_px_w_ = win_w_px_;
            term_px_h_ = std::max(1, win_h_px_ - strip_px);
            // Chrome cell rect (full width, 1 row) at the chosen edge.
            chrome_ = {0, side_ == ChromeSide::Top ? 0 : rows_ - 1, cols_, 1};
            // Terminal pixel origin (top-left in window space): shifted down by
            // the strip when the bar is on top.
            term_origin_x_px_ = 0;
            term_origin_y_px_ = (side_ == ChromeSide::Top) ? strip_px : 0;
        } else {
            // Vertical bar: width auto-clamped to side_width, min 8 cells.
            thick_cells_ = std::clamp(side_width_cells, 8, std::max(8, cols_ - 8));
            const int strip_px = thick_cells_ * cw_;
            term_px_w_ = std::max(1, win_w_px_ - strip_px);
            term_px_h_ = win_h_px_;
            chrome_ = {side_ == ChromeSide::Left ? 0 : cols_ - thick_cells_, 0, thick_cells_, rows_};
            term_origin_x_px_ = (side_ == ChromeSide::Left) ? strip_px : 0;
            term_origin_y_px_ = 0;
        }
    }

    [[nodiscard]] ChromeSide side() const noexcept { return side_; }
    [[nodiscard]] bool hidden() const noexcept { return hidden_; }
    // The floating window-controls cell rect (top-right) when the bar is hidden.
    [[nodiscard]] RectC ctrl_cells() const noexcept { return ctrl_; }
    [[nodiscard]] bool vertical() const noexcept {
        return side_ == ChromeSide::Left || side_ == ChromeSide::Right;
    }
    [[nodiscard]] int cols() const noexcept { return cols_; }
    [[nodiscard]] int rows() const noexcept { return rows_; }
    [[nodiscard]] RectC chrome_cells() const noexcept { return chrome_; }

    // Terminal viewport in PIXELS (size the grid renders into).
    [[nodiscard]] int term_px_w() const noexcept { return term_px_w_; }
    [[nodiscard]] int term_px_h() const noexcept { return term_px_h_; }
    // GL viewport y (origin bottom-left): the strip is above the grid only when
    // the bar is on TOP; for bottom/left/right the grid touches y=0.
    [[nodiscard]] int gl_viewport_x() const noexcept { return term_origin_x_px_; }
    [[nodiscard]] int gl_viewport_y() const noexcept {
        // window-space top-left origin -> GL bottom-left: for a TOP bar the grid
        // sits at pixel y=strip from the top, i.e. y=0 from the bottom (its
        // height already excludes the strip). For a BOTTOM bar the grid sits
        // above the strip -> GL y = strip.
        return (side_ == ChromeSide::Bottom) ? (win_h_px_ - term_px_h_) : 0;
    }

    // Is a window pixel inside the CHROME strip? (for pointer routing / cursor)
    [[nodiscard]] bool on_chrome_px(int px_x, int px_y) const noexcept {
        if (hidden_) {
            // Only the small top-right controls patch counts as chrome.
            return px_x >= ctrl_.x * cw_ && px_x < (ctrl_.x + ctrl_.w) * cw_ &&
                   px_y < ctrl_.h * ch_;
        }
        switch (side_) {
        case ChromeSide::Top:    return px_y < thick_cells_ * ch_;
        case ChromeSide::Bottom: return px_y >= win_h_px_ - thick_cells_ * ch_;
        case ChromeSide::Left:   return px_x < thick_cells_ * cw_;
        case ChromeSide::Right:  return px_x >= win_w_px_ - thick_cells_ * cw_;
        }
        return false;
    }

    // Translate a WINDOW pixel into TERMINAL-grid pixel space (subtract the
    // strip origin). Used before feeding a pointer event to the EventRouter.
    [[nodiscard]] int to_term_px_x(int px_x) const noexcept { return px_x - term_origin_x_px_; }
    [[nodiscard]] int to_term_px_y(int px_y) const noexcept { return px_y - term_origin_y_px_; }
    [[nodiscard]] int term_origin_x_px() const noexcept { return term_origin_x_px_; }
    [[nodiscard]] int term_origin_y_px() const noexcept { return term_origin_y_px_; }

private:
    int win_w_px_ = 1, win_h_px_ = 1, cw_ = 1, ch_ = 1;
    int cols_ = 1, rows_ = 1, thick_cells_ = 1;
    ChromeSide side_ = ChromeSide::Top;
    bool hidden_ = false;
    RectC chrome_{};
    RectC ctrl_{}; // top-right window-controls patch when hidden
    int term_px_w_ = 1, term_px_h_ = 1;
    int term_origin_x_px_ = 0, term_origin_y_px_ = 0;
};

} // namespace hand

#endif // HAND_GUI_CHROME_LAYOUT_HPP
