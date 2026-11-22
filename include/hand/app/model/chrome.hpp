#pragma once
// hand::model — the window's chrome: the tab strip, the minimap, the settings
// panel, the dialog and the toast. Each owns its eased 0..1 `t` and whatever
// it must remember between frames; the widgets themselves live in mayag.

#include <hand/core/types.hpp>

#include <mayag/widget/settings_panel.hpp>
#include <mayag/widget/tab_strip.hpp>
#include <mayag/widget/term_dialog.hpp>
#include <mayag/widget/toast.hpp>

#include <cstdint>
#include <optional>

namespace hand {

/// The tab strip: shown from the second tab on, easing in.
struct StripModel {
    mayag::dsl::TabStripMotion motion;     ///< the sliding pill
    float  t = 0.0f;                       ///< the strip, 0..1
    float  switch_t = 1.0f;                ///< the content's slide on a tab switch, 0..1
    int    switch_dir = 0;                 ///< -1 came from the right, +1 from the left
    int    drag_tab = -1;                  ///< a tab being dragged to reorder (-1: none)
    float  drag_dx = 0.0f;                 ///< the pointer's offset from the dragged tab's slot
    float  drag_shift = 0.0f;              ///< slots the tab moved this drag, in px (travel - shift = drag_dx)
    bool   show_numbers = false;           ///< alt held: alt+N hints on the tabs
    double numbers_at = -10.0;
};

/// The scrollback minimap: open while the pointer is near the right edge.
struct MinimapModel {
    bool  open = false;
    float width = 14.0f;                   ///< eased toward rest / open width
    float hover_y = -1.0f;                 ///< pointer over it (terminal y), -1 = not
    float peek_t = 0.0f;                   ///< the peek card, 0..1
};

/// The settings panel (ctrl+,): settings_panel owns the interaction, we own its state.
struct SettingsModel {
    bool                           open = false;
    mayag::dsl::SettingsPanelState ui;
    float                          t = 0.0f;   ///< 0..1, eased in and out
};

/// The confirmation dialog: one question at a time.
struct Ask {
    enum class Kind : std::uint8_t { paste, close_tab, quit };
    Kind        kind = Kind::paste;
    TabIndex    tab{0};             ///< close_tab: which
    std::size_t focus = 0;          ///< the focused action (0 = cancel)
    bool        remember = false;   ///< "don't ask again" ticked
    std::string paste;              ///< paste: what is about to be pasted
};

struct DialogModel {
    std::optional<Ask>                 ask;    ///< engaged: a question is open
    float                              t = 0.0f;   ///< 0..1, eased toward ask.has_value()
    Ask                                was;    ///< the last question asked (drawn while it fades out)
};

/// One notice at a time, in the corner.
struct ToastModel {
    std::optional<mayag::dsl::Toast> card;
    double                           at = -10.0;   ///< when it appeared
};

}  // namespace hand
