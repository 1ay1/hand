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
#include "hand/tab_model.hpp"
#include "hand/workspace.hpp"

namespace hand {

using glyph::Rgb;
using glyph::rgb;

// What a click on the chrome means. `tab_index` is the display-order index for
// ActivateTab/CloseTab; ignored otherwise.
struct ChromeHit {
    enum class Kind { None, ActivateTab, CloseTab, NewTab, WinMinimize, WinMaximize, WinClose };
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
            draw_tab(buf, y, x, tw, tab, is_focus, frame);
            hits_.push_back({{ChromeHit::Kind::ActivateTab, i}, x, x + tw - 1});
            // Close hit is the last cell of the tab (the × slot).
            hits_.push_back({{ChromeHit::Kind::CloseTab, i}, x + tw - 2, x + tw - 1});
            x += tw;
        });
    }

    // Map a click at cell (cx, cy) to a chrome action. cy must be the chrome row
    // (0). Returns Kind::None when the click isn't on any control.
    [[nodiscard]] ChromeHit hit_test(int cx, int cy) const {
        if (cy != 0) return {};
        // Later-drawn (more specific) regions win: iterate in reverse so a
        // CloseTab region (drawn after its ActivateTab) takes priority.
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (cx >= it->x0 && cx <= it->x1) return it->hit;
        }
        return {};
    }

private:
    struct Region {
        ChromeHit hit;
        int x0, x1; // inclusive cell range on row 0
    };

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

    void draw_tab(glyph::Buffer &buf, int y, int x, int tw, const Tab &tab, bool is_focus,
                  std::uint32_t frame) {
        // Colours: focused tab reads brighter; a done-attention tab pulses.
        const TabAttention att = tab.model.attention();
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
        switch (tab.model.status()) {
        case TabStatus::Ok: gcol = rgb(120, 210, 130); break;
        case TabStatus::Failed: gcol = rgb(230, 120, 120); break;
        case TabStatus::Running: gcol = rgb(220, 200, 120); break;
        default: break;
        }
        buf.put(cx, y, tab.model.glyph(frame), glyph::Style{.fg = gcol, .bg = bg});
        cx += 2;

        // Label, clipped to leave room for the unseen dot + close ×.
        const int label_w = tw - (cx - x) - 3;
        if (label_w > 0) cx += buf.text(cx, y, tab.model.label(), st, label_w);

        // Unseen-output dot just before the close ×.
        if (tab.model.unseen() && !is_focus) {
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
