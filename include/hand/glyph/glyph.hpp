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
#include <cctype>
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

    // Linear blend a->b by t in [0,1]. The toolkit derives every incidental
    // surface (title band, popup, shadow, scrollbar) from the core colours so a
    // theme swap recolours the whole chrome coherently.
    [[nodiscard]] static Rgb mix(Rgb a, Rgb b, float t) {
        auto c = [t](std::uint8_t x, std::uint8_t y) {
            const float v = static_cast<float>(x) + (static_cast<float>(y) - x) * t;
            return static_cast<std::uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v) + 0.5f);
        };
        return rgb(c(a.r, b.r), c(a.g, b.g), c(a.b, b.b));
    }
    // The title bar: panel_bg lifted toward the accent for a coloured header.
    [[nodiscard]] Rgb title_bg() const { return mix(panel_bg, accent, 0.16f); }
    // A popup/menu surface: a touch lighter than the panel so it floats above.
    [[nodiscard]] Rgb pop_bg() const { return mix(panel_bg, fg, 0.07f); }
    // Drop-shadow colour (used at the pane's per-cell alpha).
    [[nodiscard]] Rgb shadow() const { return mix(bg, rgb(0, 0, 0), 0.55f); }
};

class Ctx {
public:
    // Overlay alpha tiers (0..255). The scrim (area outside the panel) is mostly
    // see-through so opening the pane barely dims the terminal; the panel body
    // is near-opaque so its text stays crisp. Used via the Buffer alpha plane.
    static constexpr std::uint8_t kScrimAlpha = 64;   // ~25% — faint dim
    static constexpr std::uint8_t kPanelAlpha = 242;  // ~95% — frosted, readable

    // `focus_state` is the caller's PERSISTENT focus index (which row is
    // selected). The Ctx is recreated every frame, so focus can't live in it —
    // the caller owns an int and passes its address; the Ctx reads and updates
    // it. Pass nullptr for a static, non-interactive panel.
    Ctx(Buffer &buf, const Input &in, int *focus_state = nullptr, const Theme &theme = {})
        : buf_(buf), in_(in), theme_(theme), focus_ext_(focus_state) {
        focus_ = focus_ext_ ? *focus_ext_ : 0;
    }

    // --- panel frame --------------------------------------------------------
    // Open a centered panel of `cols`x`rows` with a titled rounded frame,
    // floated over the terminal with a soft drop shadow and a coloured title
    // band. Content is laid out row by row inside.
    void begin_panel(std::string_view title, int cols = 56, int rows = 0,
                     std::uint8_t scrim_alpha = kScrimAlpha,
                     std::uint8_t panel_alpha = kPanelAlpha) {
        // Backdrop: a FAINT scrim that only slightly de-emphasises the terminal
        // (it stays clearly visible behind the pane). The two-tier alpha plane
        // below makes this region mostly see-through while the panel itself is
        // near-opaque — so opening settings doesn't "darken the whole screen".
        buf_.clear(Style{theme_.dim, theme_.bg, Attr::None});
        buf_.clear_alpha(scrim_alpha);
        panel_w_ = std::min(cols, buf_.width() - 2);
        panel_h_ = rows > 0 ? std::min(rows, buf_.height() - 2) : buf_.height() - 4;
        panel_.x = (buf_.width() - panel_w_) / 2;
        panel_.y = (buf_.height() - panel_h_) / 2;
        panel_.w = panel_w_;
        panel_.h = panel_h_;

        // The panel (and its shadow) are near-opaque so text stays crisp.
        buf_.set_alpha(Rect{panel_.x, panel_.y, panel_.w + 2, panel_.h + 1}, panel_alpha);

        // Drop shadow: a soft dark rect offset down-right, so the card visibly
        // floats above the terminal. Drawn BEFORE the panel body.
        const Style shadow{theme_.shadow(), theme_.shadow()};
        buf_.fill(Rect{panel_.x + 2, panel_.y + panel_.h, panel_.w, 1}, shadow);
        buf_.fill(Rect{panel_.x + panel_.w, panel_.y + 1, 2, panel_.h}, shadow);

        // Panel body + rounded frame.
        buf_.fill(panel_, Style{theme_.fg, theme_.panel_bg});
        buf_.frame(panel_, Style{theme_.border, theme_.panel_bg}, BoxStyle::Rounded);

        // Coloured title BAND across the top interior row: a tint with the bold
        // title, an accent ◆ mark on the left, and ‹esc› on the right. A hairline
        // rule under it separates the header from the content like a real
        // titlebar.
        const int band_y = panel_.y + 1;
        const Rgb tbg = theme_.title_bg();
        buf_.fill(Rect{panel_.x + 1, band_y, panel_.w - 2, 1}, Style{theme_.accent, tbg});
        buf_.put(panel_.x + 2, band_y, U'◆', Style{theme_.accent, tbg, Attr::Bold});
        std::string t = std::string(title);
        const int tw = Buffer::text_width(t);
        const int tx = panel_.x + (panel_w_ - tw) / 2;
        buf_.text(tx, band_y, t, Style{theme_.accent, tbg, Attr::Bold});
        // A small ‹esc› affordance on the right of the band.
        const char *hint = "esc";
        buf_.text(panel_.right() - 2 - Buffer::text_width(hint), band_y, hint,
                  Style{Theme::mix(theme_.accent, tbg, 0.35f), tbg});
        // Hairline under the title band.
        buf_.hrule(panel_.x + 1, band_y + 1, panel_.w - 2, Style{theme_.border, theme_.panel_bg});

        content_ = panel_.inset(1);
        content_.x += 1; content_.w -= 2; // horizontal breathing room
        cursor_y_ = band_y + 3;           // header band + rule + a blank line
        row_index_ = 0;
        activated_ = -1;
        consumed_ = false;
    }

    // Finish: draw the footer hint line and resolve focus wrap. `footer`
    // overrides the default interactive hint (for read-only panes).
    void end_panel(std::string_view footer = {}) {
        // Paint any open dropdown LAST so it floats above every row (overlay).
        draw_popup();

        // Footer key hints.
        const int fy = panel_.bottom() - 2;
        buf_.hrule(content_.x, fy - 1, content_.w, Style{theme_.border, theme_.panel_bg});
        const char *default_hint = "\u2191\u2193 move   \u2190\u2192/space edit   tab section   esc close";
        if (footer.empty())
            buf_.text(content_.x, fy, default_hint, Style{theme_.dim, theme_.panel_bg}, content_.w);
        else
            buf_.text(content_.x, fy, footer, Style{theme_.dim, theme_.panel_bg}, content_.w);

        // Focus wrap after we know the row count.
        if (row_count_ > 0) {
            if (focus_ < 0) focus_ = row_count_ - 1;
            if (focus_ >= row_count_) focus_ = 0;
        }
        row_count_ = row_index_;
        // Global nav that wasn't consumed by a widget: Up/Down move focus.
        // Tab is reserved by the caller for section switching (it never reaches
        // here consumed, but we deliberately don't treat it as focus-move).
        if (!consumed_) {
            if (in_.key == Key::Down) move_focus(+1);
            else if (in_.key == Key::Up) move_focus(-1);
        }
        // Write the updated focus back to the caller's persistent state.
        if (focus_ext_) *focus_ext_ = focus_;
    }

    [[nodiscard]] bool escaped() const { return in_.key == Key::Escape; }

    // --- section tab bar ---------------------------------------------------
    // A focusable row of section tabs (e.g. Appearance · Font · Colors). When
    // focused, Left/Right switch sections; the active one is highlighted. Draws
    // a rule under it. Returns true when the section changed this frame — the
    // caller then lays out only that section's widgets. This is what turns the
    // panel from a flat wall of options into a navigable, categorized UI.
    bool tab_bar(const std::vector<std::string> &sections, int *active) {
        const int r = begin_row();
        const bool foc = (r == focus_);
        const int n = (int)sections.size();
        bool changed = false;
        if (foc) {
            if (in_.key == Key::Left)  { *active = (*active - 1 + n) % n; changed = true; }
            if (in_.key == Key::Right) { *active = (*active + 1) % n; changed = true; }
        }
        *active = n > 0 ? std::clamp(*active, 0, n - 1) : 0;

        // Each tab needs (label + 2 caps/pad + 1 gap) cells. Compute widths so
        // we can SCROLL the strip: if all tabs don't fit, shift the window so
        // the active tab is always visible (with ‹/› overflow markers). This
        // guarantees every tab is reachable on any window width.
        auto tw = [&](int i) { return (int)Buffer::text_width(sections[(std::size_t)i]) + 3; };
        int total = 0;
        for (int i = 0; i < n; ++i) total += tw(i);
        const int avail = content_.w - 2;

        int start = 0;
        if (total > avail) {
            // Scroll so the active tab sits within [start, end). Grow the window
            // backward from active until it fills, then adjust so active fits.
            int used = 0;
            start = *active;
            for (int i = *active; i >= 0; --i) {
                if (used + tw(i) > avail) break;
                used += tw(i); start = i;
            }
            // If room remains, extend forward too (keeps the strip full-looking).
        }

        int x = content_.x + 1;
        const int maxx = content_.right() - 1;
        // Left overflow marker.
        if (start > 0) { buf_.put(content_.x, cursor_y_, U'‹', Style{theme_.dim, theme_.panel_bg}); }
        int last_drawn = start - 1;
        for (int i = start; i < n; ++i) {
            const bool on = (i == *active);
            const std::string &name = sections[(std::size_t)i];
            const int label_w = (int)Buffer::text_width(name);
            const int need = label_w + 2; // 1-col pad/cap each side
            if (x + need + 1 > maxx) break; // strip full
            if (on) {
                buf_.put(x, cursor_y_, U'▐', Style{theme_.accent, theme_.panel_bg});
                buf_.fill(Rect{x + 1, cursor_y_, label_w, 1}, Style{theme_.accent_fg, theme_.accent});
                buf_.text(x + 1, cursor_y_, name,
                          Style{theme_.accent_fg, theme_.accent, Attr::Bold});
                buf_.put(x + 1 + label_w, cursor_y_, U'▌', Style{theme_.accent, theme_.panel_bg});
            } else {
                buf_.text(x + 1, cursor_y_, name,
                          Style{foc ? theme_.fg : theme_.dim, theme_.panel_bg});
            }
            x += need + 1; // gap between tabs
            last_drawn = i;
        }
        // Right overflow marker.
        if (last_drawn < n - 1)
            buf_.put(content_.right() - 1, cursor_y_, U'›', Style{theme_.dim, theme_.panel_bg});
        end_row();
        buf_.hrule(content_.x, cursor_y_, content_.w, Style{theme_.border, theme_.panel_bg});
        cursor_y_ += 1;
        return changed;
    }

    // --- section heading (non-focusable) -----------------------------------
    void heading(std::string_view text) {
        row_gap();
        buf_.text(content_.x, cursor_y_, text,
                  Style{theme_.accent, theme_.panel_bg, Attr::Bold});
        cursor_y_ += 1;
        buf_.hrule(content_.x, cursor_y_, content_.w, Style{theme_.border, theme_.panel_bg});
        cursor_y_ += 1;
    }

    // --- key/value info row (non-focusable) --------------------------------
    // A two-column line: `key` in the accent colour on the left, `desc` in the
    // normal fg on the right. Used by read-only panes (the help pane) to list
    // keybindings without any interactive widget.
    void kv_row(std::string_view key, std::string_view desc) {
        const int y = cursor_y_;
        buf_.text(content_.x + 1, y, key,
                  Style{theme_.accent, theme_.panel_bg, Attr::Bold});
        // Align descriptions to a fixed column so they line up.
        const int desc_x = content_.x + 18;
        buf_.text(desc_x, y, desc, Style{theme_.fg, theme_.panel_bg});
        cursor_y_ += 1;
    }

    // A plain full-width text line (non-focusable), dimmed — for notes/footers
    // inside a pane.
    void note(std::string_view text) {
        buf_.text(content_.x + 1, cursor_y_, text, Style{theme_.dim, theme_.panel_bg});
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
        // A compact filled bar with a thumb knob + numeric value.
        const int vx = value_x();
        const int barw = std::max(8, content_.right() - vx - 8);
        const float frac = hi > lo ? float(*value - lo) / float(hi - lo) : 0.f;
        const int filled = int(frac * barw + 0.5f);
        const int knob = std::clamp(filled, 0, barw - 1);
        const Rgb rowbg = foc ? theme_.focus_bg : theme_.panel_bg;
        Style bar_on{theme_.accent, rowbg};
        Style bar_off{theme_.border, rowbg};
        for (int i = 0; i < barw; ++i) {
            if (i == knob)
                buf_.put(vx + i, cursor_y_, U'◉',
                         Style{foc ? theme_.accent : theme_.fg, rowbg, Attr::Bold});
            else
                buf_.put(vx + i, cursor_y_, i < filled ? U'━' : U'─',
                         i < filled ? bar_on : bar_off);
        }
        char num[16];
        std::snprintf(num, sizeof num, " %d", *value);
        buf_.text(vx + barw + 1, cursor_y_, num, Style{theme_.fg, rowbg, Attr::Bold});
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

    // A DROPDOWN: shows the current value; Enter/Space opens an inline,
    // scrollable list below it; arrows move within the list, Enter picks, Esc
    // closes. `open_row` is the caller's persistent "which row is expanded"
    // state (-1 = none); `list_sel` the highlighted index while open; `list_top`
    // the scroll offset. Returns true when the selection changed.
    bool dropdown(std::string_view label, int *index, const std::vector<std::string> &opts,
                  int *open_row, int *list_sel, int *list_top, int max_visible = 8,
                  std::string *filter = nullptr) {
        static std::string s_scratch; // fallback when the caller passes none
        std::string &flt = filter ? *filter : s_scratch;
        const int r = begin_row();
        const bool foc = (r == focus_);
        const int n = static_cast<int>(opts.size());
        bool changed = false;
        const bool is_open = (*open_row == r);

        if (foc && !is_open && act()) {
            // Open: seed the highlight at the current value, clear the filter.
            *open_row = r;
            *list_sel = std::clamp(*index, 0, std::max(0, n - 1));
            *list_top = std::clamp(*list_sel - max_visible / 2, 0, std::max(0, n - max_visible));
            flt.clear();
            consumed_ = true;
        }

        // --- the closed row: label + current value + ▾ ---
        paint_label(label, foc);
        const int vx = value_x();
        *index = n > 0 ? std::clamp(*index, 0, n - 1) : 0;
        const std::string val = n > 0 ? opts[static_cast<std::size_t>(*index)] : "(none)";
        Style vs{is_open ? theme_.accent : theme_.fg, foc ? theme_.focus_bg : theme_.panel_bg,
                 Attr::Bold};
        buf_.text(vx, cursor_y_, val, vs, content_.right() - vx - 2);
        // A pill-style chevron box on the right edge.
        buf_.put(content_.right() - 2, cursor_y_, is_open ? U'▴' : U'▾',
                 Style{foc ? theme_.accent : theme_.dim, foc ? theme_.focus_bg : theme_.panel_bg});
        end_row();

        if (!is_open) return changed;

        // --- filtering: build the visible index list from flt ---------
        // Substring match (case-insensitive) so you can type "night" and land on
        // Tokyo Night among 600 entries — far better than a first-letter jump.
        auto lc = [](std::string s) { for (char &c : s) c = char(std::tolower((unsigned char)c)); return s; };
        const std::string needle = lc(flt);
        std::vector<int> hits;
        hits.reserve(static_cast<std::size_t>(n));
        for (int i = 0; i < n; ++i)
            if (needle.empty() || lc(opts[static_cast<std::size_t>(i)]).find(needle) != std::string::npos)
                hits.push_back(i);
        if (hits.empty()) hits.push_back(std::clamp(*index, 0, std::max(0, n - 1)));
        const int m = static_cast<int>(hits.size());

        // *list_sel is an index into the FILTERED list here.
        if (in_.key == Key::Down) { *list_sel = std::min(m - 1, *list_sel + 1); consumed_ = true; }
        else if (in_.key == Key::Up) { *list_sel = std::max(0, *list_sel - 1); consumed_ = true; }
        else if (in_.key == Key::PageDown) { *list_sel = std::min(m - 1, *list_sel + max_visible); consumed_ = true; }
        else if (in_.key == Key::PageUp) { *list_sel = std::max(0, *list_sel - max_visible); consumed_ = true; }
        else if (in_.key == Key::Escape) { *open_row = -1; flt.clear(); consumed_ = true; return changed; }
        else if (in_.key == Key::Enter || in_.key == Key::Space) {
            // Enter just commits + closes; the value already tracks the highlight
            // (select-on-move below), so this is a confirm, not the only apply.
            const int pick = hits[static_cast<std::size_t>(std::clamp(*list_sel, 0, m - 1))];
            if (pick != *index) { *index = pick; changed = true; }
            *open_row = -1; flt.clear(); consumed_ = true;
        }
        else if (in_.key == Key::Backspace) {
            if (!flt.empty()) flt.pop_back();
            *list_sel = 0; consumed_ = true;
        }
        // Any printable char extends the incremental filter.
        else if (in_.key == Key::Char && in_.ch > 0x20 && in_.ch < 0x7f) {
            flt.push_back(char(in_.ch));
            *list_sel = 0; consumed_ = true;
        }
        *list_sel = std::clamp(*list_sel, 0, std::max(0, m - 1));

        // SELECT-ON-MOVE: the currently highlighted option becomes the live
        // value immediately, so arrowing/filtering previews it (e.g. the theme
        // recolours as you move). Enter/Esc just confirm/cancel the browse.
        if (m > 0) {
            const int hot = hits[static_cast<std::size_t>(std::clamp(*list_sel, 0, m - 1))];
            if (hot != *index && *open_row == r) { *index = hot; changed = true; }
        }

        // Keep the highlighted item in view (window over the FILTERED list).
        if (*list_sel < *list_top) *list_top = *list_sel;
        if (*list_sel >= *list_top + max_visible) *list_top = *list_sel - max_visible + 1;
        *list_top = std::clamp(*list_top, 0, std::max(0, m - max_visible));

        // Record the popup to be painted LAST (as an overlay on top of every
        // other row), not inline where it would be overdrawn by later widgets.
        // The closed row already advanced cursor_y_ via end_row(), so following
        // rows lay out normally underneath the floating menu.
        popup_.active = true;
        popup_.x = vx - 1;
        popup_.w = content_.right() - popup_.x;
        popup_.top = cursor_y_;
        popup_.max_visible = max_visible;
        popup_.list_sel = *list_sel;
        popup_.list_top = list_top;
        popup_.index = *index;
        popup_.filter = flt;
        popup_.n = n;
        popup_.hits = std::move(hits);
        popup_.opts = &opts;
        return changed;
    }

    // Paint the deferred dropdown popup (see dropdown()). Called by end_panel()
    // so the menu floats above all rows. A no-op when nothing is open.
    void draw_popup() {
        if (!popup_.active) return;
        const int m = static_cast<int>(popup_.hits.size());
        const int lx = popup_.x, lw = popup_.w, top = popup_.top;
        const auto &opts = *popup_.opts;
        int *list_top = popup_.list_top;

        // Clamp height to fit between `top` and just above the footer rule.
        const int avail = panel_.bottom() - 3 - top - 3; // -frame(2) -header(1)
        int vis = std::min(popup_.max_visible, m);
        if (avail > 0) vis = std::min(vis, avail);
        vis = std::max(vis, 1);
        if (popup_.list_sel >= *list_top + vis) *list_top = popup_.list_sel - vis + 1;
        *list_top = std::clamp(*list_top, 0, std::max(0, m - vis));
        const int box_h = vis + 2 /*frame*/ + 1 /*filter header*/;
        const Rgb pbg = theme_.pop_bg();

        // Soft drop shadow (two offset bands) so the menu clearly floats.
        buf_.fill(Rect{lx + 1, top + box_h, lw, 1}, Style{theme_.shadow(), theme_.shadow()});
        buf_.fill(Rect{lx + lw, top + 1, 1, box_h}, Style{theme_.shadow(), theme_.shadow()});
        // Body + rounded accent frame.
        buf_.fill(Rect{lx, top, lw, box_h}, Style{theme_.fg, pbg});
        buf_.frame(Rect{lx, top, lw, box_h}, Style{theme_.accent, pbg}, BoxStyle::Rounded);

        // Filter header: search glyph + live query (or hint) + match count.
        const int hy = top + 1;
        buf_.put(lx + 2, hy, U'🔍', Style{theme_.accent, pbg});
        if (popup_.filter.empty())
            buf_.text(lx + 4, hy, "type to filter…", Style{theme_.dim, pbg}, lw - 12);
        else
            buf_.text(lx + 4, hy, popup_.filter, Style{theme_.fg, pbg, Attr::Bold}, lw - 14);
        char cnt[24];
        std::snprintf(cnt, sizeof cnt, "%d/%d", m, popup_.n);
        buf_.text(lx + lw - 2 - Buffer::text_width(cnt), hy, cnt, Style{theme_.dim, pbg});

        // Options.
        for (int i = 0; i < vis; ++i) {
            const int fi = *list_top + i;
            if (fi < 0 || fi >= m) break;
            const int oi = popup_.hits[static_cast<std::size_t>(fi)];
            const int ry = top + 2 + i;
            const bool sel = (fi == popup_.list_sel);
            const bool cur = (oi == popup_.index);
            const Rgb rbg = sel ? theme_.accent : pbg;
            const Rgb rfg = sel ? theme_.accent_fg : (cur ? theme_.accent : theme_.fg);
            buf_.fill(Rect{lx + 1, ry, lw - 2, 1}, Style{rfg, rbg});
            const char32_t mark = cur ? U'✓' : (sel ? U'›' : U' ');
            buf_.put(lx + 2, ry, mark, Style{rfg, rbg, Attr::Bold});
            buf_.text(lx + 4, ry, opts[static_cast<std::size_t>(oi)],
                      Style{rfg, rbg, sel ? Attr::Bold : Attr::None}, lw - 6);
            if (m > vis) {
                const int thumb = std::clamp(*list_top * vis / std::max(1, m), 0, vis - 1);
                buf_.put(lx + lw - 2, ry, i == thumb ? U'█' : U'│',
                         Style{i == thumb ? theme_.accent : theme_.border, rbg});
            }
        }
        popup_.active = false;
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
        // Inset field surface derived from the theme (a hair darker than panel).
        Style fs{theme_.fg, foc ? Theme::mix(theme_.panel_bg, theme_.bg, 0.5f)
                                : Theme::mix(theme_.panel_bg, theme_.bg, 0.3f)};
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
        const Rgb rowbg = foc ? theme_.focus_bg : theme_.panel_bg;
        // Live swatch: a rounded chip so the colour reads as a sample, not a
        // stray block.
        buf_.put(vx, cursor_y_, U'◉', Style{sw, rowbg, Attr::Bold});
        buf_.put(vx + 1, cursor_y_, U'◉', Style{sw, rowbg, Attr::Bold});
        buf_.text(vx + 3, cursor_y_, *hex, Style{theme_.fg, rowbg, Attr::Bold});
        if (foc) buf_.put(vx + 3 + Buffer::text_width(*hex), cursor_y_, U'▏',
                          Style{theme_.accent, rowbg});
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

    // A blank spacer row (non-focusable).
    void gap() { cursor_y_ += 1; }

    [[nodiscard]] int focus() const noexcept { return focus_; }
    void set_focus(int f) noexcept { focus_ = f; }

private:
    // --- row layout ---------------------------------------------------------
    int begin_row() {
        const int idx = row_index_++;
        row_start_y_ = cursor_y_;
        // Highlight the whole focused row + a bright accent caret bar down its
        // left edge — an unmistakable cursor that reads at a glance.
        if (idx == focus_) {
            buf_.fill(Rect{content_.x, cursor_y_, content_.w, 1},
                      Style{theme_.fg, theme_.focus_bg});
            buf_.put(content_.x, cursor_y_, U'▐', Style{theme_.accent, theme_.focus_bg});
        }
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

    // A dropdown popup recorded during a widget pass, painted last by end_panel
    // so it floats above every other row (a true overlay). See dropdown().
    struct DeferredPopup {
        bool active = false;
        int x = 0, w = 0, top = 0, max_visible = 8;
        int list_sel = 0, index = 0, n = 0;
        int *list_top = nullptr;
        std::string filter;
        std::vector<int> hits;
        const std::vector<std::string> *opts = nullptr;
    } popup_{};

    Rect panel_{}, content_{};
    int panel_w_ = 0, panel_h_ = 0;
    int cursor_y_ = 0, row_start_y_ = 0;
    int row_index_ = 0, row_count_ = 0;
    int focus_ = 0;
    int label_w_ = 18;
    int activated_ = -1;
    bool consumed_ = false;
    int *focus_ext_ = nullptr; // caller's persistent focus index (in/out)
};

} // namespace glyph

#endif // HAND_GLYPH_HPP
