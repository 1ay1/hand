// SPDX-License-Identifier: LGPL-2.0-or-later
//
// CommandFlyout — the command-minimap's hover companion. When the pointer is
// over the right-edge rail, this paints a floating card listing the recent
// shell commands (OSC 133 blocks): each row shows a status pip, the command
// line, and its duration. The command under the pointer's rail row is
// highlighted; clicking it jumps the view to that command block.
//
// It's pure host UI: it reads toe::CommandView (already resolved by the engine,
// now carrying each block's absolute-row span) and drives Session::jump — the
// renderer knows nothing about it. Built on the same glyph immediate-mode
// canvas as the help/search panes and composited via Session::render_overlay,
// so it looks native on every backend.

#ifndef HAND_COMMAND_FLYOUT_HPP
#define HAND_COMMAND_FLYOUT_HPP

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "hand/glyph/buffer.hpp"
#include "hand/gui/host_config.hpp"
#include "toe/terminal.hpp"

namespace hand {

class CommandFlyout {
public:
    CommandFlyout() = default;

    [[nodiscard]] bool active() const noexcept { return active_ && !items_.empty(); }

    // Refresh from the session's command log. Reads the rail-hover row the
    // engine already tracks (rail_hover() sets it from mouse-move). <0 = the
    // pointer isn't on the rail -> hide. Cheap; only rebuilds while hovered.
    void update(const toe::Session &s) {
        const std::int64_t hover = s.rail_hover_row();
        if (hover < 0) { active_ = false; return; }
        active_ = true;
        hover_row_ = hover;
        total_rows_ = std::max<std::int64_t>(1, s.total_rows());
        items_ = s.commands();
        // Drop empty/prompt-only entries so the list reads clean.
        items_.erase(std::remove_if(items_.begin(), items_.end(),
                                    [](const toe::CommandView &c) { return c.command.empty(); }),
                     items_.end());
        // Which listed command is under the pointer? Use the NEAREST command
        // whose prompt is at or above the hovered rail row (like a scrollbar
        // thumb landing between ticks) so every rail position selects something.
        hover_idx_ = pick_hover(items_, hover_row_);
    }

    // Pure selection rule (testable): the index of the nearest command whose
    // prompt_row is <= `row`; if `row` is above all of them, the first command.
    // -1 only when there are no row-bearing commands.
    [[nodiscard]] static int pick_hover(const std::vector<toe::CommandView> &items,
                                        std::int64_t row) noexcept {
        int idx = -1;
        std::int64_t best_row = -1;
        for (std::size_t i = 0; i < items.size(); ++i) {
            const std::int64_t pr = items[i].prompt_row;
            if (pr < 0) continue;
            if (pr <= row && pr >= best_row) { best_row = pr; idx = static_cast<int>(i); }
        }
        if (idx < 0)
            for (std::size_t i = 0; i < items.size(); ++i)
                if (items[i].prompt_row >= 0) { idx = static_cast<int>(i); break; }
        return idx;
    }

    void hide() noexcept { active_ = false; }

    // Test seam: inject list + hover state directly (bypassing a live Session)
    // so the RENDER path can be exercised in isolation.
    void set_state_for_test(std::vector<toe::CommandView> items, std::int64_t hover_row,
                            std::int64_t total_rows) {
        items_ = std::move(items);
        hover_row_ = hover_row;
        total_rows_ = std::max<std::int64_t>(1, total_rows);
        hover_idx_ = pick_hover(items_, hover_row_);
        active_ = true;
    }

    // A left-click on the rail: scroll the view so the START of the command
    // under the pointer is at the top of the viewport. Read-only scroll (no
    // focus/gutter side effects). Returns true if it scrolled.
    [[nodiscard]] bool click(toe::Session &s) {
        if (!active_ || hover_idx_ < 0 ||
            hover_idx_ >= static_cast<int>(items_.size()))
            return false;
        const std::int64_t row = items_[static_cast<std::size_t>(hover_idx_)].prompt_row;
        if (row < 0) return false;
        return s.scroll_to_row(row);
    }

    // Paint the flyout card into `buf` (sized to the terminal grid in cells).
    // Floats against the right edge just left of the rail and FOLLOWS the
    // pointer vertically (centered on the hovered rail row), with a connector
    // notch pointing at that row. Everything outside the card is transparent.
    void render(glyph::Buffer &buf) const {
        if (!active()) return;
        const int W = buf.width(), H = buf.height();
        if (W < 28 || H < 6) return;

        const int n_items = static_cast<int>(items_.size());
        // Card chrome is 4 rows: top border, header, divider, bottom border.
        // The list occupies rows [card_y+3, card_y+3+list_rows), so card_h MUST
        // be list_rows+4 or the last item overwrites the bottom border (the
        // "list overflows the box" bug).
        const auto &hc = host_config();
        const int list_rows = std::min(n_items, std::max(1, H - 6));
        const int shown = std::min(list_rows, std::max(1, hc.flyout_rows));
        const int card_h = shown + 4;
        const int card_w = std::clamp(longest_text() + 15, 24,
                                      std::min({W - 6, std::max(24, hc.flyout_width)}));
        // Sit clear of the command rail on the right edge (the rail + its hover
        // expansion occupy the last few cells). Leave a 3-cell gap so the card
        // never overlaps it, and the pointer triangle bridges the gap.
        const int card_x = W - card_w - 3;

        const int center = hover_idx_ >= 0 ? hover_idx_ : n_items - 1;
        const int first = std::clamp(center - shown / 2, 0, std::max(0, n_items - shown));

        // Smart + STILL position: anchor the card on the hovered command's rail
        // position (quantised to the command, so it doesn't jitter per pixel as
        // you move within one command), then clamp fully on-screen. The pointer
        // triangle marks the exact hovered row, so the card can stay put while
        // only the highlight + scroll window move.
        const int anchor_row = hover_idx_ >= 0 && hover_idx_ < n_items
                                   ? static_cast<int>(items_[static_cast<std::size_t>(hover_idx_)]
                                                          .prompt_row *
                                                      H / std::max<std::int64_t>(1, total_rows_))
                                   : H / 2;
        const int card_y = std::clamp(anchor_row - 2, 0, std::max(0, H - card_h));

        // Palette: a dark frosted card with a cool accent.
        const glyph::Rgb bg     = glyph::rgb((hc.flyout_bg >> 16) & 0xFF,
                                             (hc.flyout_bg >> 8) & 0xFF, hc.flyout_bg & 0xFF);
        const glyph::Rgb bg_alt = glyph::rgb(28, 31, 42);   // zebra / header
        const glyph::Rgb acc    = glyph::rgb((hc.flyout_accent >> 16) & 0xFF,
                                             (hc.flyout_accent >> 8) & 0xFF,
                                             hc.flyout_accent & 0xFF);
        const glyph::Style body{glyph::rgb(220, 224, 236), bg};
        const glyph::Style border{glyph::rgb((hc.flyout_border >> 16) & 0xFF,
                                             (hc.flyout_border >> 8) & 0xFF,
                                             hc.flyout_border & 0xFF), bg};
        const glyph::Style dim{glyph::rgb(112, 118, 138), bg};

        buf.clear_alpha(0);
        const glyph::Rect card{card_x, card_y, card_w, card_h};
        // Fully opaque: a floating card must read as a SOLID panel regardless of
        // the window's translucency (a partial alpha let the translucent bg
        // bleed through and shift the card's colour). No drop shadow — a clean
        // flat card.
        buf.set_alpha(card, 255);

        buf.fill(card, body);
        buf.frame(card, border, glyph::BoxStyle::Rounded);

        // Header row: accent title + count on a slightly lighter band.
        buf.fill(glyph::Rect{card_x + 1, card_y + 1, card_w - 2, 1}, glyph::Style{acc, bg_alt});
        buf.text(card_x + 2, card_y + 1, "Commands",
                 glyph::Style{acc, bg_alt, glyph::Attr::Bold});
        {
            const std::string cnt = std::to_string(n_items);
            buf.text(card_x + card_w - 2 - static_cast<int>(cnt.size()), card_y + 1, cnt,
                     glyph::Style{glyph::rgb(140, 146, 168), bg_alt});
        }
        // Divider tucked between the header band and the list, joined to the frame.
        buf.put(card_x, card_y + 2, U'\u251C', border);
        for (int c = card_x + 1; c < card_x + card_w - 1; ++c)
            buf.put(c, card_y + 2, U'\u2500', border);
        buf.put(card_x + card_w - 1, card_y + 2, U'\u2524', border);

        const int list_y0 = card_y + 3;
        for (int r = 0; r < shown; ++r) {
            const int i = first + r;
            if (i >= n_items) break;
            const toe::CommandView &it = items_[static_cast<std::size_t>(i)];
            const int y = list_y0 + r;
            const bool hot = i == hover_idx_;

            const int status = !it.finished ? 0 : (it.succeeded() ? 1 : 2);
            const glyph::Rgb pc = status == 1 ? glyph::rgb(96, 216, 148)
                                : status == 2 ? glyph::rgb(242, 100, 100)
                                              : glyph::rgb(244, 198, 82);

            // Row background: hovered = a strong status-tinted fill edge-to-edge
            // with a bright left accent bar; others = subtle zebra striping.
            const glyph::Rgb row_bg =
                hot ? blend(bg, pc, 0.30f) : (r & 1 ? bg_alt : bg);
            const glyph::Style rs{hot ? glyph::rgb(250, 251, 255) : body.fg, row_bg};
            buf.fill(glyph::Rect{card_x + 1, y, card_w - 2, 1}, rs);
            buf.put(card_x + 1, y, hot ? U'\u2590' : U' ',
                    glyph::Style{hot ? pc : row_bg, row_bg});

            // Status pip.
            const char32_t pip = status == 0 ? U'\u25D0' : U'\u25CF';
            buf.put(card_x + 3, y, pip, glyph::Style{pc, row_bg});

            // Command text, then a right-aligned duration in a muted tone.
            const std::string dur = it.duration_ms > 0 ? fmt_dur(it.duration_ms) : std::string{};
            const int dur_w = dur.empty() ? 0 : static_cast<int>(dur.size()) + 1;
            buf.text(card_x + 5, y, first_line(it.command), rs,
                     std::max(1, card_w - 7 - dur_w));
            if (dur_w > 0)
                buf.text(card_x + card_w - 1 - static_cast<int>(dur.size()), y, dur,
                         hot ? glyph::Style{glyph::rgb(220, 224, 236), row_bg} : dim);
        }

        // Pointer triangle: replaces the right border cell at the hovered
        // row's height so the card reads as pinned to that command (points at
        // the rail). Coloured by the accent.
        if (hover_idx_ >= 0) {
            const int hy = std::clamp(list_y0 + (hover_idx_ - first), card_y + 1,
                                      card_y + card_h - 2);
            buf.put(card_x + card_w - 1, hy, U'\u25B6', glyph::Style{acc, bg});
        }

        // Scroll affordances: chevrons in the divider / bottom border when the
        // list is windowed, so it's clear there's more above or below.
        const int mid = card_x + card_w / 2;
        if (first > 0)
            buf.put(mid, card_y + 2, U'\u25B4', glyph::Style{acc, bg}); // ▴ more above
        if (first + shown < n_items)
            buf.put(mid, card_y + card_h - 1, U'\u25BE', glyph::Style{acc, bg}); // ▾ more below
    }

private:
    [[nodiscard]] int longest_text() const {
        int m = 0;
        for (const auto &it : items_)
            m = std::max(m, glyph::Buffer::text_width(first_line(it.command)));
        return std::min(m, 30);
    }
    static std::string first_line(const std::string &s) {
        const auto nl = s.find('\n');
        return nl == std::string::npos ? s : s.substr(0, nl);
    }
    // Linear blend a->b by t in [0,1] (self-contained; no Theme dependency).
    static glyph::Rgb blend(glyph::Rgb a, glyph::Rgb b, float t) noexcept {
        const auto mix1 = [t](std::uint8_t x, std::uint8_t y) {
            return static_cast<std::uint8_t>(x + (static_cast<int>(y) - x) * t);
        };
        return glyph::rgb(mix1(a.r, b.r), mix1(a.g, b.g), mix1(a.b, b.b));
    }
    static std::string fmt_dur(std::int64_t ms) {
        if (ms < 1000) return std::to_string(ms) + "ms";
        if (ms < 60000) {
            char b[16];
            std::snprintf(b, sizeof b, "%.1fs", static_cast<double>(ms) / 1000.0);
            return b;
        }
        return std::to_string(ms / 60000) + "m" + std::to_string((ms % 60000) / 1000) + "s";
    }

    bool active_ = false;
    std::int64_t hover_row_ = -1;
    std::int64_t total_rows_ = 1;
    int hover_idx_ = -1;
    std::vector<toe::CommandView> items_;
};

} // namespace hand

#endif // HAND_COMMAND_FLYOUT_HPP
