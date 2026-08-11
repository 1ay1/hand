// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Unit test for the SHARED chord classifier (backend_base classify_chord). This
// is the ONE place key bindings are decided; proving it here means every
// backend that produces the neutral toe::KeyEvent gets identical, correct
// chords — no per-backend key logic, no drift.

#include "hand/platform/backend_base.hpp"

#include <cstdio>

using hand::platform::Chord;
using hand::platform::classify_chord;
using toe::KeyEvent;
using toe::Modifiers;
using toe::SpecialKey;
using toe::TextInput;

static int fails = 0;
static void ck(bool ok, const char *n) {
    if (!ok) { std::printf("FAIL %s\n", n); ++fails; }
}

static KeyEvent text_key(const char *s, bool ctrl, bool shift, bool alt = false) {
    KeyEvent k;
    k.key = TextInput{std::string{s}};
    k.mods = Modifiers{ctrl, alt, shift};
    return k;
}

int main() {
    // Ctrl+Shift+T -> NewTab, case-insensitive (backends fold Ctrl to lower).
    ck(classify_chord(text_key("t", true, true)) == Chord::NewTab, "ctrl+shift+t -> NewTab");
    ck(classify_chord(text_key("T", true, true)) == Chord::NewTab, "ctrl+shift+T -> NewTab");
    // Ctrl+Shift+W -> CloseTab.
    ck(classify_chord(text_key("w", true, true)) == Chord::CloseTab, "ctrl+shift+w -> CloseTab");
    // Ctrl+Shift+F -> OpenSearch.
    ck(classify_chord(text_key("f", true, true)) == Chord::OpenSearch, "ctrl+shift+f -> OpenSearch");
    // Ctrl+Shift+, / < -> ToggleSettings; ? / / -> ToggleHelp.
    ck(classify_chord(text_key(",", true, true)) == Chord::ToggleSettings, "ctrl+shift+, settings");
    ck(classify_chord(text_key("?", true, true)) == Chord::ToggleHelp, "ctrl+shift+? help");

    // Ctrl+Tab / Ctrl+Shift+Tab cycle.
    {
        KeyEvent tab; tab.key = SpecialKey::Tab; tab.mods = Modifiers{true, false, false};
        ck(classify_chord(tab) == Chord::NextTab, "ctrl+tab -> NextTab");
        KeyEvent stab; stab.key = SpecialKey::Tab; stab.mods = Modifiers{true, false, true};
        ck(classify_chord(stab) == Chord::PrevTab, "ctrl+shift+tab -> PrevTab");
    }

    // NON-chords must pass through as None (reach the terminal).
    ck(classify_chord(text_key("t", false, false)) == Chord::None, "plain t -> None");
    ck(classify_chord(text_key("t", true, false)) == Chord::None, "ctrl+t (no shift) -> None");
    ck(classify_chord(text_key("a", true, true)) == Chord::None, "ctrl+shift+a -> None");
    {
        KeyEvent plain_tab; plain_tab.key = SpecialKey::Tab; plain_tab.mods = Modifiers{};
        ck(classify_chord(plain_tab) == Chord::None, "plain Tab -> None (reaches shell)");
    }

    // Punctuation chords: the backends fold Ctrl+Shift+, to '<' (or ',') and
    // Ctrl+Shift+? to '?' (or '/'). BOTH forms must classify — the X11 backend
    // used to drop non-letter ctrl combos, making these dead.
    ck(classify_chord(text_key(",", true, true)) == Chord::ToggleSettings, "ctrl+shift+, -> Settings");
    ck(classify_chord(text_key("<", true, true)) == Chord::ToggleSettings, "ctrl+shift+< -> Settings");
    ck(classify_chord(text_key("?", true, true)) == Chord::ToggleHelp, "ctrl+shift+? -> Help");
    ck(classify_chord(text_key("/", true, true)) == Chord::ToggleHelp, "ctrl+shift+/ -> Help");

    std::printf(fails ? "%d CHORD TEST(S) FAILED\n" : "ALL CHORD TESTS PASS\n", fails);
    return fails ? 1 : 0;
}
