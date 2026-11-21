// hand::view — what floats over the whole window.
//
//   term_dialog     paste guard / close / quit (built while asking, kept while it fades)
//   settings_panel  over the terminal (ctrl+,)
//   ssh_hosts       the host picker (ctrl+shift+s)
//   ssh_manager     every host, organised and editable (ctrl+shift+h)
//   term_search     the scrollback search bar
//   resize_hud, toasts, hint_bar

#include "view.hpp"

#include <mayag/widget/resize_hud.hpp>
#include <mayag/widget/ssh.hpp>
#include <mayag/widget/ssh_manager.hpp>
#include <mayag/widget/term_dialog.hpp>
#include <mayag/widget/term_search.hpp>
#include <mayag/widget/terminal/overlays.hpp>
#include <mayag/widget/toast.hpp>

#include <filesystem>

namespace hand::view {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

namespace {

/// The paste guard: what is about to be pasted, risky lines flagged.
TermDialogSpec paste_dialog(const Ctx& c, const Model& m) {
    using T = DialogAction::Tone;
    TermDialogSpec d;
    std::string body = m.pending_paste;
    while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.pop_back();
    const auto lines = split_lines(body);
    int flagged = 0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
        const Risk r = risk_of(lines[i]);
        flagged += r.flag ? 1 : 0;
        if (i < 8) d.preview.push_back({lines[i], r.flag, r.note});
    }
    d.preview_more = std::max(static_cast<int>(lines.size()) - 8, 0);
    const std::string proc = fg_process(m);
    d.tag = flagged ? "CAREFUL" : "PASTE";
    d.tone = flagged ? c.theme.danger : c.theme.warning;
    d.title = "Paste " + std::to_string(lines.size()) + (lines.size() == 1 ? " line" : " lines") +
              (proc.empty() ? std::string{"?"} : " into " + proc + "?");
    d.body = flagged ? std::to_string(flagged) + (flagged == 1 ? " line looks" : " lines look") + " dangerous \u2014 read before it runs."
           : m.term->bracketed() ? proc + " will hold it for editing; nothing runs until you press enter."
                                 : "This program did not ask for bracketed paste: each line runs as it lands.";
    d.meta = std::to_string(lines.size()) + "L \u00B7 " + std::to_string(m.pending_paste.size()) + " bytes";
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
        const std::string what = busy_of(m.at(i), &el);
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

void dialog(const Ctx& c, const Model& m) {
    if (m.dialog.ask) {
        TermDialogSpec d = m.dialog.ask->kind == Ask::Kind::paste ? paste_dialog(c, m) : close_dialog(c, m, *m.dialog.ask);
        d.focused = static_cast<int>(m.dialog.ask->focus);
        d.remember_on = m.dialog.ask->remember;
        d.time = m.time;
        d.width = std::min(600.0f, m.size.x - 40.0f);
        m.dialog.last = std::move(d);
    }
    if (m.dialog.t <= 0.0f) return;
    TermDialogSpec d = m.dialog.last;
    d.appear = m.dialog.t;
    c.overlay(Overlay{.content = term_dialog(c, "ask", d), .id = part_id("ask", part_menu),
                      .placement = Placement::centered, .dismiss = Dismiss::never, .layer = 40,
                      .scrim = 0.45f * m.dialog.t, .passthrough = !m.dialog.ask.has_value()});
}

void settings_overlay(const Ctx& c, const Model& m) {
    if (!m.settings.open && m.settings.t <= 0.0f) return;
    const float e = 1.0f - (1.0f - m.settings.t) * (1.0f - m.settings.t);   // ease-out
    c.overlay(Overlay{
        .content = (node(settings_panel(c, "settings", setting_specs(m.cfg), m.settings.ui, settings_props(c, m),
                                        [](SettingsOp op) -> App::Msg { return App::Panel{std::move(op)}; }))
                    | opacity(e) | translate(0.0f, (1.0f - e) * 16.0f)).build(),
        .id = part_id("settings", part_menu),
        .placement = Placement::centered,
        .dismiss = m.settings.open ? Dismiss::modal : Dismiss::never,
        .layer = 10,
        .scrim = 0.35f * e,
        .passthrough = !m.settings.open});   // closing: already out of the way
}

}  // namespace

void overlays(const Ctx& c, const Model& m) {
    settings_overlay(c, m);
    if (m.search_open)
        term_search(c, "search", node_id("term"), m.search_query, false, false,
                    static_cast<int>(m.term->search_current()) - 1, static_cast<int>(m.term->search_count()));
    if (m.time - m.resized_at < 0.8)
        resize_hud(c, "hud", m.term->cols(), m.term->rows(),
                   std::to_string(static_cast<int>(m.size.x)) + "\u00D7" + std::to_string(static_cast<int>(m.size.y)) + "px");
    if (m.toast.card && m.time - m.toast.at < 3.0 && !m.settings.open)
        toasts(c, "toasts", {*m.toast.card}, m.size);
    if (m.hint) hint_bar(c, "hint.tip", {.count = m.hint->hints.size(), .typed = m.hint->typed, .font_size = m.font_size, .y = m.size.y - 40.0f});
    if (m.ssh.picker.open) {
        const auto hosts = picker_hosts(m);
        ssh_hosts(c, "hosts", m.ssh.picker.query, hosts, rank_hosts(hosts, m.ssh.picker.query.text, unix_now()), m.ssh.picker.sel,
                  SshHostsProps{.now = unix_now(), .source = m.ssh.book.config_label.empty() ? "~/.ssh/config" : m.ssh.book.config_label,
                                .action = "new tab", .width = std::min(720.0f, m.win.x - 48.0f)});
    }
    if (m.ssh.manager.open || m.ssh.manager.t > 0.0f) {
        const float e = 1.0f - (1.0f - m.ssh.manager.t) * (1.0f - m.ssh.manager.t);   // ease-out
        std::string store = (std::filesystem::path{m.ssh.sources.store}.filename()).string();
        SshManagerProps mp{.now = unix_now(),
                           .source = m.ssh.book.config_label.empty() ? "~/.ssh/config" : m.ssh.book.config_label,
                           .store = store.empty() ? "hosts.vibe" : store,
                           .width = std::min(1020.0f, m.win.x - 40.0f),
                           .height = std::min(660.0f, m.win.y - 40.0f)};
        for (const auto& f : m.ssh.book.folders) mp.folders.push_back({f.name, f.user, f.jump, f.identity, f.port});
        for (const auto& s : m.ssh.manager.imports) {
            SshImportSource is{s.name, s.where, static_cast<int>(s.hosts.size()), 0};
            for (const auto& h : s.hosts) is.fresh += m.ssh.book.find(h.alias) ? 0 : 1;
            mp.imports.push_back(std::move(is));
        }
        mp.files = m.ssh.manager.files;
        mp.tunnels_known = m.ssh.manager.ports_known;
        c.overlay(Overlay{
            .content = (node(ssh_manager(c, "mgr", picker_hosts(m), m.ssh.manager.ui, mp,
                                         [](SshManagerOp op) -> App::Msg { return App::Manager{std::move(op)}; }))
                        | opacity(e) | translate(0.0f, (1.0f - e) * 16.0f)).build(),
            .id = part_id("mgr", part_menu),
            .placement = Placement::centered,
            .dismiss = m.ssh.manager.open ? Dismiss::modal : Dismiss::never,
            .layer = 12,
            .scrim = 0.4f * e,
            .passthrough = !m.ssh.manager.open});
    }
    dialog(c, m);
}

}  // namespace hand::view
