#pragma once
// hand::workspace_view — one tab's split tree, drawn with mayag.
//
//   leaf   -> mayag pane_frame (title strip when split, exit footer, focus)
//             around mayag terminal_view (the cell grid, in the mono face)
//   split  -> mayag pane_split (ratio, draggable divider, double-click 1:1)
//
// hand adds only the recursion and the wiring: each pane's grid comes from
// the caller, every divider and pane carries its message inline (mayag
// handlers), so the app has no ids to decode. Ids are derived from the
// stable PaneId / node index ("pane.7", "split.3").

#include "prelude.hpp"
#include "../core/theme.hpp"
#include "../core/workspace.hpp"
#include "editor_view.hpp"

#include <mayag/app/handlers.hpp>
#include <mayag/widget/pane.hpp>
#include <mayag/widget/terminal_view.hpp>

#include <functional>
#include <string>

namespace hand {

using namespace mayag::dsl;

/// What one pane shows.
struct PaneView {
    std::string               title;
    const TermGrid*           grid = nullptr;   ///< a terminal...
    const Doc*                doc = nullptr;    ///< ...or a file being edited
    bool                      exited = false;
    int                       exit_code = 0;
};

/// The model-side surface the view needs: pane lookup, and messages for
/// focus and ratio. Satisfied by hand::App's Model.
template <class Model>
concept HasWorkspace = requires(const Model& m, PaneId p) {
    { m.tab() } -> std::convertible_to<const Tab&>;
    { m.pane_view(p) } -> std::convertible_to<PaneView>;
    { m.shown() } -> std::convertible_to<Appearance>;
};

namespace detail {

[[nodiscard]] inline std::string pane_key(PaneId p) { return "pane." + std::to_string(p.v); }
[[nodiscard]] inline std::string split_key(std::uint32_t n) { return "split." + std::to_string(n); }

template <class Model>
[[nodiscard]] Node leaf(const mayag::Ctx& c, const Model& m, PaneId id, bool many, bool window_focused,
                        double time) {
    const Appearance a = m.shown();
    const Tab& t = m.tab();
    const PaneView pv = m.pane_view(id);
    const bool focused = t.focus == id && window_focused;
    const std::string key = pane_key(id);
    const double phase = static_cast<double>(a.blink_rate.get());
    const bool cursor_on = !a.cursor_blink || std::fmod(time, 2.0 * phase) < phase;
    if (pv.doc != nullptr) {
        // An editor pane: its own header strip replaces the pane title.
        Node ed = editor_view<Model>(c, *pv.doc, a, {.pane = id, .focused = focused, .cursor_on = cursor_on});
        mayag::dsl::PaneInfo info{.title = pv.title, .focused = focused, .zoomed = t.zoom == id, .show_title = false};
        return (pane_frame(c, key, info, std::move(ed))
                | selected(focused && many)
                | on_press([id] { return mayag::Edit<Model>{[id](Model& mm) { mm.focus(id); }}; })).build();
    }
    const TermTheme th = term_theme_of(a, c.theme);
    TermViewOptions o{
        .font_size = a.font_size.get(), .line_height = a.line_height.get(), .padding = a.padding.get(),
        .focused = focused, .cursor_on = cursor_on,
        .dim_unfocused = many ? a.dim_unfocused.get() : 0.0f, .marks = a.marks, .opacity = a.opacity.get(),
        .family = mayag::custom_family(a.mono.v)};
    TermGrid g = pv.grid ? *pv.grid : TermGrid{};
    g.cursor_shape = a.cursor == CursorStyle::beam ? TermCursorShape::beam
                   : a.cursor == CursorStyle::underline ? TermCursorShape::underscore : TermCursorShape::block;
    Node grid = terminal_view(c, key + ".grid", g, th, o);
    mayag::dsl::PaneInfo info{.title = pv.title, .focused = focused, .zoomed = t.zoom == id,
                              .show_title = many && a.pane_titles, .exited = pv.exited, .exit_code = pv.exit_code};
    return (pane_frame(c, key, info, std::move(grid))
            | on_press([id] { return mayag::Edit<Model>{[id](Model& mm) { mm.focus(id); }}; })).build();
}

template <class Model>
[[nodiscard]] Node subtree(const mayag::Ctx& c, const Model& m, std::uint32_t at, int depth, bool focused, double time) {
    const Tab& t = m.tab();
    if (depth > 32 || at >= t.nodes.size()) return Node{};
    const bool many = t.nodes.size() > 1;
    if (const auto* l = std::get_if<Leaf>(&t.nodes[at])) return leaf(c, m, l->pane, many, focused, time);
    const auto& s = std::get<Split>(t.nodes[at]);
    Node a = subtree(c, m, s.a, depth + 1, focused, time);
    Node b = subtree(c, m, s.b, depth + 1, focused, time);
    const bool horiz = s.axis == mayag::Axis::horizontal;
    // The divider drags the ratio; a double click evens it.
    return pane_split(c, split_key(at), s.axis, s.ratio, std::move(a), std::move(b),
                      on_drag([at, horiz](const mayag::Dragged& d) {
                          return mayag::Edit<Model>{[at, dd = horiz ? d.delta.x : d.delta.y](Model& mm) {
                              mm.nudge_split(at, dd);
                          }};
                      }),
                      on_click([at](const mayag::Clicked& k) -> std::optional<mayag::Edit<Model>> {
                          if (k.count != 2) return std::nullopt;
                          return mayag::Edit<Model>{[at](Model& mm) { mm.even_split(at); }};
                      }));
}

}  // namespace detail

/// The panes of the active tab; a zoomed pane alone.
template <class Model>
    requires HasWorkspace<Model>
[[nodiscard]] Node workspace_view(const mayag::Ctx& c, const Model& m, bool window_focused, double time) {
    const Tab& t = m.tab();
    Node body = t.zoom ? detail::leaf(c, m, *t.zoom, true, window_focused, time)
                       : detail::subtree(c, m, t.root, 0, window_focused, time);
    return (v(std::move(body)) | grow() | fill_both() | min_size(0, 0)).build();
}

}  // namespace hand
