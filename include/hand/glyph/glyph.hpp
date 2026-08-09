// SPDX-License-Identifier: LGPL-2.0-or-later
//
// glyph — the immediate-mode context + widgets. You describe your UI every
// frame as a sequence of widget calls; the context handles focus, keyboard
// navigation, and paints into a Buffer. No retained widget tree, no callbacks,
// no allocation per frame — the classic IMGUI model, sized for a terminal.
//
//   glyph::Ctx ui(buf, input);
//   ui.begin_panel("Settings");
//   ui.toggle("Ligatures", &cfg.ligatures);
//   ui.slider_int("Font size", &cfg.size, 6, 48);
//   ui.select("Cursor", &cfg.cursor, {"block","bar","underline"});
//   ui.text_input("Font family", &cfg.family);
//   if (ui.button("Save")) save(cfg);
//   ui.end_panel();
//
// Navigation: Up/Down (or Tab/Shift-Tab) move focus between rows; Left/Right,
// Space, and typing edit the focused widget; Enter activates buttons; Esc
// closes. Every widget is one focusable ROW, which is the right grain for a
// settings form and keeps keyboard nav trivial.

#ifndef HAND_GLYPH_HPP
#define HAND_GLYPH_HPP

#include <algorithm>
#include <cstdio>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "hand/glyph/buffer.hpp"

namespace glyph {

// The keys the UI reacts to. The host translates its native input into these.
enum class Key {
    None, Up, Down, Left, Right, Tab, ShiftTab, Enter, Escape, Space,
    Backspace, Delete, Home, End, Char, PageUp, PageDown,
};

struct Input {
    Key key = Key::None;
    char32_t ch = 0;   // when key == Char, the typed codepoint
    // Mouse (optional; -1 = none). Cell coordinates within the panel.
    int mouse_x = -1, mouse_y = -1;
    bool mouse_pressed = false;
};

// The visual theme. A few colors + accents; everything else derives.
struct Theme {
    Rgb bg        = rgb(24, 24, 32);
    Rgb panel_bg  = rgb(30, 30, 40);
    Rgb fg        = rgb(220, 220, 228);
    Rgb dim       = rgb(120, 120, 135);
    Rgb accent    = rgb(120, 170, 255);
    Rgb accent_fg = rgb(16, 18, 26);
    Rgb border    = rgb(70, 72, 92);
    Rgb focus_bg  = rgb(44, 48, 68);
    Rgb ok        = rgb(120, 210, 140);
    Rgb warn      = rgb(240, 180, 90);

    [[nodiscard]] Style base() const { return {fg, panel_bg}; }
    [[nodiscard]] Style dimmed() const { return {dim, panel_bg}; }
    [[nodiscard]] Style focused() const { return {fg, focus_bg}; }
    [[nodiscard]] Style accented() const { return {accent, panel_bg}; }
};

class Ctx {
public:
    Ctx(Buffer &buf, const Input &in, const Theme &theme = {})
        : buf_(buf), in_(in), theme_(theme) {}

    // --- panel frame --------------------------------------------------------
    // Open a centered panel of `cols`x`rows` with a titled rounded frame.
    // Content is laid out row by row inside. Returns the content rect.
    void begin_panel(std::string_view title, int cols = 56, int rows = 0) {
        // Dim the whole backdrop first (a subtle scrim over the terminal).
        buf_.clear(Style{theme_.dim, theme_.bg, Attr::None});
        panel_w_ = std::min(cols, buf_.width() - 2);
        // Auto-height: caller may pass 0 and we grow as widgets are added, but
        // for a stable layout we reserve a generous height and frame at end.
        panel_h_ = rows > 0 ? std::min(rows, buf_.height() - 2) : buf_.height() - 4;
        panel_.x = (buf_.width() - panel_w_) / 2;
        panel_.y = (buf_.height() - panel_h_) / 2;
        panel_.w = panel_w_;
        panel_.h = panel_h_;

        buf_.fill(panel_, Style{theme_.fg, theme_.panel_bg});
        buf_.frame(panel_, Style{theme_.border, theme_.panel_bg}, BoxStyle::Rounded);

        // Title, centered in the top border with padding.
        std::string t = " " + std::string(title) + " ";
        const int tw = Buffer::text_width(t);
        const int tx = panel_.x + (panel_w_ - tw) / 2;
        buf_.text(tx, panel_.y, t, Style{theme_.accent, theme_.panel_bg, Attr::Bold});

        content_ = panel_.inset(1);
        content_.x += 1; content_.w -= 2; // horizontal breathing room
        cursor_y_ = content_.y + 1;
        row_index_ = 0;
        if (first_frame_) { focus_ = 0; first_frame_ = false; }
        activated_ = -1;
        consumed_ = false;
    }

    // Finish: draw the footer hint line and resolve focus wrap.
    void end_panel() {
        // Footer key hints.
        const int fy = panel_.bottom() - 2;
        buf_.hrule(content_.x, fy - 1, content_.w, Style{theme_.border, theme_.panel_bg});
        const char *hint = "↑↓ move   ←→/space edit   enter ok   esc close";
        buf_.text(content_.x, fy, hint, Style{theme_.dim, theme_.panel_bg});

        // Focus wrap after we know the row count.
        if (row_count_ > 0) {
            if (focus_ < 0) focus_ = row_count_ - 1;
            if (focus_ >= row_count_) focus_ = 0;
        }
        row_count_ = row_index_;
        // Global nav that wasn't consumed by a widget: move focus.
        if (!consumed_) {
            if (in_.key == Key::Down || in_.key == Key::Tab) move_focus(+1);
            else if (in_.key == Key::Up || in_.key == Key::ShiftTab) move_focus(-1);
        }
    }

    [[nodiscard]] bool escaped() const { return in_.key == Key::Escape; }

    // --- section heading (non-focusable) -----------------------------------
    void heading(std::string_view text) {
        row_gap();
        buf_.text(content_.x, cursor_y_, text,
                  Style{theme_.accent, theme_.panel_bg, Attr::Bold});
        cursor_y_ += 1;
        buf_.hrule(content_.x, cursor_y_, content_.w, Style{theme_.border, theme_.panel_bg});
        cursor_y_ += 1;
    }

    // --- widgets ------------------------------------------------------------

    // On/off toggle. Returns true if the value changed this frame.
    bool toggle(std::string_view label, bool *value) {
        const int r = begin_row();
        const bool foc = (r == focus_);
        bool changed = false;
        if (foc && (act() || in_.key == Key::Left || in_.key == Key::Right)) {
            *value = !*value;
            changed = true;
            consumed_ = true;
        }
        paint_label(label, foc);
        const char *on = "  ●  ", *off = "  ○  ";
        Style sw = *value ? Style{theme_.accent_fg, theme_.accent}
                          : Style{theme_.dim, foc ? theme_.focus_bg : theme_.panel_bg};
        const std::string txt = *value ? " on " : " off";
        const int vx = value_x();
        buf_.text(vx, cursor_y_, *value ? on : off, sw);
        buf_.text(vx + 5, cursor_y_, txt,
                  Style{*value ? theme_.ok : theme_.dim, foc ? theme_.focus_bg : theme_.panel_bg});
        end_row();
        return changed;
    }

    // Integer slider with left/right (and shift for x10). Returns changed.
    bool slider_int(std::string_view label, int *value, int lo, int hi, int step = 1) {
        const int r = begin_row();
        const bool foc = (r == focus_);
        bool changed = false;
        if (foc) {
            if (in_.key == Key::Left) { *value = std::max(lo, *value - step); changed = true; consumed_ = true; }
            else if (in_.key == Key::Right) { *value = std::min(hi, *value + step); changed = true; consumed_ = true; }
        }
        paint_label(label, foc);
        // A compact [====----] bar + numeric value.
        const int vx = value_x();
        const int barw = std::max(8, content_.right() - vx - 8);
        const float frac = hi > lo ? float(*value - lo) / float(hi - lo) : 0.f;
        const int filled = int(frac * barw + 0.5f);
        Style bar_on{theme_.accent, foc ? theme_.focus_bg : theme_.panel_bg};
        Style bar_off{theme_.border, foc ? theme_.focus_bg : theme_.panel_bg};
        for (int i = 0; i < barw; ++i)
            buf_.put(vx + i, cursor_y_, i < filled ? U'━' : U'─', i < filled ? bar_on : bar_off);
        char num[16];
        std::snprintf(num, sizeof num, " %d", *value);
        buf_.text(vx + barw + 1, cursor_y_, num,
                  Style{theme_.fg, foc ? theme_.focus_bg : theme_.panel_bg, Attr::Bold});
        end_row();
        return changed;
    }

    // Cycle-select among options with left/right. Returns changed.
    bool select(std::string_view label, int *index, const std::vector<std::string> &opts) {
        const int r = begin_row();
        const bool foc = (r == focus_);
        bool changed = false;
        const int n = static_cast<int>(opts.size());
        if (foc && n > 0) {
            if (in_.key == Key::Left) { *index = (*index - 1 + n) % n; changed = true; consumed_ = true; }
            else if (in_.key == Key::Right || act()) { *index = (*index + 1) % n; changed = true; consumed_ = true; }
        }
        paint_label(label, foc);
        *index = n > 0 ? std::clamp(*index, 0, n - 1) : 0;
        const std::string val = n > 0 ? opts[static_cast<std::size_t>(*index)] : "";
        const int vx = value_x();
        Style vs{theme_.fg, foc ? theme_.focus_bg : theme_.panel_bg, Attr::Bold};
        buf_.text(vx, cursor_y_, "‹ ", Style{theme_.dim, foc ? theme_.focus_bg : theme_.panel_bg});
        buf_.text(vx + 2, cursor_y_, val, vs);
        buf_.text(vx + 2 + Buffer::text_width(val), cursor_y_, " ›",
                  Style{theme_.dim, foc ? theme_.focus_bg : theme_.panel_bg});
        end_row();
        return changed;
    }

    // Free-text editor. Typing edits when focused; returns changed.
    bool text_input(std::string_view label, std::string *value) {
        const int r = begin_row();
        const bool foc = (r == focus_);
        bool changed = false;
        if (foc) {
            if (in_.key == Key::Char && in_.ch >= 0x20) { append_utf8(*value, in_.ch); changed = true; consumed_ = true; }
            else if (in_.key == Key::Backspace && !value->empty()) { pop_utf8(*value); changed = true; consumed_ = true; }
        }
        paint_label(label, foc);
        const int vx = value_x();
        const int field_w = content_.right() - vx;
        Style fs{theme_.fg, foc ? rgb(20, 22, 30) : rgb(22, 24, 32)};
        buf_.fill(Rect{vx, cursor_y_, field_w, 1}, fs);
        // Show the tail of the value if it overflows.
        std::string shown = *value;
        int vw = Buffer::text_width(shown);
        while (vw > field_w - 1 && !shown.empty()) { pop_utf8(shown); vw = Buffer::text_width(shown); }
        buf_.text(vx, cursor_y_, shown, fs);
        if (foc) buf_.put(vx + vw, cursor_y_, U'▏', Style{theme_.accent, fs.bg});
        end_row();
        return changed;
    }

    // A hex color field with a live swatch. Value is "#rrggbb".
    bool color(std::string_view label, std::string *hex) {
        const int r = begin_row();
        const bool foc = (r == focus_);
        bool changed = false;
        if (foc) {
            if (in_.key == Key::Char && is_hex_char(in_.ch) && hex->size() < 7) { hex->push_back(char(in_.ch)); changed = true; consumed_ = true; }
            else if (in_.key == Key::Backspace && hex->size() > 1) { hex->pop_back(); changed = true; consumed_ = true; }
        }
        paint_label(label, foc);
        const int vx = value_x();
        Rgb sw = parse_hex(*hex);
        buf_.fill(Rect{vx, cursor_y_, 2, 1}, Style{sw, sw});     // live swatch
        buf_.text(vx + 3, cursor_y_, *hex,
                  Style{theme_.fg, foc ? theme_.focus_bg : theme_.panel_bg, Attr::Bold});
        if (foc) buf_.put(vx + 3 + Buffer::text_width(*hex), cursor_y_, U'▏',
                          Style{theme_.accent, theme_.focus_bg});
        end_row();
        return changed;
    }

    // A button row. Returns true when activated (Enter/Space) while focused.
    bool button(std::string_view text) {
        const int r = begin_row();
        const bool foc = (r == focus_);
        const bool hit = foc && act();
        if (hit) consumed_ = true;
        const std::string lbl = "  " + std::string(text) + "  ";
        const int bw = Buffer::text_width(lbl);
        const int bx = content_.x + (content_.w - bw) / 2;
        Style bs = foc ? Style{theme_.accent_fg, theme_.accent, Attr::Bold}
                       : Style{theme_.fg, theme_.border};
        buf_.text(bx, cursor_y_, lbl, bs);
        end_row();
        return hit;
    }

    // A read-only info line (non-focusable).
    void info(std::string_view text) {
        buf_.text(content_.x, cursor_y_, text, Style{theme_.dim, theme_.panel_bg});
        cursor_y_ += 1;
    }

    [[nodiscard]] int focus() const noexcept { return focus_; }
    void set_focus(int f) noexcept { focus_ = f; }

private:
    // --- row layout ---------------------------------------------------------
    int begin_row() {
        const int idx = row_index_++;
        row_start_y_ = cursor_y_;
        // Highlight the whole focused row for a clear cursor.
        if (idx == focus_)
            buf_.fill(Rect{content_.x, cursor_y_, content_.w, 1},
                      Style{theme_.fg, theme_.focus_bg});
        return idx;
    }
    void end_row() { cursor_y_ += 1; }
    void row_gap() { cursor_y_ += 1; }

    void paint_label(std::string_view label, bool foc) {
        Style ls{foc ? theme_.fg : theme_.fg, foc ? theme_.focus_bg : theme_.panel_bg,
                 foc ? Attr::Bold : Attr::None};
        buf_.text(content_.x + 1, cursor_y_, label, ls, label_w_);
    }
    [[nodiscard]] int value_x() const noexcept { return content_.x + label_w_ + 2; }

    [[nodiscard]] bool act() const noexcept { return in_.key == Key::Enter || in_.key == Key::Space; }

    void move_focus(int d) {
        if (row_count_ <= 0) return;
        focus_ = (focus_ + d + row_count_) % row_count_;
    }

    static void append_utf8(std::string &s, char32_t cp) {
        if (cp < 0x80) s.push_back(char(cp));
        else if (cp < 0x800) { s.push_back(char(0xC0 | (cp >> 6))); s.push_back(char(0x80 | (cp & 0x3F))); }
        else if (cp < 0x10000) { s.push_back(char(0xE0 | (cp >> 12))); s.push_back(char(0x80 | ((cp >> 6) & 0x3F))); s.push_back(char(0x80 | (cp & 0x3F))); }
        else { s.push_back(char(0xF0 | (cp >> 18))); s.push_back(char(0x80 | ((cp >> 12) & 0x3F))); s.push_back(char(0x80 | ((cp >> 6) & 0x3F))); s.push_back(char(0x80 | (cp & 0x3F))); }
    }
    static void pop_utf8(std::string &s) {
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
        if (!s.empty()) s.pop_back();
    }
    static bool is_hex_char(char32_t c) {
        return c == '#' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    }
    static Rgb parse_hex(const std::string &h) {
        if (h.size() != 7 || h[0] != '#') return rgb(40, 40, 48);
        auto v = [&](int i) {
            auto d = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return 0;
            };
            return d(h[i]) * 16 + d(h[i + 1]);
        };
        return rgb(std::uint8_t(v(1)), std::uint8_t(v(3)), std::uint8_t(v(5)));
    }

    Buffer &buf_;
    const Input &in_;
    Theme theme_;

    Rect panel_{}, content_{};
    int panel_w_ = 0, panel_h_ = 0;
    int cursor_y_ = 0, row_start_y_ = 0;
    int row_index_ = 0, row_count_ = 0;
    int focus_ = 0;
    int label_w_ = 18;
    int activated_ = -1;
    bool consumed_ = false;
    bool first_frame_ = true;
};

} // namespace glyph

#endif // HAND_GLYPH_HPP
