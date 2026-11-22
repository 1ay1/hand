// hand::view — what floats over the whole window.
//
//   term_dialog     paste guard / close / quit (built while asking, kept while it fades)
//   settings_panel  over the terminal (ctrl+,)
//   ssh_hosts       the host picker (ctrl+shift+s)
//   ssh_manager     every host, organised and editable (ctrl+shift+h)
//   term_search     the scrollback search bar
//   resize_hud, toasts, hint_bar

//
// The big ones live in their own files so each compiles in parallel:
// dialog.cpp, settings_overlay.cpp, ssh_overlay.cpp.

#include "view.hpp"

#include <mayag/widget/resize_hud.hpp>
#include <mayag/widget/term_search.hpp>
#include <mayag/widget/terminal/overlays.hpp>
#include <mayag/widget/toast.hpp>

namespace hand::view {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

void overlays(const Ctx& c, const Model& m) {
    const Session& se = m.tab();
    settings_overlay(c, m);
    if (se.search_open && !se.file)
        term_search(c, "search", node_id("term"), se.search_query, false, false,
                    static_cast<int>(se.term->search_current()) - 1, static_cast<int>(se.term->search_count()));
    if (m.time - m.resized_at < 0.8 && !se.file)
        resize_hud(c, "hud", se.term->cols(), se.term->rows(),
                   std::to_string(static_cast<int>(m.size.x)) + "\u00D7" + std::to_string(static_cast<int>(m.size.y)) + "px");
    if (m.toast.card && m.time - m.toast.at < 3.0 && !m.settings.open)
        toasts(c, "toasts", {*m.toast.card}, m.size);
    if (se.hint && !se.file) hint_bar(c, "hint.tip", {.count = se.hint->hints.size(), .typed = se.hint->typed, .font_size = m.font_size, .y = m.size.y - 40.0f});
    ssh_overlays(c, m);
    dialog(c, m);
}

}  // namespace hand::view
