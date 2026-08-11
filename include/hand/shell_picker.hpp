// SPDX-License-Identifier: LGPL-2.0-or-later
//
// ShellPicker — a small centred popup that lists the installed shells; pick one
// with ↑/↓ + Enter to open a NEW TAB running it. Ctrl+Shift+N opens it. Type to
// filter. Built on the glyph immediate-mode canvas, composited like the other
// overlays. It holds NO state beyond the open flag + cursor; the host reads
// chosen() after handle() returns 'accepted'.

#ifndef HAND_SHELL_PICKER_HPP
#define HAND_SHELL_PICKER_HPP

#include <algorithm>
#include <string>
#include <vector>

#include "hand/glyph/buffer.hpp"
#include "hand/platform/shells.hpp"
#include "toe/app.hpp"

namespace hand {

class ShellPicker {
public:
    [[nodiscard]] bool active() const noexcept { return active_; }

    void open() {
        shells_ = installed_shells();
        active_ = true;
        sel_ = 0;
        filter_.clear();
    }
    void close() { active_ = false; chosen_.clear(); }

    // The shell path the user accepted (empty until they hit Enter). Consumed
    // by the host to spawn a tab, then cleared on the next open.
    [[nodiscard]] const std::string &chosen() const noexcept { return chosen_; }

    enum class Result { Ignored, Consumed, Accepted, Cancelled };

    // Feed a window event. Returns Accepted when a shell was chosen (read
    // chosen()), Cancelled on Escape, Consumed while navigating, Ignored if the
    // picker isn't open.
    Result handle(const toe::win::Event &ev) {
        if (!active_) return Result::Ignored;
        using namespace toe::win;
        if (const auto *kp = std::get_if<KeyPressed>(&ev)) {
            if (kp->key.kind != toe::KeyEvent::Kind::press) return Result::Consumed;
            if (const auto *sk = std::get_if<toe::SpecialKey>(&kp->key.key)) {
                const auto vis = filtered();
                switch (*sk) {
                case toe::SpecialKey::Escape: active_ = false; return Result::Cancelled;
                case toe::SpecialKey::Up:
                    if (!vis.empty()) sel_ = (sel_ + (int)vis.size() - 1) % (int)vis.size();
                    return Result::Consumed;
                case toe::SpecialKey::Down:
                    if (!vis.empty()) sel_ = (sel_ + 1) % (int)vis.size();
                    return Result::Consumed;
                case toe::SpecialKey::Enter:
                case toe::SpecialKey::KpEnter:
                    if (!vis.empty()) { chosen_ = vis[(std::size_t)sel_]; active_ = false; return Result::Accepted; }
                    return Result::Consumed;
                case toe::SpecialKey::Backspace:
                    if (!filter_.empty()) { filter_.pop_back(); sel_ = 0; }
                    return Result::Consumed;
                default: return Result::Consumed;
                }
            }
            return Result::Consumed;
        }
        if (const auto *t = std::get_if<TextEntered>(&ev)) {
            for (char c : t->utf8)
                if (static_cast<unsigned char>(c) >= 0x20) filter_ += c;
            sel_ = 0;
            return Result::Consumed;
        }
        return Result::Consumed; // swallow the rest while open
    }

    // Paint the popup centred in `buf` (grid cells). Transparent outside it.
    void render(glyph::Buffer &buf) const {
        if (!active_) return;
        const int W = buf.width(), H = buf.height();
        if (W < 24 || H < 6) return;
        const auto vis = filtered();
        const int rows = std::min<int>(std::max<int>((int)vis.size(), 1), std::min(12, H - 6));
        const int inner_w = std::clamp(longest() + 8, 24, W - 6);
        const int box_h = rows + 4; // border + title + filter + list + border
        const int x = (W - inner_w) / 2;
        const int y = std::max(1, (H - box_h) / 2);

        const glyph::Rgb bg = glyph::rgb(22, 24, 33), acc = glyph::rgb(122, 168, 255);
        const glyph::Style body{glyph::rgb(224, 227, 238), bg};
        const glyph::Style border{glyph::rgb(70, 78, 110), bg};
        const glyph::Style dim{glyph::rgb(120, 126, 148), bg};

        buf.clear_alpha(0);
        buf.set_alpha(glyph::Rect{x, y, inner_w, box_h}, 250);
        buf.fill(glyph::Rect{x, y, inner_w, box_h}, body);
        buf.frame(glyph::Rect{x, y, inner_w, box_h}, border, glyph::BoxStyle::Rounded);
        buf.text(x + 2, y, " New tab \u2014 shell ", glyph::Style{acc, bg, glyph::Attr::Bold});

        // Filter line.
        const std::string fl = filter_.empty() ? "type to filter\u2026" : filter_;
        buf.text(x + 2, y + 1, fl, filter_.empty() ? dim : body, inner_w - 4);
        for (int c = x + 1; c < x + inner_w - 1; ++c) buf.put(c, y + 2, U'\u2500', border);

        // Scroll the list so the selection is visible.
        const int first = std::clamp(sel_ - rows / 2, 0, std::max(0, (int)vis.size() - rows));
        for (int r = 0; r < rows; ++r) {
            const int i = first + r;
            if (i >= (int)vis.size()) break;
            const int ry = y + 3 + r;
            const bool on = (i == sel_);
            const glyph::Style rs = on
                ? glyph::Style{glyph::rgb(245, 247, 255), glyph::rgb(52, 60, 92)}
                : body;
            if (on) {
                buf.fill(glyph::Rect{x + 1, ry, inner_w - 2, 1}, rs);
                buf.put(x + 1, ry, U'\u2590', glyph::Style{acc, rs.bg});
            }
            const std::string &path = vis[(std::size_t)i];
            buf.put(x + 3, ry, U'\u276f', glyph::Style{acc, rs.bg});
            buf.text(x + 5, ry, shell_name(path), rs, 12);
            // full path, dimmed, right side
            const int px = x + 5 + 13;
            if (px < x + inner_w - 2)
                buf.text(px, ry, path, on ? rs : dim, x + inner_w - 2 - px);
        }
    }

private:
    [[nodiscard]] std::vector<std::string> filtered() const {
        if (filter_.empty()) return shells_;
        std::vector<std::string> out;
        for (const auto &s : shells_)
            if (shell_name(s).find(filter_) != std::string::npos ||
                s.find(filter_) != std::string::npos)
                out.push_back(s);
        return out;
    }
    [[nodiscard]] int longest() const {
        int m = 0;
        for (const auto &s : shells_) m = std::max<int>(m, (int)s.size() + 14);
        return std::min(m, 44);
    }

    bool active_ = false;
    int sel_ = 0;
    std::string filter_;
    std::string chosen_;
    std::vector<std::string> shells_;
};

} // namespace hand

#endif // HAND_SHELL_PICKER_HPP
