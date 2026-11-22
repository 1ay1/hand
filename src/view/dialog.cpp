// hand::view — the dialog over the window: the paste guard, closing a tab,
// quitting (term_dialog; built while asking, kept while it fades out).

#include "view.hpp"

#include <mayag/widget/term_dialog.hpp>

#include <algorithm>

namespace hand::view {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

namespace {

/// The paste guard: what is about to be pasted, risky lines flagged.
TermDialogSpec paste_dialog(const Ctx& c, const Model& m, const Ask& a) {
    using T = DialogAction::Tone;
    TermDialogSpec d;
    std::string body = a.paste;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
    const auto lines = split_lines(body);
    int flagged = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Risk r = risk_of(lines[i]);
        flagged += r.flag ? 1 : 0;
        if (i < 8) d.preview.push_back({lines[i], r.flag, r.note});
    }
    d.preview_more = std::max(static_cast<int>(lines.size()) - 8, 0);
    const std::string proc = fg_process(m.tab());
    d.tag = flagged ? "CAREFUL" : "PASTE";
    d.tone = flagged ? c.theme.danger : c.theme.warning;
    d.title = "Paste " + std::to_string(lines.size()) + (lines.size() == 1 ? " line" : " lines") +
              (proc.empty() ? std::string{"?"} : " into " + proc + "?");
    d.body = flagged ? std::to_string(flagged) + (flagged == 1 ? " line looks" : " lines look") + " dangerous \u2014 read before it runs."
           : m.tab().term->bracketed() ? proc + " will hold it for editing; nothing runs until you press enter."
                                 : "This program did not ask for bracketed paste: each line runs as it lands.";
    d.meta = std::to_string(lines.size()) + "L \u00B7 " + std::to_string(a.paste.size()) + " bytes";
    d.actions = {{"cancel", "esc", T::normal}, {"one line", "\u21E7\u23CE", T::normal},
                 {"paste", "\u23CE", flagged ? T::danger : T::primary}};
    d.remember = m.cfg.paste_confirm == PasteConfirm::multiline ? "only ask for risky pastes" : "";
    return d;
}

/// Close a tab / quit: what is still running in it.
TermDialogSpec close_dialog(const Ctx& c, const Model& m, const Ask& a) {
    using T = DialogAction::Tone;
    TermDialogSpec d;
    const bool quit = a.kind == Ask::Kind::quit;
    std::vector<std::size_t> which;
    if (quit) for (std::size_t i = 0; i < m.tab_count(); ++i) which.push_back(i);
    else which.push_back(a.tab.get());
    int busy = 0;
    for (std::size_t i : which) {
        std::string el;
        const std::string what = busy_of(m, m.at(i), &el);
        if (what.empty() && quit) continue;
        busy += what.empty() ? 0 : 1;
        d.preview.push_back({"tab " + std::to_string(i + 1) + "  " + (what.empty() ? std::string{"(idle)"} : "\u276F " + what), false, el});
    }
    d.tag = quit ? "QUIT" : "CLOSE";
    d.tone = c.theme.danger;
    d.title = quit ? "Quit hand?" : "Close tab " + std::to_string(a.tab.get() + 1) + "?";
    d.body = busy == 0 ? (quit ? "Every tab closes with it." : "Nothing is running in it.")
           : busy == 1 ? "Something is still running \u2014 closing ends it."
                       : std::to_string(busy) + " tabs still have something running \u2014 closing ends them.";
    d.meta = quit ? std::to_string(m.tab_count()) + (m.tab_count() == 1 ? " tab" : " tabs") : "";
    d.actions = {{"cancel", "esc", T::normal}, {quit ? "quit" : "close tab", "\u23CE", T::danger}};
    d.remember = "don't ask again";
    return d;
}

}  // namespace

void dialog(const Ctx& c, const Model& m) {
    if (m.dialog.t <= 0.0f) return;
    const Ask& a = m.dialog.ask ? *m.dialog.ask : m.dialog.was;   // closing: the last question fades out
    TermDialogSpec d = a.kind == Ask::Kind::paste ? paste_dialog(c, m, a) : close_dialog(c, m, a);
    d.focused = static_cast<int>(a.focus);
    d.remember_on = a.remember;
    d.time = m.time;
    d.width = std::min(600.0f, m.size.x - 40.0f);
    d.appear = m.dialog.t;
    c.overlay(Overlay{.content = term_dialog(c, "ask", d), .id = part_id("ask", part_menu),
                      .placement = Placement::centered, .dismiss = Dismiss::never, .layer = 40,
                      .scrim = 0.45f * m.dialog.t, .passthrough = !m.dialog.ask.has_value()});
}

}  // namespace hand::view
