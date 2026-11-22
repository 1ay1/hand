// hand::view — the window's widget tree: the tab strip over the terminal.
//
//   tabbed_window
//   ├── tab_strip            one TermTabInfo per session         (view/tabs.cpp)
//   └── the terminal         terminal_view + its overlays        (view/terminal.cpp)
//       └── file_pane        the tab's directory, when open (ctrl+shift+e)
//
// Plus what floats over the whole window -- dialog, toasts, host picker,
// search, resize readout (view/overlays.cpp) -- and the settings window
// (view/settings.cpp). Nothing here draws: it turns the model into widget
// props and composes mayag widgets.

#include "view.hpp"

#include <mayag/widget/terminal/window.hpp>
#include <mayag/widget/file_pane.hpp>
#include <mayag/widget/tab_strip.hpp>

namespace hand {

using namespace mayag;
using namespace mayag::dsl;

Node App::view(const Model& m) {
    const Ctx& c = ctx();
    view::overlays(c, m);
    Node body = view::terminal(c, m);
    if (m.pane.open) {   // the active tab's files, beside its terminal
        Node pane = file_pane(c, "files", m.pane.ui, detail::pane_props(m),
                              [](FilePaneOp op) -> App::Msg { return App::PaneOp{std::move(op)}; },
                              [](FilePaneResize r) -> App::Msg { return App::PaneResize{r.begin, r.grow, r.end}; });
        body = (m.cfg.files_left ? h(node(std::move(pane)), node(std::move(body))) : h(node(std::move(body)), node(std::move(pane))))
               | width(m.win.x) | height(m.size.y);
    }
    const float full = std::clamp(m.cfg.tab_height, 22.0f, 60.0f);
    TabbedWindowProps wp{.size = m.win, .strip_full = full, .strip_t = m.strip.t, .bottom = m.cfg.tab_bottom,
                         .switch_t = m.strip.switch_t, .switch_dir = m.strip.switch_dir};
    Node strip = m.strip.t > 0.0f ? view::tabs(c, m, full) : Node{};
    // no strip and no system title bar: the window's buttons, small, in the corner
    if (detail::corner_buttons(m)) corner_controls(c, "corner", detail::corner_props(m));
    return tabbed_window(c, wp, std::move(strip), std::move(body));
}

}  // namespace hand
