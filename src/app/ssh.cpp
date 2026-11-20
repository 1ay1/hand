// hand::App — SSH: the host picker, connecting, reconnecting.

#include "detail.hpp"

#include <hand/core/shell.hpp>

#include <mayag/widget/ssh.hpp>
#include <mayag/widget/term_dialog.hpp>
#include <mayag/widget/toast.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace hand {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

App::Cmd App::update(Model& m, OpenHosts) {
    if (m.hosts_open) { m.hosts_open = false; return {}; }
    if (m.ask.has_value() || m.settings_open) return {};
    // read the files fresh: ~/.ssh/config may have changed since launch
    m.host_src = resolve(HostSources{.read_known = m.cfg.ssh_known_hosts});
    if (!m.cfg.demo) m.book = hosts_load(m.host_src);
    m.hosts_open = true;
    m.hosts_query = {};
    m.hosts_sel = 0;
    return {};
}

App::Cmd App::update(Model& m, HostsKey k) {
    const auto ranked = picker_rank(m);
    const int n = static_cast<int>(ranked.size());
    const bool adhoc = n == 0 && parse_destination(m.hosts_query.text).has_value();
    const int total = n + (adhoc ? 1 : 0);
    const auto chosen = [&]() -> const Host* {
        if (n == 0) return nullptr;
        return &m.book.hosts[static_cast<std::size_t>(ranked[static_cast<std::size_t>(std::clamp(m.hosts_sel, 0, n - 1))])];
    };
    const Mods md = k.mods;
    if (k.key == Key::escape) { m.hosts_open = false; return {}; }
    if (k.key == Key::enter) {
        // enter: a new tab; alt+enter: here (only from a plain shell prompt)
        if (const Host* h = chosen()) return update(m, ConnectHost{*h, md.alt});
        if (adhoc) return update(m, ConnectTyped{});
        return {};
    }
    if (md.ctrl && !md.shift && k.key == Key::s) {   // pin / unpin
        if (const Host* h = chosen()) {
            const std::string alias = h->alias;
            const bool to = !h->pinned;
            if (!m.cfg.demo) hosts_set_pinned(m.host_src, *h, to);
            for (auto& x : m.book.hosts) if (x.alias == alias) x.pinned = to;
            // follow the host to its new place in the list
            const auto r2 = picker_rank(m);
            for (std::size_t i = 0; i < r2.size(); ++i)
                if (m.book.hosts[static_cast<std::size_t>(r2[i])].alias == alias) m.hosts_sel = static_cast<int>(i);
        }
        return {};
    }
    if (md.ctrl && !md.shift && k.key == Key::y) {   // the command, for a script or a teammate
        if (const Host* h = chosen()) {
            const std::string cmd = ssh_command(*h);
            m.toast = Toast{.title = "copied", .body = cmd, .kind = Toast::Kind::info};
            m.toast_at = m.time;
            return WriteClipboard{cmd};
        }
        return {};
    }
    if (md.ctrl && !md.shift && k.key == Key::e) {   // ~/.ssh/config in your editor, in a new tab
        m.hosts_open = false;
        const char* ed = std::getenv("VISUAL");
        if (!ed || !*ed) ed = std::getenv("EDITOR");
        const std::string editor = ed && *ed ? ed : "vi";
        (void)update(m, NewTab{});
        if (m.pty) {
            std::string path = m.host_src.ssh_config;
            std::string q = "'";
            for (char ch : path) q += ch == '\'' ? std::string{"'\\''"} : std::string(1, ch);
            m.pty->write(editor + " " + q + "'\r");
        }
        return {};
    }
    if (k.key == Key::down || (k.key == Key::tab && !md.shift) || (md.ctrl && k.key == Key::n)) {
        if (total > 0) m.hosts_sel = (m.hosts_sel + 1) % total;
        return {};
    }
    if (k.key == Key::up || (md.ctrl && k.key == Key::p) || (md.shift && k.key == Key::tab)) {
        if (total > 0) m.hosts_sel = (m.hosts_sel - 1 + total) % total;
        return {};
    }
    if (k.key == Key::page_down) { m.hosts_sel = std::min(m.hosts_sel + 8, std::max(total - 1, 0)); return {}; }
    if (k.key == Key::page_up) { m.hosts_sel = std::max(m.hosts_sel - 8, 0); return {}; }
    if (m.hosts_query.handle_key(k.key, md)) m.hosts_sel = 0;
    return {};
}

App::Cmd App::update(Model& m, ConnectHost c) {
    m.hosts_open = false;
    if (!m.cfg.demo) hosts_record_use(m.host_src, c.host, unix_now());
    for (auto& x : m.book.hosts)
        if (x.alias == c.host.alias) { ++x.uses; x.last = unix_now(); }
    // here: type it at this tab's prompt (scrollback and cwd stay); only when
    // the tab is a quiet local shell -- never into a running program
    if (c.here && m.pty && !m.link.launched && busy_of(m, nullptr).empty() && !remote_of(m)) {
        m.pty->write(m.term->encode_paste(ssh_command(c.host)) + "\r");
        return {};
    }
    const bool first_split = m.tab_count() == 1 && !m.cfg.strip_always();
    // beside the current tab, not at the end: it belongs to what you were doing
    const std::size_t at = m.active + 1;
    m.tabs.insert(m.tabs.begin() + static_cast<long>(at), spawn_ssh(m, c.host));
    m.switch_dir = 1;
    activate(m, at);
    m.switch_t = m.cfg.motion ? 0.0f : 1.0f;
    if (first_split && !m.cfg.motion) m.strip_t = 1.0f;
    fit(m);
    return SetTitle{c.host.alias};
}

App::Cmd App::update(Model& m, ConnectTyped) {
    auto h = parse_destination(m.hosts_query.text);
    if (!h) return {};
    // a typed name that is an alias after all (the picker matched nothing fuzzy, but exact wins)
    if (const Host* b = m.book.find(h->alias)) return update(m, ConnectHost{*b});
    return update(m, ConnectHost{*h});
}

App::Cmd App::update(Model& m, ToggleManager) {
    if (m.manager_open) { m.manager_open = false; m.manager.editing = false; return {}; }
    if (m.ask.has_value()) return {};
    m.settings_open = false;
    m.hosts_open = false;
    // the files may have changed since launch (or since the picker read them)
    m.host_src = resolve(HostSources{.read_known = m.cfg.ssh_known_hosts});
    if (!m.cfg.demo) m.book = hosts_load(m.host_src);
    m.manager_open = true;
    m.manager.close = false;
    m.manager.out.clear();
    m.manager.filter.clear();
    m.manager.filtering = false;
    m.manager.editing = false;
    m.manager.confirm_delete = false;
    // start on the host this tab talks to, else the best-ranked one
    const auto hosts = picker_hosts(m);
    m.manager.sel = -1;
    if (m.remote)
        for (std::size_t i = 0; i < m.book.hosts.size(); ++i)
            if (m.book.hosts[i].alias == m.remote->alias) m.manager.sel = static_cast<int>(i);
    if (m.manager.sel < 0) {
        const auto r = rank_hosts(hosts, "", unix_now());
        m.manager.sel = r.empty() ? 0 : r.front();
    }
    return {};
}

App::Cmd App::update(Model& m, Manager p) {
    using A = SshManagerAction;
    p.op(m.manager);
    std::vector<Cmd> cmds;
    for (auto& a : std::exchange(m.manager.out, {})) {
        const bool known = a.host >= 0 && a.host < static_cast<int>(m.book.hosts.size());
        switch (a.kind) {
            case A::Kind::connect:
            case A::Kind::connect_here:
                if (!known) break;
                m.manager_open = false;
                cmds.push_back(update(m, ConnectHost{m.book.hosts[static_cast<std::size_t>(a.host)], a.kind == A::Kind::connect_here}));
                break;
            case A::Kind::pin: {
                if (!known) break;
                Host& h = m.book.hosts[static_cast<std::size_t>(a.host)];
                h.pinned = !h.pinned;
                if (!m.cfg.demo) hosts_set_pinned(m.host_src, h, h.pinned);
                break;
            }
            case A::Kind::copy_command: {
                if (!known) break;
                const std::string cmd = ssh_command(m.book.hosts[static_cast<std::size_t>(a.host)]);
                m.toast = Toast{.title = "copied", .body = cmd, .kind = Toast::Kind::info};
                m.toast_at = m.time;
                cmds.push_back(WriteClipboard{cmd});
                break;
            }
            case A::Kind::save: {
                // the draft back into a Host: a new one is hand's own; an
                // existing one keeps where it came from
                const SshHost& d = a.draft;
                Host h = known ? m.book.hosts[static_cast<std::size_t>(a.host)] : Host{};
                if (!known) h.source = HostSource::saved;
                if (h.editable()) {
                    h.alias = d.alias; h.hostname = d.hostname; h.user = d.user; h.port = d.port;
                    h.jump = d.jump; h.identity = d.identity;
                    if (h.source == HostSource::history) h.source = HostSource::saved;
                }
                h.env = static_cast<HostEnv>(d.env);
                h.env_set = d.env != SshHost::Env::none;
                if (!h.env_set) h.env = guess_env(h.alias, h.hostname);
                h.tags = d.tags; h.forwards = d.forwards; h.command = d.command;
                if (!m.cfg.demo) hosts_save(m.host_src, h, a.old_alias);
                if (known) m.book.hosts[static_cast<std::size_t>(a.host)] = h;
                else m.book.hosts.push_back(h);
                // stay on it (a rename or a new host moves it in the list)
                for (std::size_t i = 0; i < m.book.hosts.size(); ++i)
                    if (m.book.hosts[i].alias == h.alias) m.manager.sel = static_cast<int>(i);
                m.toast = Toast{.title = known ? "saved" : "host added", .body = h.alias, .kind = Toast::Kind::success};
                m.toast_at = m.time;
                break;
            }
            case A::Kind::remove: {
                if (!known) break;
                const Host h = m.book.hosts[static_cast<std::size_t>(a.host)];
                if (!m.cfg.demo) hosts_remove(m.host_src, h.alias);
                if (h.editable()) {
                    m.book.hosts.erase(m.book.hosts.begin() + a.host);
                    m.manager.sel = std::min(m.manager.sel, static_cast<int>(m.book.hosts.size()) - 1);
                } else {
                    // a ~/.ssh/config host stays; only hand's notes on it go
                    Host& x = m.book.hosts[static_cast<std::size_t>(a.host)];
                    x.pinned = false; x.tags.clear(); x.forwards.clear(); x.command.clear();
                    x.env_set = false; x.env = guess_env(x.alias, x.hostname);
                }
                m.toast = Toast{.title = h.editable() ? "host deleted" : "notes forgotten", .body = h.alias, .kind = Toast::Kind::info};
                m.toast_at = m.time;
                break;
            }
        }
    }
    if (std::exchange(m.manager.close, false)) { m.manager_open = false; m.manager.editing = false; }
    return Cmd::batch(std::move(cmds));
}

App::Cmd App::update(Model& m, Reconnect) {
    if (!m.link.dropped || !m.remote) return {};
    (void)m.term->feed("\r\n\x1b[2m\u21C4 reconnecting to " + m.remote->alias + "\u2026\x1b[0m\r\n");
    m.sid = m.next_sid++;   // a new reader stream for the new pty
    start_ssh(m, m, *m.remote);
    m.grid_key = 0;
    settle_scroll(m);
    return {};
}

}  // namespace hand
