#pragma once
// hand::cli::checks — the self-test, one suite per feature. Each suite
// drives the app like a user would (keys, clicks, drags, resizes, real shells
// on real ptys) and reports every expectation through Check.

#include "../cli.hpp"

#include <cstdio>

namespace hand::cli::checks {

/// Prints `ok` / `FAIL` per expectation and counts the failures.
class Check {
  public:
    void operator()(bool passed, const char* what) {
        std::printf("  %s  %s\n", passed ? "ok  " : "FAIL", what);
        failures_ += passed ? 0 : 1;
    }
    [[nodiscard]] int failures() const noexcept { return failures_; }

  private:
    int failures_ = 0;
};

/// A suite: drives `rt` (sized `size`) and checks through `ok`.
using Suite = void (*)(Runtime& rt, mayag::Vec2 size, Check& ok);

void pty(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< a real child on a real pty, through toe
void settings_panel(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< the settings panel: open, navigate, edit, close
void every_setting(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< every setting, stepped through the panel, takes effect
void visual_settings(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< visual settings, checked on actual pixels
void terminal_features(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< shell marks, links, search, hints, the minimap, cursor motion
void fish_integration(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< the injected shell integration in a real fish
void resize_fish(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< resize never corrupts: real fish, a drag of sizes
void wide_squeeze(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< wide output squeezed thin and back: no gap above the prompt
void wrap_spaces(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< a real space at a wrap point survives a rewrap
void selection(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< selection, copy mode, paste and its guard
void tab_list(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< Tabs keeps its invariants
void tabs(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< tabs: open, switch, move, drag, close, background output
void caches_exact(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< every cache draws what a fresh frame draws
void keystroke(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< a typed key's echo leaves nothing animating
void tui_mouse(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< the mouse in a TUI: hover (1003) and drags (1002)
void pane_drag(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< the file pane's edge follows the pointer; grids wait for the drag
void editor(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< files from the pane open in editor tabs: keys, chords, save, close
void mac(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< the Mac: command shortcuts, the input method
void ssh(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< ssh: config, hosts.vibe, the picker, the manager, reconnect
void config_roundtrip(Runtime& rt, mayag::Vec2 size, Check& ok);   ///< hand.vibe: every row survives save and load

}  // namespace hand::cli::checks
