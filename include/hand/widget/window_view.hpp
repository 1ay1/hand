#pragma once
// hand::window_view — the whole window, as one widget.
//
//   ┌ term_tabs ─────────────────────────────── ⌕  ⚙  ─ □ × ┐   tab strip, drag = move
//   │ workspace_view (pane_split / pane_frame / terminal_view) │
//   └ status_bar ───────────────────────────────────────────── ┘
//
// Composition only: every piece is a mayag widget (term_tabs, button,
// status_bar) or a hand widget built from them (workspace_view). The app's
// view() calls this and posts overlays; it builds no layout of its own.
//
// Window behaviour lives here too, inline: dragging the empty strip moves
// the window (mayag WindowAction::move), double-clicking it maximises, and
// the window buttons minimise / maximise / close -- each carried as a
// message on the control itself.

#include "prelude.hpp"
#include "files_view.hpp"
#include "workspace_view.hpp"

#include <mayag/app/effects.hpp>
#include <mayag/widget/status_bar.hpp>
#include <mayag/widget/term_tabs.hpp>

#include <string>
#include <vector>

namespace hand {

/// What the window chrome asks the app for.
namespace on_window {
struct Move {};
struct ToggleMaximize {};
struct Minimize {};
struct Close {};
struct OpenPalette {};
struct OpenSettings {};
struct ToggleFiles {};
}  // namespace on_window

struct WindowViewProps {
    std::vector<TermTab>    tabs;
    int                     active = 0;
    std::vector<StatusItem> status_left, status_right;
    bool                    window_controls = true;   ///< client-side decorations
    bool                    focused = true;
    double                  time = 0.0;
};

namespace detail {

/// A square chrome button holding one glyph: quiet at rest, lit on hover.
/// `danger` washes red on hover (the close button).
template <class Msg>
[[nodiscard]] Node chrome_button(const mayag::Ctx& c, std::string_view id, std::string_view glyph, std::string_view what,
                                 Msg msg, bool danger = false, float w = 34.0f) {
    const mayag::Theme& t = c.theme;
    const auto nid = node_id(id);
    const bool hov = c.hovered(nid), prs = c.pressed(nid);
    return (h(text_owned(std::string{glyph}) | font(t.font_size * 1.05f)
              | fg(hov ? (danger ? t.on_danger : t.text_primary) : t.text_secondary))
            | size(w, 34.0f) | center
            | bg(hov ? (danger ? t.danger : (prs ? t.surface_raised : t.tint(t.accent, 0.10f))) : t.surface.fade(0.0f))
            | transition() | pointer_cursor | role(mayag::a11y::Role::button) | label(what) | id_of(id)
            | on_click([m = std::move(msg)] { return m; })).build();
}

}  // namespace detail

template <class Model>
    requires HasWorkspace<Model> && HasFiles<Model>
[[nodiscard]] Node window_view(const mayag::Ctx& c, const Model& m, const WindowViewProps& p) {
    const mayag::Theme& t = c.theme;
    const bool nerd = t.icons == mayag::IconSet::nerd;

    // The tab strip; dragging its empty grab handle moves the window (on
    // DRAG, not press, so a double click there can still maximise), and
    // mayag term_tabs carries the handlers.
    Node tabs = term_tabs(c, "tabs", p.tabs, p.active, p.time,
                          on_drag([] { return on_window::Move{}; }),
                          on_click([](const mayag::Clicked& e) -> std::optional<on_window::ToggleMaximize> {
                              if (e.count == 2) return on_window::ToggleMaximize{};
                              return std::nullopt;
                          }));

    auto actions = h(node(detail::chrome_button(c, "win.files", nerd ? (m.files_open ? "\uF07C" : "\uF07B") : "\u2630",
                                                "files", on_window::ToggleFiles{})),
                     node(detail::chrome_button(c, "win.palette", nerd ? "\uF120" : "\u2315", "command palette", on_window::OpenPalette{})),
                     node(detail::chrome_button(c, "win.settings", nerd ? "\uF013" : "\u2699", "appearance", on_window::OpenSettings{})),
                     when(p.window_controls, node((box() | size(1, 16) | bg(var::border) | margin(0, t.spaced(6))).build())),
                     when(p.window_controls, node(detail::chrome_button(c, "win.min", "\u2013", "minimise", on_window::Minimize{}, false, 40.0f))),
                     when(p.window_controls, node(detail::chrome_button(c, "win.max", "\u25A1", "maximise", on_window::ToggleMaximize{}, false, 40.0f))),
                     when(p.window_controls, node(detail::chrome_button(c, "win.close", "\u2715", "close window", on_window::Close{}, true, 44.0f))))
                 | align(mayag::Align::center) | height(34.0f) | with_look(sheet_of(t).strip, t);

    auto top = h(node(std::move(tabs)) | grow() | min_size(0, 0), std::move(actions))
             | width(pct(100)) | height(34.0f) | align(mayag::Align::stretch);

    // Body: the files panel (when open) | splitter | the panes.
    using E = mayag::Edit<Model>;
    Node panes = workspace_view(c, m, p.focused, p.time);
    Node body = m.files_open
        ? (h(node(files_view(c, m)),
             node(split_handle(c, "files.split", mayag::Axis::horizontal))
                 | on_drag([](const mayag::Dragged& d) {
                       return E{[dx = d.delta.x](Model& mm) { mm.files_width = std::clamp(mm.files_width + dx, 160.0f, 560.0f); }};
                   })
                 | on_click([](const mayag::Clicked& k) -> std::optional<E> {
                       if (k.count != 2) return std::nullopt;
                       return E{[](Model& mm) { mm.files_width = 260.0f; }};
                   }),
             node(std::move(panes)) | grow() | height(pct(100)) | min_size(0, 0))
           | width(pct(100)) | height(pct(100))).build()
        : std::move(panes);

    return (v(std::move(top),
              node(std::move(body)) | grow() | width(pct(100)) | min_size(0, 0),
              node(status_bar(c, "status", p.status_left, p.status_right)))
            | fill_both() | bg(var::background) | opacity(p.focused ? 1.0f : 0.94f) | transition()).build();
}

}  // namespace hand
