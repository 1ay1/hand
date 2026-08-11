// SPDX-License-Identifier: LGPL-2.0-or-later
//
// ChromeBar — hand's client-side window chrome: a single top row that replaces
// the OS titlebar with hand's own Activity Tabs + window controls. Rendered
// into a glyph::Buffer (the same immediate-mode canvas the settings/search
// overlays use) and composited via Session::render_overlay, so it looks native
// on every backend and costs no new GL code.
//
// Anatomy of the row (all in terminal CELL coordinates):
//
//   │ ▸ npm test 3s  api ● ✓ make …│                        ─  □  ✕
//   └── tabs, each: [status glyph] [label] [unseen dot] [close ×]        └ window controls
//
// The bar is PURE: it takes a Workspace snapshot + a spinner frame and produces
// (a) painted cells and (b) a list of hit regions, so the host can route a
// click to "activate tab k", "close tab k", or a window control. It never
// touches the engine or the window — the host wires the actions.

#ifndef HAND_CHROME_BAR_HPP
#define HAND_CHROME_BAR_HPP

#include <cstdint>
#include <string>
#include <vector>

#include "hand/glyph/buffer.hpp"
#include "hand/gui/chrome_layout.hpp"
#include "hand/tab_model.hpp"
#include "hand/workspace.hpp"

namespace hand {

using glyph::Rgb;
using glyph::rgb;

// What a click on the chrome means. `tab_index` is the display-order index for
// ActivateTab/CloseTab; ignored otherwise.
struct ChromeHit {
    enum class Kind { None, ActivateTab, CloseTab, NewTab, WinMinimize, WinMaximize, WinClose, ScrollLeft, ScrollRight };
    Kind kind = Kind::None;
    std::size_t tab_index = 0;
};

class ChromeBar {
public:
    // The chrome occupies the top ROW of the grid (row 0). The terminal content
    // is offset down by this many rows by the host.
    static constexpr int kRows = 1;

    // Paint the chrome for `ws` into row 0 of `buf`. `frame` drives the running
    // spinner + the done-attention pulse. Records hit regions for click routing.
    void render(glyph::Buffer &buf, const Workspace &ws, std::uint32_t frame) {
        hits_.clear();
        const int w = buf.width();
        if (w <= 0 || buf.height() <= 0) return;
        const int y = 0;

        // Base bar.
        const glyph::Style base{.fg = rgb(150, 150, 165), .bg = rgb(24, 24, 32)};
        buf.fill({0, y, w, 1}, base);

        // Reserve the right end for window controls:  ─  □  ✕  (3 cells + gaps).
        // Layout: "  \u2500  \u25A1  \u2715 " -> we place them at fixed offsets from the right.
        const int ctrl_w = 9; // "  -  o  x " padded
        const int ctrl_x = w - ctrl_w;
        draw_window_controls(buf, y, ctrl_x);

        // A "+" new-tab button just left of the controls.
        const int plus_x = ctrl_x - 3;
        if (plus_x > 2) {
            const glyph::Style plus{.fg = rgb(150, 150, 165), .bg = rgb(24, 24, 32)};
            buf.put(plus_x + 1, y, U'+', plus);
            hits_.push_back({{ChromeHit::Kind::NewTab, 0}, plus_x, plus_x + 3});
        }

        // Tabs fill the space from x=0 up to plus_x. Each tab gets an equal
        // share, clamped to a sensible max width; labels are clipped.
        const int avail = (plus_x > 0 ? plus_x : w) - 1;
        const std::size_t n = ws.count();
        if (n == 0) return;
        const int per = std::max(8, std::min(28, avail / static_cast<int>(n)));

        int x = 0;
        ws.for_each_tab([&](const Tab &tab, bool is_focus, std::size_t i) {
            if (x + 4 > avail) return; // out of room; drop overflow tabs
            const int tw = std::min(per, avail - x);
            draw_tab(buf, y, x, tw, tab.model, is_focus, frame);
            hits_.push_back({{ChromeHit::Kind::ActivateTab, i}, x, x + tw - 1});
            // Close hit is the last cell of the tab (the × slot).
            hits_.push_back({{ChromeHit::Kind::CloseTab, i}, x + tw - 2, x + tw - 1});
            x += tw;
        });
    }

    // Render the chrome from a GUI model whose tabs expose for_each_ordered
    // yielding entries with a `.model` TabModel. Templated to avoid a header
    // dependency on gui/model.hpp (keeps chrome_bar reusable).
    template <class Model>
    void render_model(glyph::Buffer &buf, const Model &m, std::uint32_t frame) {
        hits_.clear();
        const int w = buf.width();
        if (w <= 0 || buf.height() <= 0) return;
        const int y = 0;
        const glyph::Style base{.fg = rgb(150, 150, 165), .bg = rgb(24, 24, 32)};
        buf.fill({0, y, w, 1}, base);

        const int ctrl_w = 9;
        const int ctrl_x = w - ctrl_w;
        draw_window_controls(buf, y, ctrl_x);

        const int plus_x = ctrl_x - 3;
        if (plus_x > 2) {
            buf.put(plus_x + 1, y, U'+', base);
            hits_.push_back({{ChromeHit::Kind::NewTab, 0}, plus_x, plus_x + 3});
        }

        const int avail = (plus_x > 0 ? plus_x : w) - 1;
        const std::size_t n = m.tabs().size();
        if (n == 0) return;
        const int per = std::max(8, std::min(28, avail / static_cast<int>(n)));

        int x = 0;
        m.tabs().for_each_ordered([&](const auto &entry, bool is_focus, std::size_t i) {
            if (x + 4 > avail) return;
            const int tw = std::min(per, avail - x);
            draw_tab(buf, y, x, tw, entry.model, is_focus, frame);
            hits_.push_back({{ChromeHit::Kind::ActivateTab, i}, x, x + tw - 1});
            hits_.push_back({{ChromeHit::Kind::CloseTab, i}, x + tw - 2, x + tw - 1});
            x += tw;
        });
    }

    // Map a click at cell (cx, cy) to a chrome action. cy must be the chrome row
    // (0). Returns Kind::None when the click isn't on any control.
    [[nodiscard]] ChromeHit hit_test(int cx, int cy) const {
        // Later-drawn (more specific) regions win: iterate in reverse so a
        // CloseTab region (drawn after its ActivateTab) takes priority.
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (cx >= it->x0 && cx <= it->x1 && cy >= it->y0 && cy <= it->y1) return it->hit;
        }
        return {};
    }

    // === UNIFIED oriented render (top/bottom/left/right + overflow scroll) ===
    // Paint the tab bar into `r` (a cell rect) with the given placement. Handles
    // BOTH orientations and scrolls the tab strip when there are more tabs than
    // fit, always keeping the focused tab in view. `show_ctrls`/`show_plus`
    // toggle the window buttons and + button. Records 2-D hit regions.
    template <class Model>
    void render_oriented(glyph::Buffer &buf, const Model &m, std::uint32_t frame, RectC r,
                         ChromeSide side, bool show_ctrls, bool show_plus) {
        hits_.clear();
        if (r.w <= 0 || r.h <= 0) return;
        const bool vert = (side == ChromeSide::Left || side == ChromeSide::Right);
        const glyph::Style base{.fg = rgb(150, 150, 165), .bg = rgb(24, 24, 32)};
        buf.fill({r.x, r.y, r.w, r.h}, base);

        const std::size_t n = m.tabs().size();
        std::size_t focus_i = 0;
        m.tabs().for_each_ordered([&](const auto &, bool f, std::size_t i) { if (f) focus_i = i; });

        if (vert) render_vertical(buf, m, frame, r, side, show_ctrls, show_plus, n, focus_i);
        else      render_horizontal(buf, m, frame, r, side, show_ctrls, show_plus, n, focus_i);
    }

private:
    struct Region {
        ChromeHit hit;
        int x0, y0, x1, y1; // inclusive cell range
    };
    void reg(ChromeHit h, int x0, int y0, int x1, int y1) {
        hits_.push_back({h, x0, y0, x1, y1});
    }

    // --- horizontal (top/bottom) with overflow scroll --------------------
    template <class Model>
    void render_horizontal(glyph::Buffer &buf, const Model &m, std::uint32_t frame, RectC r,
                           ChromeSide, bool show_ctrls, bool show_plus, std::size_t n,
                           std::size_t focus_i) {
        const int y = r.y;
        const glyph::Style base{.fg = rgb(150, 150, 165), .bg = rgb(24, 24, 32)};
        int right = r.x + r.w;
        if (show_ctrls) { right -= 9; draw_window_controls(buf, y, right); }
        if (show_plus && right - 3 > r.x + 2) {
            const int plus_x = right - 3;
            buf.put(plus_x + 1, y, U'+', base);
            reg({ChromeHit::Kind::NewTab, 0}, plus_x, y, plus_x + 2, y);
            right = plus_x;
        }
        const int avail = right - r.x - 1;
        if (n == 0 || avail < 6) return;
        // Per-tab width: fit all if possible, else a readable min and SCROLL.
        const int per = std::clamp(avail / static_cast<int>(n), 10, 28);
        const int fit = std::max(1, avail / per);
        const std::size_t first = scroll_first(n, static_cast<std::size_t>(fit), focus_i);
        int x = r.x;
        // ‹ more-left indicator.
        if (first > 0) { buf.put(x, y, U'\u2039', base); reg({ChromeHit::Kind::ScrollLeft, 0}, x, y, x, y); x += 1; }
        std::size_t drawn = 0;
        m.tabs().for_each_ordered([&](const auto &e, bool foc, std::size_t i) {
            if (i < first || drawn >= static_cast<std::size_t>(fit)) return;
            const int tw = std::min(per, right - x - 1);
            if (tw < 6) return;
            draw_tab(buf, y, x, tw, e.model, foc, frame);
            reg({ChromeHit::Kind::ActivateTab, i}, x, y, x + tw - 1, y);
            reg({ChromeHit::Kind::CloseTab, i}, x + tw - 2, y, x + tw - 1, y);
            x += tw; ++drawn;
        });
        // › more-right indicator.
        if (first + static_cast<std::size_t>(fit) < n && x < right)
            { buf.put(x, y, U'\u203a', base); reg({ChromeHit::Kind::ScrollRight, 0}, x, y, x, y); }
    }

    // --- vertical (left/right): each tab is a ROW, scrolls on overflow -----
    template <class Model>
    void render_vertical(glyph::Buffer &buf, const Model &m, std::uint32_t frame, RectC r,
                         ChromeSide side, bool show_ctrls, bool show_plus, std::size_t n,
                         std::size_t focus_i) {
        const glyph::Style base{.fg = rgb(150, 150, 165), .bg = rgb(24, 24, 32)};
        // Reserve the bottom rows for + and window controls.
        int bottom = r.y + r.h;
        if (show_ctrls) { draw_window_controls_v(buf, r, bottom - 1); bottom -= 1; }
        if (show_plus && bottom - 1 > r.y) {
            const int py = bottom - 1;
            buf.fill({r.x, py, r.w, 1}, base);
            buf.put(r.x + 1, py, U'+', base);
            buf.text(r.x + 3, py, "new tab", glyph::Style{.fg = rgb(150,150,165), .bg = rgb(24,24,32)}, r.w - 4);
            reg({ChromeHit::Kind::NewTab, 0}, r.x, py, r.x + r.w - 1, py);
            bottom -= 1;
        }
        const int list_rows = bottom - r.y;
        if (n == 0 || list_rows < 1) return;
        const std::size_t fit = static_cast<std::size_t>(list_rows);
        // Reserve a row for each scroll chevron when overflowing.
        std::size_t vis = fit;
        const bool overflow = n > fit;
        if (overflow) vis = (fit >= 2) ? fit - 2 : fit; // top+bottom chevrons
        const std::size_t first = scroll_first(n, vis, focus_i);
        int yy = r.y;
        if (overflow) { buf.put(r.x + r.w / 2, yy, U'\u25B4', base); reg({ChromeHit::Kind::ScrollLeft, 0}, r.x, yy, r.x + r.w - 1, yy); yy += 1; }
        std::size_t drawn = 0;
        m.tabs().for_each_ordered([&](const auto &e, bool foc, std::size_t i) {
            if (i < first || drawn >= vis) return;
            draw_tab_v(buf, r, yy, e.model, foc, frame, side);
            reg({ChromeHit::Kind::ActivateTab, i}, r.x, yy, r.x + r.w - 1, yy);
            reg({ChromeHit::Kind::CloseTab, i}, r.x + r.w - 2, yy, r.x + r.w - 1, yy);
            yy += 1; ++drawn;
        });
        if (overflow && first + vis < n)
            { buf.put(r.x + r.w / 2, yy, U'\u25BE', base); reg({ChromeHit::Kind::ScrollRight, 0}, r.x, yy, r.x + r.w - 1, yy); }
    }

    // Keep the focused tab within the visible window of `fit` items.
    static std::size_t scroll_first(std::size_t n, std::size_t fit, std::size_t focus) {
        if (n <= fit) return 0;
        std::size_t first = (focus > fit / 2) ? focus - fit / 2 : 0;
        if (first + fit > n) first = n - fit;
        return first;
    }

    void draw_window_controls_v(glyph::Buffer &buf, RectC r, int y) {
        const glyph::Style base{.fg = rgb(150, 150, 165), .bg = rgb(24, 24, 32)};
        buf.fill({r.x, y, r.w, 1}, base);
        buf.put(r.x + 1, y, U'\u2013', glyph::Style{.fg = rgb(180,180,120), .bg = rgb(24,24,32)});
        buf.put(r.x + 3, y, U'\u25A1', glyph::Style{.fg = rgb(120,180,120), .bg = rgb(24,24,32)});
        buf.put(r.x + 5, y, U'\u2715', glyph::Style{.fg = rgb(210,110,110), .bg = rgb(24,24,32)});
        reg({ChromeHit::Kind::WinMinimize, 0}, r.x, y, r.x + 2, y);
        reg({ChromeHit::Kind::WinMaximize, 0}, r.x + 3, y, r.x + 4, y);
        reg({ChromeHit::Kind::WinClose, 0}, r.x + 5, y, r.x + r.w - 1, y);
    }

    void draw_tab_v(glyph::Buffer &buf, RectC r, int y, const TabModel &tm, bool is_focus,
                    std::uint32_t frame, ChromeSide side) {
        const TabAttention att = tm.attention();
        const bool pulse_on = (frame / 8) % 2 == 0;
        Rgb bg = is_focus ? rgb(44, 44, 58) : rgb(24, 24, 32);
        Rgb fg = is_focus ? rgb(235, 235, 240) : rgb(150, 150, 165);
        if (att == TabAttention::DoneOk && pulse_on) bg = rgb(30, 70, 40);
        if (att == TabAttention::DoneFail && pulse_on) bg = rgb(80, 34, 34);
        const glyph::Style st{.fg = fg, .bg = bg};
        buf.fill({r.x, y, r.w, 1}, st);
        // Accent bar on the inner edge of the focused tab.
        if (is_focus) {
            const int ax = (side == ChromeSide::Left) ? r.x : r.x + r.w - 1;
            buf.put(ax, y, U'\u2590', glyph::Style{.fg = rgb(120, 170, 255), .bg = bg});
        }
        int cx = r.x + 1;
        Rgb gcol = fg;
        switch (tm.status()) {
        case TabStatus::Ok: gcol = rgb(120, 210, 130); break;
        case TabStatus::Failed: gcol = rgb(230, 120, 120); break;
        case TabStatus::Running: gcol = rgb(220, 200, 120); break;
        default: break;
        }
        buf.put(cx, y, tm.glyph(frame), glyph::Style{.fg = gcol, .bg = bg});
        cx += 2;
        const int label_w = r.w - (cx - r.x) - 2;
        if (label_w > 0) buf.text(cx, y, tm.label(), st, label_w);
        if (tm.unseen() && !is_focus)
            buf.put(r.x + r.w - 2, y, U'\u2022', glyph::Style{.fg = rgb(120,170,230), .bg = bg});
        if (is_focus && r.w >= 4)
            buf.put(r.x + r.w - 2, y, U'\u2715', glyph::Style{.fg = rgb(180,120,120), .bg = bg});
    }

    void draw_window_controls(glyph::Buffer &buf, int y, int x0) {
        const glyph::Style min_s{.fg = rgb(180, 180, 120), .bg = rgb(24, 24, 32)};
        const glyph::Style max_s{.fg = rgb(120, 180, 120), .bg = rgb(24, 24, 32)};
        const glyph::Style cls_s{.fg = rgb(210, 110, 110), .bg = rgb(24, 24, 32)};
        int x = x0;
        buf.put(x + 1, y, U'\u2013', min_s); // – minimize
        hits_.push_back({{ChromeHit::Kind::WinMinimize, 0}, x, x + 2});
        x += 3;
        buf.put(x + 1, y, U'\u25A1', max_s); // □ maximize/restore
        hits_.push_back({{ChromeHit::Kind::WinMaximize, 0}, x, x + 2});
        x += 3;
        buf.put(x + 1, y, U'\u2715', cls_s); // ✕ close window
        hits_.push_back({{ChromeHit::Kind::WinClose, 0}, x, x + 2});
    }

    void draw_tab(glyph::Buffer &buf, int y, int x, int tw, const TabModel &tm, bool is_focus,
                  std::uint32_t frame) {
        // Colours: focused tab reads brighter; a done-attention tab pulses.
        const TabAttention att = tm.attention();
        const bool pulse_on = (frame / 8) % 2 == 0; // ~slow blink
        Rgb bg = is_focus ? rgb(44, 44, 58) : rgb(24, 24, 32);
        Rgb fg = is_focus ? rgb(235, 235, 240) : rgb(150, 150, 165);
        if (att == TabAttention::DoneOk && pulse_on) bg = rgb(30, 70, 40);
        if (att == TabAttention::DoneFail && pulse_on) bg = rgb(80, 34, 34);
        const glyph::Style st{.fg = fg, .bg = bg};
        buf.fill({x, y, tw, 1}, st);

        int cx = x + 1;
        // Status glyph (spinner / ✓ / ✗ / ●), coloured by status.
        Rgb gcol = fg;
        switch (tm.status()) {
        case TabStatus::Ok: gcol = rgb(120, 210, 130); break;
        case TabStatus::Failed: gcol = rgb(230, 120, 120); break;
        case TabStatus::Running: gcol = rgb(220, 200, 120); break;
        default: break;
        }
        buf.put(cx, y, tm.glyph(frame), glyph::Style{.fg = gcol, .bg = bg});
        cx += 2;

        // Label, clipped to leave room for the unseen dot + close ×.
        const int label_w = tw - (cx - x) - 3;
        if (label_w > 0) cx += buf.text(cx, y, tm.label(), st, label_w);

        // Unseen-output dot just before the close ×.
        if (tm.unseen() && !is_focus) {
            buf.put(x + tw - 3, y, U'\u2022', glyph::Style{.fg = rgb(120, 170, 230), .bg = bg});
        }
        // Close × in the last cell (only shown on the focused tab or on hover;
        // here: always on the focused tab to keep it discoverable).
        if (is_focus && tw >= 4) {
            buf.put(x + tw - 2, y, U'\u2715', glyph::Style{.fg = rgb(180, 120, 120), .bg = bg});
        }
    }

    std::vector<Region> hits_;
};

} // namespace hand

#endif // HAND_CHROME_BAR_HPP
