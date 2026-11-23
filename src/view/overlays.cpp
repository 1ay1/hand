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
#include <mayag/widget/picker.hpp>
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
    if (m.quick.open) {   // ⌘P: the project's files, fuzzy-found
        const auto ranked = quick_open_ranked(m);
        std::vector<PickerItem> items;
        Ranked rk;
        for (const auto& [i, fm] : ranked) {
            const std::string& f = m.quick.files[static_cast<std::size_t>(i)];
            const auto slash = f.find_last_of('/');
            const std::string name = slash == std::string::npos ? f : f.substr(slash + 1);
            // the name, its folder dimmed after it; matched letters in the name only
            FuzzyMatch shown;
            for (auto pos : fm.positions)
                if (slash == std::string::npos || pos > slash) shown.positions.push_back(static_cast<std::uint16_t>(slash == std::string::npos ? pos : pos - slash - 1));
            bool open_tab = false;
            for (std::size_t t = 0; t < m.tab_count(); ++t) open_tab = open_tab || (m.at(t).file && m.at(t).file->path == m.quick.root + "/" + f);
            rk.order.push_back(static_cast<int>(items.size()));
            rk.matches.push_back(std::move(shown));
            items.push_back({.label = name, .detail = slash == std::string::npos ? std::string{} : f.substr(0, slash),
                             .hint = open_tab ? "open" : "", .icon_name = name});
        }
        const std::string where = m.quick.root.substr(m.quick.root.find_last_of('/') + 1);
        picker(c, "quick", m.quick.q, items, rk, m.quick.sel,
               PickerStyle{.placeholder = m.quick.loading ? "Listing " + where + "\u2026" : "Open a file in " + where, .prefix = "\u2192",
                           .width = std::min(680.0f, m.win.x - 80.0f),
                           .empty_text = m.quick.loading ? "Listing files\u2026" : "No file matches",
                           .footer = {{"\u23CE", "open"}, {"\u2191\u2193", "choose"}, {"esc", "close"}},
                           .noun = m.quick.truncated ? "files (the first 50k)" : "files"});
    }
    dialog(c, m);
}

}  // namespace hand::view
