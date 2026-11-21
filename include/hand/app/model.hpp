#pragma once
// hand::app model — everything hand knows, as plain typed data.
//
//   Session   one tab: its pty, its screen, and what animates over it  (model/session.hpp)
//   Tabs      the sessions in strip order, and which is in front     (model/tabs.hpp)
//   Model     the window: config, the tabs, and what sits over and beside
//             them -- the strip, the dialog, the settings, ssh, the file pane
//
// A mode that may be absent is an optional, never a bool next to its data:
// `copy` is engaged exactly while copy mode is on; `ask` exactly while a
// dialog is asking. What a value means is in its type.

#include <hand/app/model/chrome.hpp>
#include <hand/app/model/modes.hpp>
#include <hand/app/model/pane.hpp>
#include <hand/app/model/ssh.hpp>
#include <hand/app/model/tabs.hpp>
#include <hand/config/config.hpp>
#include <hand/core/pty.hpp>
#include <hand/core/term.hpp>
#include <hand/core/types.hpp>

#include <mayag/mayag.hpp>
#include <mayag/widget/terminal_view.hpp>

#include <cstdint>
#include <memory>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hand {

/// The window: config, its tabs, and what sits over and beside them.
struct Model {
    Config                  cfg;
    Palette                 palette;
    mayag::Vec2             win{800, 540};        ///< the window
    mayag::Vec2             size{800, 500};       ///< the terminal area (the window below the tab strip)
    float                   font_size = 13.0f;    ///< cfg.font_size, or zoomed
    bool                    focused = true;
    std::string             preedit;              ///< the input method's composition, drawn at the cursor (not yet typed)
    bool                    cursor_on = true;
    double                  time = 0.0;
    mayag::dsl::TermMetrics metrics{};            ///< cell size, from the font (fit())
    Tabs                    tabs;                 ///< every tab, in strip order; never empty
    SessionIds              session_ids;          ///< hands out each new session's id
    StripModel              strip;                ///< the tab strip
    bool                    ctrl_held = false;    ///< ctrl is down: links are armed
    MinimapModel            minimap;
    std::uint64_t           clock = 0;            ///< 1 Hz: live timers, activity samples
    double                  resized_at = -10.0;
    bool                    sized_once = false;
    ToastModel              toast;
    mutable mayag::Rect     sel_chip{};           ///< where the selection chip was drawn (terminal coords)
    // what floats over the terminal, and what sits beside it
    SettingsModel           settings;             ///< ctrl+,
    DialogModel             dialog;               ///< the one open question
    SshModel                ssh;                  ///< ctrl+shift+s picker, ctrl+shift+h manager
    PaneModel               pane;                 ///< ctrl+shift+e

    /// The tab in front: what the keyboard, the mouse and the view act on.
    [[nodiscard]] Session& tab() noexcept { return tabs.active(); }
    [[nodiscard]] const Session& tab() const noexcept { return tabs.active(); }
    [[nodiscard]] std::size_t tab_count() const noexcept { return tabs.size(); }
    [[nodiscard]] std::size_t active() const noexcept { return tabs.index(); }
    [[nodiscard]] Session& at(std::size_t i) noexcept { return tabs[i]; }
    [[nodiscard]] const Session& at(std::size_t i) const noexcept { return tabs[i]; }
    [[nodiscard]] float strip_h() const noexcept;
};

}  // namespace hand
