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

    // A left-click landed on the rail while the flyout is up. If a command is
    // under the pointer, jump the view to it and return true (consumed).
    [[nodiscard]] bool click(toe::Session &s) {
        if (!active_ || hover_idx_ < 0 ||
            hover_idx_ >= static_cast<int>(items_.size()))
            return false;
        return s.jump_to_command(items_[static_cast<std::size_t>(hover_idx_)].id);
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
        const int list_rows = std::min(n_items, std::min(12, H - 5));
        const int card_h = list_rows + 3;               // header + divider + list + pad
        const int card_w = std::clamp(longest_text() + 16, 26, std::min(W - 8, 48));
        const int card_x = W - card_w - 4;              // leave room for the rail + notch

        // Scroll the list so the hovered row is centered in the window.
        const int center = hover_idx_ >= 0 ? hover_idx_ : n_items - 1;
        const int first = std::clamp(center - list_rows / 2, 0, std::max(0, n_items - list_rows));

        // Follow the mouse: center the card on the hovered rail pixel row.
        const int hover_y = static_cast<int>(hover_row_ * H /
                                             std::max<std::int64_t>(1, total_rows_));
        const int card_y = std::clamp(hover_y - card_h / 2, 1, std::max(1, H - card_h - 1));

        // Palette (self-contained; a dark frosted card with a cool accent).
        const glyph::Rgb bg   = glyph::rgb(24, 26, 36);
        const glyph::Rgb bg2  = glyph::rgb(30, 33, 46);   // header band
        const glyph::Rgb acc  = glyph::rgb(120, 165, 255);
        const glyph::Style body{glyph::rgb(224, 227, 238), bg};
        const glyph::Style border{glyph::rgb(70, 78, 110), bg};
        const glyph::Style dim{glyph::rgb(120, 125, 145), bg};

        buf.clear_alpha(0);
        const glyph::Rect card{card_x, card_y, card_w, card_h};
        buf.set_alpha(card, 250);
        // Two-step soft shadow down-right.
        buf.set_alpha(glyph::Rect{card_x + 1, card_y + card_h, card_w, 1}, 90);
        buf.set_alpha(glyph::Rect{card_x + 2, card_y + card_h + 1, card_w - 2, 1}, 45);
        buf.set_alpha(glyph::Rect{card_x + card_w, card_y + 1, 1, card_h}, 60);

        buf.fill(card, body);
        buf.frame(card, border, glyph::BoxStyle::Rounded);

        // Header band: title + count, then a divider rule.
        buf.fill(glyph::Rect{card_x + 1, card_y + 1, card_w - 2, 1}, glyph::Style{acc, bg2});
        buf.text(card_x + 2, card_y + 1, "COMMANDS", glyph::Style{acc, bg2});
        {
            const std::string cnt = std::to_string(n_items);
            buf.text(card_x + card_w - 2 - static_cast<int>(cnt.size()), card_y + 1, cnt,
                     glyph::Style{glyph::rgb(150, 156, 180), bg2});
        }
        // Divider under the header.
        for (int c = card_x + 1; c < card_x + card_w - 1; ++c)
            buf.put(c, card_y + 2, U'\u2500', border);

        const int list_y0 = card_y + 3;
        for (int r = 0; r < list_rows; ++r) {
            const int i = first + r;
            if (i >= n_items) break;
            const toe::CommandView &it = items_[static_cast<std::size_t>(i)];
            const int y = list_y0 + r;
            const bool hot = i == hover_idx_;

            const int status = !it.finished ? 0 : (it.succeeded() ? 1 : 2);
            const glyph::Rgb pc = status == 1 ? glyph::rgb(90, 214, 140)
                                : status == 2 ? glyph::rgb(240, 96, 96)
                                              : glyph::rgb(242, 194, 74);

            // Selected row: a status-tinted fill + bright left accent bar so it
            // pops, and a caret linking it to the connector notch on the right.
            const glyph::Rgb row_bg = hot ? blend(bg, pc, 0.22f) : bg;
            const glyph::Style rs{hot ? glyph::rgb(248, 249, 255) : body.fg, row_bg};
            buf.fill(glyph::Rect{card_x + 1, y, card_w - 2, 1}, rs);
            if (hot)
                buf.put(card_x + 1, y, U'\u2590', glyph::Style{pc, row_bg});

            // Status pip.
            const char32_t pip = status == 0 ? U'\u25D0' : U'\u25CF';
            buf.put(card_x + 3, y, pip, glyph::Style{pc, row_bg});

            // Command text, then a right-aligned duration in a muted tone.
            const std::string dur = it.duration_ms > 0 ? fmt_dur(it.duration_ms) : std::string{};
            const int dur_w = dur.empty() ? 0 : static_cast<int>(dur.size()) + 1;
            const int text_x = card_x + 5;
            const int text_max = card_w - 7 - dur_w;
            buf.text(text_x, y, first_line(it.command), rs, std::max(1, text_max));
            if (dur_w > 0)
                buf.text(card_x + card_w - 1 - static_cast<int>(dur.size()), y, dur,
                         hot ? glyph::Style{glyph::rgb(210, 214, 230), row_bg} : dim);
        }

        // Connector notch: a caret on the card's right edge at the hovered
        // row's height, pointing toward the rail so the card reads as attached
        // to the exact command under the pointer.
        if (hover_idx_ >= 0) {
            const int hy = std::clamp(list_y0 + (hover_idx_ - first), card_y + 1,
                                      card_y + card_h - 2);
            buf.put(card_x + card_w - 1, hy, U'\u25B8', glyph::Style{acc, bg});
            buf.put(card_x + card_w, hy, U'\u25B8', glyph::Style{acc, bg});
            buf.set_alpha(glyph::Rect{card_x + card_w, hy, 1, 1}, 250);
        }
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
