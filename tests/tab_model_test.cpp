// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Unit tests for hand::TabModel — the pure status-derivation brain of Activity
// Tabs. No windowing, no engine: feed synthetic TabSignals, assert the derived
// label / status / attention. Covers the innovative behaviours: auto-labeling
// from the running command, the "done in a background tab" attention latch, and
// the unseen-output dot.

#include "hand/tab_model.hpp"

#include <cstdio>

using hand::TabAttention;
using hand::TabModel;
using hand::TabSignal;
using hand::TabStatus;

static int fails = 0;
static void ck(bool ok, const char *name) {
    if (!ok) {
        std::printf("FAIL %s\n", name);
        ++fails;
    }
}

int main() {
    // Idle at a prompt: label is the cwd basename, status Idle.
    {
        TabModel m;
        TabSignal s;
        s.cwd = "/home/ayush/projects/api";
        m.update(s, /*active=*/true);
        ck(m.status() == TabStatus::Idle, "idle status");
        ck(m.label() == "api", "idle label = cwd basename");
        ck(m.attention() == TabAttention::None, "idle no attention");
    }

    // Running a command: spinner status, label shows program + ▸.
    {
        TabModel m;
        TabSignal s;
        s.cwd = "/home/ayush/api";
        s.running = true;
        s.running_cmd = "npm test --watch";
        s.running_ms = 3400;
        m.update(s, true);
        ck(m.status() == TabStatus::Running, "running status");
        ck(m.label().find("npm") != std::string::npos, "running label has program");
        ck(m.label().find("3s") != std::string::npos, "running label shows elapsed >=1s");
    }

    // Finished OK while ACTIVE: no attention (you saw it), status Ok.
    {
        TabModel m;
        TabSignal s;
        s.cwd = "/home/ayush/api";
        s.have_last = true;
        s.last_cmd = "make";
        s.last_exit = 0;
        m.update(s, /*active=*/true);
        ck(m.status() == TabStatus::Ok, "ok status");
        ck(m.attention() == TabAttention::None, "finished while active -> no attention");
    }

    // THE KILLER FEATURE: a command finishes while the tab is in the BACKGROUND
    // -> attention latches (DoneOk / DoneFail) until visited.
    {
        TabModel m;
        TabSignal s;
        s.cwd = "/home/ayush/api";
        // Frame 1: idle in the background (establish baseline).
        m.update(s, /*active=*/false);
        ck(m.attention() == TabAttention::None, "bg idle -> no attention yet");
        // Frame 2: a build finishes successfully while still in the background.
        s.have_last = true;
        s.last_cmd = "cargo build";
        s.last_exit = 0;
        m.update(s, /*active=*/false);
        ck(m.attention() == TabAttention::DoneOk, "bg command done OK -> DoneOk alert");
        // Frame 3: the user switches TO this tab -> alert clears.
        m.update(s, /*active=*/true);
        ck(m.attention() == TabAttention::None, "visiting the tab clears the alert");
    }

    // Failure in a background tab pulses DoneFail with the exit code in the label.
    {
        TabModel m;
        TabSignal s;
        s.cwd = "/home/ayush/api";
        m.update(s, false); // baseline
        s.have_last = true;
        s.last_cmd = "pytest";
        s.last_exit = 1;
        m.update(s, false);
        ck(m.status() == TabStatus::Failed, "failed status");
        ck(m.attention() == TabAttention::DoneFail, "bg failure -> DoneFail alert");
        ck(m.label().find("(1)") != std::string::npos, "failed label shows exit code");
    }

    // Unseen-output dot: generation bumps while inactive light the dot; going
    // active clears it.
    {
        TabModel m;
        TabSignal s;
        s.cwd = "/x";
        s.generation = 5;
        m.update(s, /*active=*/true); // establish seen baseline
        ck(!m.unseen(), "active tab: no unseen dot");
        s.generation = 9; // output arrived while we're away
        m.update(s, /*active=*/false);
        ck(m.unseen(), "background output lights the unseen dot");
        m.update(s, /*active=*/true);
        ck(!m.unseen(), "revisiting clears the unseen dot");
    }

    // A second distinct completion re-triggers attention (not deduped forever).
    {
        TabModel m;
        TabSignal s;
        s.cwd = "/x";
        m.update(s, false);
        s.have_last = true; s.last_cmd = "a"; s.last_exit = 0;
        m.update(s, false);
        ck(m.attention() == TabAttention::DoneOk, "first completion alerts");
        m.update(s, true); // seen, cleared
        s.last_cmd = "b"; s.last_exit = 3; // a NEW command finished, again in bg
        m.update(s, false);
        ck(m.attention() == TabAttention::DoneFail, "second distinct completion re-alerts");
    }

    // Dead child: status Dead.
    {
        TabModel m;
        TabSignal s;
        s.alive = false;
        m.update(s, true);
        ck(m.status() == TabStatus::Dead, "exited child -> Dead status");
    }

    std::printf(fails ? "%d TAB MODEL TEST(S) FAILED\n" : "ALL TAB MODEL TESTS PASS\n", fails);
    return fails ? 1 : 0;
}
