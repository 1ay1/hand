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
    if (m.hud_up && !se.file)
        resize_hud(c, "hud", se.term->cols(), se.term->rows(),
                   std::to_string(static_cast<int>(m.size.x)) + "\u00D7" + std::to_string(static_cast<int>(m.size.y)) + "px");
    if (m.toast.card && !m.settings.open)   // it leaves by ToastExpired
        toasts(c, "toasts", {*m.toast.card}, m.size);
    if (se.hint && !se.file) hint_bar(c, "hint.tip", {.count = se.hint->hints.size(), .typed = se.hint->typed, .font_size = m.font_size, .y = m.size.y - 40.0f});
    ssh_overlays(c, m);
    if (m.quick.open) {   // ⌘P: the project's files, fuzzy-found
        const auto& ranked = quick_open_ranked(m);
        const auto& files = *m.quick.files;
        const auto row = [&](int k) {   // only the rows on screen: the ranked list is 200 long
            const auto& [i, fm] = ranked[static_cast<std::size_t>(k)];
            const std::string& f = files[static_cast<std::size_t>(i)];
            const auto slash = f.find_last_of('/');
            const std::string name = slash == std::string::npos ? f : f.substr(slash + 1);
            // the name, its folder dimmed after it; matched letters in the name only
            PickerRow r;
            for (auto pos : fm.positions)
                if (slash == std::string::npos || pos > slash) r.positions.push_back(static_cast<std::uint16_t>(slash == std::string::npos ? pos : pos - slash - 1));
            bool open_tab = false;
            for (std::size_t t = 0; t < m.tab_count(); ++t) open_tab = open_tab || (m.at(t).file && m.at(t).file->path == m.quick.root + "/" + f);
            r.item = {.label = name, .detail = slash == std::string::npos ? std::string{} : f.substr(0, slash),
                      .hint = open_tab ? "open" : "", .icon_name = name};
            return r;
        };
        const std::string where = m.quick.root.substr(m.quick.root.find_last_of('/') + 1) + (m.quick.host ? " on " + m.quick.host->alias : std::string{});
        picker(c, "quick", m.quick.q, PickerCount{static_cast<int>(ranked.size()), ranked.size()}, row, m.quick.sel,
               PickerStyle{.placeholder = m.quick.loading ? "Listing " + where + "\u2026" : "Open a file in " + where, .prefix = "\u2192",
                           .width = std::min(680.0f, m.win.x - 80.0f),
                           .empty_text = m.quick.loading ? "Listing files\u2026" : "No file matches",
                           .footer = {{"\u23CE", "open"}, {"\u2191\u2193", "choose"}, {"esc", "close"}},
                           .noun = m.quick.truncated ? "files (the first 50k)" : "files"});
    }
    if (m.search_all.open) {   // ⌘⇧F: every line in the project with the text
        const auto& s = m.search_all;
        const auto row = [&](int k) {   // only the rows on screen, of up to 2000 hits
            const SearchHit& h = s.hits[static_cast<std::size_t>(k)];
            // the line, its indent dropped, and cut to keep the match in view
            std::size_t a = h.text.find_first_not_of(" \t");
            if (a == std::string::npos) a = 0;
            if (static_cast<std::size_t>(h.col) > a + 60) a = static_cast<std::size_t>(h.col) - 40;   // a long line: start near the match
            while (a > 0 && a < h.text.size() && (static_cast<unsigned char>(h.text[a]) & 0xC0) == 0x80) --a;   // not mid-character
            std::string line = h.text.substr(a, 140);
            const bool cut_front = a > h.text.find_first_not_of(" \t");
            if (cut_front) line = "\u2026" + line;
            PickerRow r;
            const std::size_t at = static_cast<std::size_t>(h.col) - a + (cut_front ? 3 : 0);   // … is 3 bytes
            for (int j = 0; j < h.len; ++j) if (at + static_cast<std::size_t>(j) < line.size()) r.positions.push_back(static_cast<std::uint16_t>(at + static_cast<std::size_t>(j)));
            const auto slash = h.file.find_last_of('/');
            const std::string name = slash == std::string::npos ? h.file : h.file.substr(slash + 1);
            r.item = {.label = std::move(line), .detail = h.file, .hint = std::to_string(h.line + 1), .icon_name = name};
            return r;
        };
        // how many files the hits are in
        std::size_t files = 0;
        for (std::size_t i = 0; i < s.hits.size(); ++i) files += i == 0 || s.hits[i].file != s.hits[i - 1].file ? 1 : 0;
        const std::string where = (s.root.empty() || s.root == "." ? std::string{"the project"} : s.root.substr(s.root.find_last_of('/') + 1)) +
                                  (s.host ? " on " + s.host->alias : std::string{});
        picker(c, "search_all", s.q, PickerCount{static_cast<int>(s.hits.size()), s.hits.size()}, row, s.sel,
               PickerStyle{.placeholder = "Search the text of every file in " + where,
                           .prefix = "/",   // the search idiom (⌕ is missing from most UI fonts)
                           .width = std::min(820.0f, m.win.x - 60.0f),
                           .max_rows = 14,
                           .empty_text = s.q.text.size() < 2 ? "Type two or more characters"
                                       : s.searching ? "Searching\u2026" : !s.error.empty() ? s.error : "Nothing found",
                           .footer = {{"\u23CE", "open at the line"}, {"\u2191\u2193", "choose"}, {"esc", "close"}},
                           .noun = (s.truncated ? "results (the first 2000) in " : "results in ") + std::to_string(files) +
                                   (files == 1 ? " file" : " files") + (s.searching ? "  \u00B7  searching\u2026" : ""), .mono = true});
    }
    dialog(c, m);
}

}  // namespace hand::view
