// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Unit tests for the pure GUI reducer gui_update. Zero threads, zero windowing:
// feed GuiMsgs, assert the model transition + the emitted GuiCmds. This is the
// whole GUI control flow (tab lifecycle, focus, input routing, quit) proven as
// pure data.

#include "hand/gui/model.hpp"

#include <cstdio>

using namespace hand;

static int fails = 0;
static void ck(bool ok, const char *n) {
    if (!ok) { std::printf("FAIL %s\n", n); ++fails; }
}

template <class C>
static bool has(const GuiCmds &cmds) {
    for (auto &c : cmds) if (std::holds_alternative<C>(c)) return true;
    return false;
}
template <class C>
static const C *get(const GuiCmds &cmds) {
    for (auto &c : cmds) if (auto *p = std::get_if<C>(&c)) return p;
    return nullptr;
}

int main() {
    GuiModel m{TabId{1}};
    ck(m.tabs().size() == 1, "born with one tab");

    // NewTab: inserts + focuses a new tab, emits SpawnTab + Present.
    {
        auto cmds = gui_update(m, NewTab{});
        ck(m.tabs().size() == 2, "NewTab grows the workspace");
        ck(has<SpawnTab>(cmds), "NewTab emits SpawnTab");
        ck(has<Present>(cmds), "NewTab repaints");
    }
    const TabId t2 = m.tabs().focus().id;

    // Another tab, then navigation is pure focus movement + repaint.
    gui_update(m, NewTab{});
    ck(m.tabs().size() == 3, "three tabs");
    {
        auto cmds = gui_update(m, PrevTab{});
        ck(m.tabs().focus().id == t2, "PrevTab moves focus to the middle tab");
        ck(has<Present>(cmds), "PrevTab repaints");
    }

    // ForwardText routes input to the FOCUSED tab as SendToTab.
    {
        auto cmds = gui_update(m, ForwardText{"ls\n"});
        const auto *s = get<SendToTab>(cmds);
        ck(s != nullptr, "ForwardText emits SendToTab");
        ck(s && s->id == m.tabs().focus().id, "input routed to the focused tab");
        ck(s && s->bytes == "ls\n", "input bytes carried through");
    }

    // A background tab's command finishing latches attention (the innovation),
    // and does NOT steal focus.
    {
        // t1 is the first tab; focus is currently t2. Send a completed command
        // to t1 (a background tab).
        const TabId t1{1};
        // establish a baseline signal for t1 first.
        gui_update(m, TabCommand{t1, /*running=*/true, "make", false, 0});
        auto cmds = gui_update(m, TabCommand{t1, /*running=*/false, "make", true, 0});
        ck(m.tabs().focus().id == t2, "background completion does not steal focus");
        // find t1 and check its attention latched DoneOk.
        TabAttention att = TabAttention::None;
        m.tabs().for_each_ordered([&](TabEntry &e, bool, std::size_t) {
            if (e.id == t1) att = e.model.attention();
        });
        ck(att == TabAttention::DoneOk, "background tab latches DoneOk attention");
        ck(has<Present>(cmds), "status change repaints the chrome");
    }

    // TabExited on a non-last tab: KillTab + close + Present, no quit.
    {
        const std::size_t before = m.tabs().size();
        auto cmds = gui_update(m, TabExited{m.tabs().focus().id, 0});
        ck(has<KillTab>(cmds), "TabExited joins the actor via KillTab");
        ck(m.tabs().size() == before - 1, "exited tab removed");
        ck(!has<Quit>(cmds), "still tabs left => no quit");
    }

    // Close down to the last tab, then closing it quits.
    while (m.tabs().size() > 1) gui_update(m, CloseTab{});
    ck(m.tabs().size() == 1, "one tab remains");
    {
        auto cmds = gui_update(m, CloseTab{});
        ck(has<Quit>(cmds), "closing the last tab quits");
        ck(m.quitting(), "model marked quitting");
    }

    // WinCloseReq always quits.
    {
        GuiModel m2{TabId{1}};
        auto cmds = gui_update(m2, WinCloseReq{});
        ck(has<Quit>(cmds) && m2.quitting(), "window close request quits");
    }

    // Tick advances the frame + repaints (drives spinner/pulse).
    {
        GuiModel m3{TabId{1}};
        auto cmds = gui_update(m3, Tick{42});
        ck(m3.frame() == 42, "Tick advances the frame counter");
        ck(has<Present>(cmds), "Tick repaints");
    }

    std::printf(fails ? "%d GUI UPDATE TEST(S) FAILED\n" : "ALL GUI UPDATE TESTS PASS\n", fails);
    return fails ? 1 : 0;
}
