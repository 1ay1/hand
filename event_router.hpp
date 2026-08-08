// SPDX-License-Identifier: LGPL-2.0-or-later
//
// EventRouter — the frontend's input policy, expressed as an exhaustive visitor
// over the platform's closed Event sum type.
//
// Instead of a 150-line `std::visit` lambda welded into the frame pump, each
// event kind is a small, named `operator()` overload. `std::visit(router, ev)`
// dispatches at compile time — no `if constexpr` ladder, no `std::function`,
// no per-keystroke heap churn. The loop keeps the *mechanics* (poll, drain,
// render, swap); the router owns the *meaning* (what a keypress or click does).

#ifndef HAND_EVENT_ROUTER_HPP
#define HAND_EVENT_ROUTER_HPP

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

#include <sys/wait.h>
#include <unistd.h>

#include "gvte/platform/surface.hpp"
#include "gvte/terminal.hpp"

namespace hand {

namespace pf = gvte::platform;

// Routes translated window events into Session actions. One instance is reused
// across the whole session; `drained()` reports (and resets) whether this batch
// handed bytes to the child, so the host can coalesce the echo into one frame.
class EventRouter {
public:
    EventRouter(gvte::Session &session, pf::Surface &surface, gvte::PixelSize &px, bool &running)
        : s_(session), surf_(surface), px_(px), running_(running) {}

    // The visitor entry point: `std::visit(router, event)`.
    template <class E> void operator()(const E &e) { handle(e); }

    // True (and cleared) if this poll batch sent input to the child. The host
    // uses it to wait briefly for the echo before rendering — same-frame echo.
    [[nodiscard]] bool take_wrote_input() noexcept {
        return std::exchange(wrote_input_, false);
    }

private:
    // --- window lifecycle --------------------------------------------------
    void handle(const pf::CloseRequested &) { running_ = false; }

    void handle(const pf::Resized &e) {
        px_ = e.size;
        // TEA: a resize is a Msg; update() reflows the grid and returns a
        // ResizePty Cmd that run() applies.
        s_.run(s_.update(gvte::Resized{px_}));
    }

    // --- keyboard ----------------------------------------------------------
    void handle(const pf::KeyPressed &e) {
        // Shift+PageUp/Down scroll the scrollback on the primary screen; on the
        // alt screen they go to the app (which owns its own paging).
        if (const auto *sk = std::get_if<gvte::SpecialKey>(&e.key.key);
            sk && e.key.mods.shift &&
            (*sk == gvte::SpecialKey::PageUp || *sk == gvte::SpecialKey::PageDown)) {
            if (!s_.on_alt_screen()) {
                const int page = s_.grid_size().rows - 1;
                s_.scroll(*sk == gvte::SpecialKey::PageUp ? page : -page);
                return;
            }
        }

        // Clipboard shortcuts intercept before the key reaches the child.
        if (const auto *txt = std::get_if<gvte::TextInput>(&e.key.key);
            txt && e.key.mods.ctrl && e.key.mods.shift) {
            if (txt->utf8 == "v" || txt->utf8 == "V") return paste_clipboard();
            if (txt->utf8 == "c" || txt->utf8 == "C") return copy_selection();
        }

        // Everything else is child input: encode + write via the TEA pipeline.
        send_to_child(gvte::Key{e.key});
    }

    void handle(const pf::TextEntered &e) {
        // Ordinary typed/composed text — a Key (never bracketed-paste).
        gvte::KeyEvent k;
        k.key = gvte::TextInput{std::string{e.utf8}};
        send_to_child(gvte::Key{std::move(k)});
    }

    // --- pointer -----------------------------------------------------------
    void handle(const pf::MouseDown &e) {
        const auto [col, vrow] = cell_of(e.x, e.y);
        // Ctrl+Click (or plain click when the app isn't tracking the mouse)
        // opens an OSC 8 hyperlink under the pointer, if any.
        if (e.button == pf::MouseButton::left && (e.mods.ctrl || !s_.wants_mouse())) {
            if (std::string_view uri = s_.link_at(vrow, col); !uri.empty()) {
                open_uri(uri);
                return;
            }
        }
        if (app_owns_mouse(e.mods.shift)) {
            report(gvte::Session::MouseEvent::press, button_code(e.button), col, vrow, e.mods);
        } else if (e.button == pf::MouseButton::left) {
            if (e.click_count >= 3) s_.select_line(vrow, col);
            else if (e.click_count == 2) s_.select_word(vrow, col);
            else s_.select_begin(vrow, col, 0);
        } else if (e.button == pf::MouseButton::middle) {
            paste_clipboard(); // primary-selection paste
        }
    }

    void handle(const pf::MouseMove &e) {
        const auto [col, vrow] = cell_of(e.x, e.y);
        if (s_.wants_mouse() &&
            (s_.wants_mouse_motion() || (e.button_down && s_.wants_mouse_drag()))) {
            report(gvte::Session::MouseEvent::motion, e.button_down ? 0 : 3, col, vrow, {});
        } else if (e.button_down) {
            s_.select_extend(vrow, col);
        } else {
            // Idle pointer motion: track the OSC 8 link under it for hover
            // highlighting. The Session bumps damage when the link changes.
            s_.set_hover(vrow, col);
        }
    }

    void handle(const pf::MouseUp &e) {
        const auto [col, vrow] = cell_of(e.x, e.y);
        if (app_owns_mouse(e.mods.shift)) {
            report(gvte::Session::MouseEvent::release, button_code(e.button), col, vrow, e.mods);
        } else if (e.button == pf::MouseButton::left && s_.has_selection()) {
            // Copy on release so a plain drag-select fills the clipboard.
            if (std::string sel = s_.selected_text(); !sel.empty()) surf_.set_clipboard(sel);
        }
    }

    void handle(const pf::MouseWheel &e) {
        const auto [col, vrow] = cell_of(0, 0);
        if (s_.wants_mouse()) {
            // Wheel maps to buttons 64 (up) / 65 (down) in X10/SGR mouse.
            for (int i = 0; i < std::abs(e.dy); ++i)
                report(gvte::Session::MouseEvent::press, e.dy > 0 ? 64 : 65, col, vrow, {});
        } else if (!s_.on_alt_screen()) {
            s_.scroll(e.dy * 3); // 3 lines per notch
        }
    }

    // --- shared helpers ----------------------------------------------------
    struct CellPos { int col, vrow; };
    [[nodiscard]] CellPos cell_of(int x, int y) const noexcept {
        return {x / std::max(1, s_.cell_width()), y / std::max(1, s_.cell_height())};
    }

    [[nodiscard]] bool app_owns_mouse(bool shift_held) const noexcept {
        return s_.wants_mouse() && !shift_held; // Shift always forces local select
    }

    static int button_code(pf::MouseButton b) noexcept {
        switch (b) {
        case pf::MouseButton::right: return 2;
        case pf::MouseButton::middle: return 1;
        case pf::MouseButton::left: default: return 0;
        }
    }

    void report(gvte::Session::MouseEvent kind, int btn, int col, int vrow,
                gvte::Modifiers m) {
        s_.report_mouse(kind, btn, col, vrow, m.shift, m.alt, m.ctrl);
    }

    void send_to_child(gvte::Msg &&m) {
        s_.run(s_.update(m));
        wrote_input_ = true;
    }

    void paste_clipboard() {
        if (std::string clip = surf_.get_clipboard(); !clip.empty())
            send_to_child(gvte::Paste{std::move(clip)});
    }

    void copy_selection() {
        if (!s_.has_selection()) return;
        if (std::string sel = s_.selected_text(); !sel.empty()) surf_.set_clipboard(sel);
    }

    // Launch the desktop URL handler for an OSC 8 link. fork/exec (never
    // system()) so the URI can't be shell-interpreted — it's passed as a single
    // argv element. Double-fork so the opener isn't a zombie child of the term.
    static void open_uri(std::string_view uri) {
        std::string url{uri};
        const pid_t pid = ::fork();
        if (pid != 0) {
            if (pid > 0) { int st = 0; ::waitpid(pid, &st, 0); } // reap the first fork
            return;
        }
        if (::fork() == 0) { // grandchild: actually exec, detached
            ::setsid();
            const char *argv[] = {"xdg-open", url.c_str(), nullptr};
            ::execvp("xdg-open", const_cast<char *const *>(argv));
            ::_exit(127);
        }
        ::_exit(0); // first child exits immediately
    }

    gvte::Session &s_;
    pf::Surface &surf_;
    gvte::PixelSize &px_;
    bool &running_;
    bool wrote_input_ = false;
};

} // namespace hand

#endif // HAND_EVENT_ROUTER_HPP
