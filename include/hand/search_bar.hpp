// SPDX-License-Identifier: LGPL-2.0-or-later
//
// SearchBar — an in-terminal scrollback search UI, built on the same glyph
// immediate-mode toolkit as the settings/help panes and composited the same way
// (Session::render_overlay). Opened with Ctrl+Shift+F; type to filter, Enter /
// Ctrl+G jumps to the next match, Shift+Enter the previous, Esc closes.
//
// It owns only UI state (the query string + open flag). The actual searching
// lives in the engine (toe::Session::search / search_next / search_prev), which
// scans the whole ring, highlights matches, and scrolls the current one into
// view. This class is the thin driver: keystrokes -> Session calls, plus a
// one-line status bar (query text + "3/17" match readout).

#ifndef HAND_SEARCH_BAR_HPP
#define HAND_SEARCH_BAR_HPP

#include <cstdio>
#include <string>
#include <string_view>
#include <variant>

#include "hand/glyph/buffer.hpp"
#include "toe/app.hpp"
#include "toe/terminal.hpp"

namespace hand {

class SearchBar {
public:
    [[nodiscard]] bool active() const noexcept { return active_; }

    // Open the bar for `session` (re-runs the current query, if any).
    void open(toe::Session &session) {
        active_ = true;
        rescan(session);
    }

    // Close the bar and drop the engine's highlighting.
    void close(toe::Session &session) {
        if (!active_) return;
        active_ = false;
        session.search_clear();
    }

    // Feed one window event while the bar is open. Returns true if consumed
    // (must not reach the child). `session` drives the actual search.
    [[nodiscard]] bool handle(const toe::win::Event &ev, toe::Session &session) {
        if (!active_) return false;
        if (const auto *k = std::get_if<toe::win::KeyPressed>(&ev)) {
            return handle_key(k->key, session);
        }
        if (const auto *t = std::get_if<toe::win::TextEntered>(&ev)) {
            // Filter control bytes; append printable UTF-8.
            for (char ch : t->utf8) {
                if (static_cast<unsigned char>(ch) < 0x20) return true;
            }
            query_ += t->utf8;
            rescan(session);
            return true;
        }
        // Swallow everything else (mouse, etc.) so it doesn't hit the child
        // while the bar is up — except let scroll wheel through so the user can
        // still scroll. (Wheel is a MouseWheel event; not consumed here.)
        if (std::holds_alternative<toe::win::MouseWheel>(ev)) return false;
        return true;
    }

    // Paint the one-line bar into `buf` (already sized to the grid). Bottom row.
    void render(glyph::Buffer &buf) const {
        if (!active_) return;
        const int w = buf.width();
        const int y = buf.height() - 1;
        if (w <= 0 || y < 0) return;

        const glyph::Style bar{.fg = glyph::rgb(230, 230, 235), .bg = glyph::rgb(40, 40, 52)};
        const glyph::Style dim{.fg = glyph::rgb(150, 150, 165), .bg = glyph::rgb(40, 40, 52)};
        const glyph::Style cnt{.fg = glyph::rgb(210, 160, 40), .bg = glyph::rgb(40, 40, 52)};
        buf.fill({0, y, w, 1}, bar);

        int x = buf.text(0, y, " search: ", dim);
        x += buf.text(x, y, query_.empty() ? "" : query_, bar, w - x);
        // A block caret at the end of the query.
        if (x < w) buf.put(x, y, U'\u2588', bar);

        // Right-aligned "cur/total" readout.
        char rd[32];
        std::snprintf(rd, sizeof rd, " %zu/%zu ", cur_, total_);
        const int rw = glyph::Buffer::text_width(rd);
        if (rw < w) buf.text(w - rw, y, rd, total_ ? cnt : dim);
    }

private:
    // Re-run the query against the engine and cache the readout counts.
    void rescan(toe::Session &session) {
        total_ = session.search(query_);
        cur_ = session.search_current();
    }

    [[nodiscard]] bool handle_key(const toe::KeyEvent &key, toe::Session &session) {
        using SK = toe::SpecialKey;
        if (const auto *sk = std::get_if<SK>(&key.key)) {
            switch (*sk) {
            case SK::Escape:
                close(session);
                return true;
            case SK::Enter:
            case SK::KpEnter:
                if (key.mods.shift) session.search_prev();
                else session.search_next();
                cur_ = session.search_current();
                return true;
            // Arrows also step through matches (next = Down, prev = Up) — the
            // common expectation alongside Enter / Shift+Enter.
            case SK::Down:
                session.search_next();
                cur_ = session.search_current();
                return true;
            case SK::Up:
                session.search_prev();
                cur_ = session.search_current();
                return true;
            case SK::Backspace:
                if (!query_.empty()) {
                    pop_utf8();
                    rescan(session);
                }
                return true;
            default:
                return true; // swallow other special keys
            }
        }
        // Ctrl+G / Ctrl+N = next, Ctrl+P = previous (readline-ish).
        if (const auto *ti = std::get_if<toe::TextInput>(&key.key)) {
            if (key.mods.ctrl && ti->utf8.size() == 1) {
                const char c = ti->utf8[0];
                if (c == 'g' || c == 'G' || c == 'n' || c == 'N') {
                    session.search_next();
                    cur_ = session.search_current();
                    return true;
                }
                if (c == 'p' || c == 'P') {
                    session.search_prev();
                    cur_ = session.search_current();
                    return true;
                }
            }
        }
        return false; // let TextEntered handle the printable text
    }

    // Remove the last UTF-8 scalar from query_ (not just the last byte).
    void pop_utf8() {
        while (!query_.empty()) {
            const auto b = static_cast<unsigned char>(query_.back());
            query_.pop_back();
            if ((b & 0xC0) != 0x80) break; // stop at a lead byte
        }
    }

    bool active_ = false;
    std::string query_{};
    std::size_t total_ = 0, cur_ = 0;
};

} // namespace hand

#endif // HAND_SEARCH_BAR_HPP
