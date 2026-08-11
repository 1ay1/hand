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

    // A left-click landed on the rail while the flyout is up. If a command is
    // under the pointer, jump the view to it and return true (consumed).
    [[nodiscard]] bool click(toe::Session &s) {
        if (!active_ || hover_idx_ < 0 ||
            hover_idx_ >= static_cast<int>(items_.size()))
            return false;
        return s.jump_to_command(items_[static_cast<std::size_t>(hover_idx_)].id);
    }

    // Paint the flyout card into `buf` (sized to the terminal grid in cells).
    // Floats against the right edge just left of the rail, vertically centered
    // on the hovered row. Everything outside the card is transparent (alpha 0).
    void render(glyph::Buffer &buf) const {
        if (!active() ) return;
        const int W = buf.width(), H = buf.height();
        if (W < 26 || H < 4) return;

        const int n_items = static_cast<int>(items_.size());
        const int max_rows = std::min(n_items, std::min(14, H - 2));
        const int card_h = max_rows + 2;
        const int card_w = std::clamp(longest_text() + 15, 24, W - 6);
        const int card_x = W - card_w - 3;             // just left of the rail

        // Scroll the window so the hovered row is visible & roughly centered.
        const int center = hover_idx_ >= 0 ? hover_idx_ : n_items - 1;
        const int first = std::clamp(center - max_rows / 2, 0, std::max(0, n_items - max_rows));

        // Vertical placement: PINNED near the top so the card doesn't jitter
        // around while you slide along the rail — only the highlighted row and
        // the scroll window change. Nudge down only if it would clip the top.
        const int card_y = std::clamp(2, 1, std::max(1, H - card_h - 1));

        const glyph::Rgb bg = glyph::rgb(26, 28, 38);
        buf.clear_alpha(0);
        const glyph::Rect card{card_x, card_y, card_w, card_h};
        buf.set_alpha(card, 246);
        buf.set_alpha(glyph::Rect{card_x + 1, card_y + card_h, card_w, 1}, 80); // shadow

        const glyph::Style body{glyph::rgb(226, 228, 238), bg};
        const glyph::Style border{glyph::rgb(92, 100, 138), bg};
        const glyph::Style dim{glyph::rgb(128, 132, 150), bg};
        buf.fill(card, body);
        buf.frame(card, border, glyph::BoxStyle::Rounded);
        buf.text(card_x + 2, card_y, " Commands ",
                 glyph::Style{glyph::rgb(150, 180, 255), bg});
        // A caret hinting the list belongs to the rail on the right.
        buf.put(card_x + card_w, card_y + card_h / 2, U'\u25B8',
                glyph::Style{glyph::rgb(150, 180, 255), bg});

        for (int r = 0; r < max_rows; ++r) {
            const int i = first + r;
            if (i >= n_items) break;
            const toe::CommandView &it = items_[static_cast<std::size_t>(i)];
            const int y = card_y + 1 + r;
            const bool hot = i == hover_idx_;
            const glyph::Style rs =
                hot ? glyph::Style{glyph::rgb(245, 247, 255), glyph::rgb(56, 66, 104)} : body;
            if (hot) {
                buf.fill(glyph::Rect{card_x + 1, y, card_w - 2, 1}, rs);
                // Bright left accent bar + caret so the hovered command reads as
                // SELECTED, not merely tinted.
                buf.put(card_x + 1, y, U'\u2590',
                        glyph::Style{glyph::rgb(120, 170, 255), rs.bg});
            }

            const int status = !it.finished ? 0 : (it.succeeded() ? 1 : 2);
            const char32_t pip = status == 0 ? U'\u25D0' : U'\u25CF';
            const glyph::Rgb pc = status == 1 ? glyph::rgb(90, 210, 140)
                                : status == 2 ? glyph::rgb(235, 95, 95)
                                              : glyph::rgb(240, 190, 70);
            buf.put(card_x + 2, y, pip, glyph::Style{pc, rs.bg});

            const std::string dur = it.duration_ms > 0 ? fmt_dur(it.duration_ms) : std::string{};
            const int dur_w = static_cast<int>(dur.size());
            const int text_x = card_x + 4;
            const int text_max = card_w - 6 - dur_w;
            buf.text(text_x, y, first_line(it.command), rs, std::max(1, text_max));
            if (dur_w > 0)
                buf.text(card_x + card_w - 1 - dur_w, y, dur, hot ? rs : dim);
        }
    }

private:
    [[nodiscard]] int longest_text() const {
        int m = 0;
        for (const auto &it : items_)
            m = std::max(m, glyph::Buffer::text_width(first_line(it.command)));
        return std::min(m, 56);
    }
    static std::string first_line(const std::string &s) {
        const auto nl = s.find('\n');
        return nl == std::string::npos ? s : s.substr(0, nl);
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
