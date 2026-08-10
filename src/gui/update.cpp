// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand/gui/update.cpp — the pure GUI reducer.
//
// gui_update folds one GuiMsg into the GuiModel and returns the GuiCmds to run.
// It is the ONLY place tab lifecycle + focus + input routing is decided, and it
// is pure (no I/O), so the whole GUI is testable by asserting on returned Cmds.

#include "hand/gui/model.hpp"

#include <utility>

namespace hand {

namespace {

// Find the entry with `id` in display order; returns nullptr if gone (a late
// message from an already-closed tab is harmlessly ignored).
TabEntry *find(GuiModel &m, TabId id) {
    TabEntry *hit = nullptr;
    m.tabs().for_each_ordered([&](TabEntry &e, bool, std::size_t) {
        if (e.id == id) hit = &e;
    });
    return hit;
}

bool is_focus(const GuiModel &m, TabId id) { return m.tabs().focus().id == id; }

} // namespace

GuiCmds gui_update(GuiModel &m, GuiMsg msg) {
    GuiCmds cmds;

    std::visit(
        [&](auto &&e) {
            using T = std::decay_t<decltype(e)>;

            // ---- messages from tab actor threads ----------------------------
            if constexpr (std::is_same_v<T, TabOutput>) {
                if (auto *t = find(m, e.id)) {
                    t->generation = e.generation;
                    m.refresh(*t, is_focus(m, e.id));
                    if (is_focus(m, e.id)) cmds.push_back(Present{});
                }
            } else if constexpr (std::is_same_v<T, TabTitleChanged>) {
                if (auto *t = find(m, e.id)) {
                    t->title = std::move(e.title);
                    m.refresh(*t, is_focus(m, e.id));
                    if (is_focus(m, e.id)) cmds.push_back(SetWindowTitle{t->title});
                    cmds.push_back(Present{});
                }
            } else if constexpr (std::is_same_v<T, TabDirChanged>) {
                if (auto *t = find(m, e.id)) {
                    t->cwd = std::move(e.cwd);
                    m.refresh(*t, is_focus(m, e.id));
                    cmds.push_back(Present{});
                }
            } else if constexpr (std::is_same_v<T, TabCommand>) {
                if (auto *t = find(m, e.id)) {
                    t->running_cmd = e.running;
                    t->cur_cmd = std::move(e.cmd);
                    if (e.have_exit) {
                        t->have_last = true;
                        t->last_cmd = t->cur_cmd;
                        t->last_exit = e.exit_code;
                    }
                    m.refresh(*t, is_focus(m, e.id));
                    cmds.push_back(Present{}); // chrome status changed
                }
            } else if constexpr (std::is_same_v<T, TabExited>) {
                // The tab's child died: join its actor, then drop it from the
                // zipper. If it was the last tab, quit.
                cmds.push_back(KillTab{e.id});
                const bool was_focus = is_focus(m, e.id);
                (void)was_focus;
                // Move focus to the dying tab, then close it (close_focus is the
                // only removal primitive; refuses to empty).
                m.tabs().for_each_ordered([&](TabEntry &te, bool, std::size_t i) {
                    if (te.id == e.id) m.tabs().focus_index(i);
                });
                if (!m.tabs().close_focus()) {
                    m.set_quitting();
                    cmds.push_back(Quit{});
                } else {
                    cmds.push_back(Present{});
                }
            }

            // ---- window / user ---------------------------------------------
            else if constexpr (std::is_same_v<T, WinResized>) {
                cmds.push_back(Present{});
            } else if constexpr (std::is_same_v<T, WinFocus>) {
                cmds.push_back(Present{});
            } else if constexpr (std::is_same_v<T, WinCloseReq>) {
                m.set_quitting();
                cmds.push_back(Quit{});
            } else if constexpr (std::is_same_v<T, Tick>) {
                m.set_frame(e.frame);
                // Keep the focused tab's derived state fresh (spinner/pulse).
                m.refresh(m.tabs().focus(), /*active=*/true);
                cmds.push_back(Present{});
            }

            // ---- tab management --------------------------------------------
            else if constexpr (std::is_same_v<T, NewTab>) {
                const TabId id = m.mint_id();
                const std::string cwd = m.tabs().focus().cwd; // open beside, same dir
                m.tabs().insert_after_focus(TabEntry{id});
                cmds.push_back(SpawnTab{cwd});
                cmds.push_back(Present{});
            } else if constexpr (std::is_same_v<T, CloseTab>) {
                const TabId id = m.tabs().focus().id;
                cmds.push_back(KillTab{id});
                if (!m.tabs().close_focus()) {
                    m.set_quitting();
                    cmds.push_back(Quit{});
                } else {
                    cmds.push_back(Present{});
                }
            } else if constexpr (std::is_same_v<T, NextTab>) {
                m.tabs().focus_next_cyclic();
                m.refresh(m.tabs().focus(), true);
                cmds.push_back(Present{});
            } else if constexpr (std::is_same_v<T, PrevTab>) {
                m.tabs().focus_prev_cyclic();
                m.refresh(m.tabs().focus(), true);
                cmds.push_back(Present{});
            } else if constexpr (std::is_same_v<T, FocusTabAt>) {
                m.tabs().focus_index(e.index);
                m.refresh(m.tabs().focus(), true);
                cmds.push_back(Present{});
            } else if constexpr (std::is_same_v<T, ForwardBytes>) {
                cmds.push_back(SendToTab{m.tabs().focus().id, std::move(e.bytes)});
            }
        },
        std::move(msg));

    return cmds;
}

} // namespace hand
