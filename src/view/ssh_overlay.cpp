// hand::view — ssh over the window: the host picker (ctrl+shift+s) and the
// host manager (ctrl+shift+h).

#include "view.hpp"

#include <mayag/widget/ssh.hpp>
#include <mayag/widget/ssh_manager.hpp>

#include <filesystem>

namespace hand::view {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

void ssh_overlays(const Ctx& c, const Model& m) {
    if (m.ssh.picker.open) {
        const auto hosts = picker_hosts(m);
        ssh_hosts(c, "hosts", m.ssh.picker.query, hosts, rank_hosts(hosts, m.ssh.picker.query.text, unix_now(m)), m.ssh.picker.sel,
                  SshHostsProps{.now = unix_now(m), .source = m.ssh.book.config_label.empty() ? "~/.ssh/config" : m.ssh.book.config_label,
                                .action = "new tab", .width = std::min(720.0f, m.win.x - 48.0f), .keymap = m.env.keymap});
    }
    if (m.ssh.manager.open || m.ssh.manager.t > 0.0f) {
        const float e = 1.0f - (1.0f - m.ssh.manager.t) * (1.0f - m.ssh.manager.t);   // ease-out
        const std::string& sp = m.ssh.sources.store;   // hosts.vibe's file name
        const std::string store = sp.substr(sp.find_last_of("/\\") == std::string::npos ? 0 : sp.find_last_of("/\\") + 1);
        SshManagerProps mp{.now = unix_now(m),
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
}

}  // namespace hand::view
