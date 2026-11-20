// hand::App — SSH: the host picker, connecting, reconnecting.

#include "detail.hpp"

#include <hand/core/shell.hpp>
#include <hand/core/proc.hpp>
#include <hand/ssh/sftp.hpp>

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
    if (!m.cfg.demo) {
        m.book = hosts_load(m.host_src);
        m.imports = import_sources();   // PuTTY's registry and MobaXterm.ini: a few ms
    }
    m.manager.import_open = false;
    m.manager.tab = 0;
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
    return update(m, TunnelProbe{});   // tunnel status from the first frame
}

namespace {
using Sk = Sink<App::Msg>;
/// What the Files tab shows of the full listing: dotfiles only on request.
void refilter(Model& m) {
    m.files.entries.clear();
    for (const auto& e : m.files_all)
        if (m.manager.show_hidden || e.name.empty() || e.name[0] != '.') m.files.entries.push_back(e);
    m.manager.files_sel = std::clamp(m.manager.files_sel, 0, std::max(static_cast<int>(m.files.entries.size()) - 1, 0));
}
App::Cmd list_files(Model& m, const Host& h, std::string path) {
    if (m.files.alias != h.alias) m.files = SshFilesView{.alias = h.alias};
    m.files.loading = true;
    m.files.error.clear();
    if (m.cfg.demo) return {};
    return App::Cmd::task([](Sk out, Host host, HostBook book, std::string p) {
        const auto l = sftp::list(host, book, p);
        App::FilesListed r{host.alias, l.path.empty() ? p : l.path, {}, l.error};
        for (const auto& e : l.entries) r.entries.push_back({e.name, e.when, e.target, e.dir, e.link, e.size});
        out.send(App::Msg{std::move(r)});
    }, h, m.book, std::move(path));
}
std::string base_name(const std::string& p) {
    std::string s = p;
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s.substr(s.rfind('/') == std::string::npos ? 0 : s.rfind('/') + 1);
}
}  // namespace

App::Cmd App::update(Model& m, Manager p) {
    using A = SshManagerAction;
    const bool hidden_before = m.manager.show_hidden;
    p.op(m.manager);
    if (m.manager.show_hidden != hidden_before) refilter(m);
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
                h.tags = d.tags; h.forwards = d.forwards; h.command = d.command; h.folder = d.folder;
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
            case A::Kind::save_folder: {
                Folder f{a.folder.name, a.folder.user, a.folder.jump, a.folder.identity, a.folder.port};
                if (!m.cfg.demo) folder_save(m.host_src, f, a.old_alias);
                std::erase_if(m.book.folders, [&](const Folder& x) { return x.name == f.name || (!a.old_alias.empty() && x.name == a.old_alias); });
                m.book.folders.push_back(f);
                if (!a.old_alias.empty() && a.old_alias != f.name)   // a rename moves its hosts
                    for (auto& h : m.book.hosts) {
                        if (h.folder == a.old_alias) h.folder = f.name;
                        else if (h.folder.rfind(a.old_alias + "/", 0) == 0) h.folder = f.name + h.folder.substr(a.old_alias.size());
                    }
                m.toast = Toast{.title = "folder saved", .body = f.name, .kind = Toast::Kind::success};
                m.toast_at = m.time;
                break;
            }
            case A::Kind::remove_folder:
                if (!m.cfg.demo) folder_remove(m.host_src, a.old_alias);
                std::erase_if(m.book.folders, [&](const Folder& x) { return x.name == a.old_alias; });
                m.toast = Toast{.title = "folder settings removed", .body = a.old_alias + ": its hosts keep their own", .kind = Toast::Kind::info};
                m.toast_at = m.time;
                break;
            case A::Kind::import_source: {
                if (a.host < 0 || a.host >= static_cast<int>(m.imports.size())) break;
                const auto& src = m.imports[static_cast<std::size_t>(a.host)];
                int added = 0;
                for (Host h : src.hosts) {
                    if (m.book.find(h.alias)) continue;   // already here (same name): theirs wins
                    h.env = guess_env(h.alias, h.hostname);
                    if (!m.cfg.demo) hosts_save(m.host_src, h, "");
                    m.book.hosts.push_back(std::move(h));
                    ++added;
                }
                m.toast = Toast{.title = added ? "imported" : "nothing new",
                                .body = added ? std::to_string(added) + (added == 1 ? " host" : " hosts") + " from " + src.name + ", in folder " + src.name
                                              : "every " + src.name + " session is here already",
                                .kind = added ? Toast::Kind::success : Toast::Kind::info};
                m.toast_at = m.time;
                break;
            }
            case A::Kind::open_url:
                cmds.push_back(update(m, OpenUrl{a.path}));
                break;
            case A::Kind::files_open:
                if (known) cmds.push_back(list_files(m, m.book.hosts[static_cast<std::size_t>(a.host)], a.path));
                break;
            case A::Kind::files_copy_path:
                m.toast = Toast{.title = "copied", .body = a.path, .kind = Toast::Kind::info};
                m.toast_at = m.time;
                cmds.push_back(WriteClipboard{a.path});
                break;
            case A::Kind::files_terminal: {
                if (!known) break;
                // a tab that starts in that directory: cd, then the login shell
                Host h = m.book.hosts[static_cast<std::size_t>(a.host)];
                std::string q = "'";
                for (char ch : a.path) q += ch == '\'' ? std::string{"'\\''"} : std::string(1, ch);
                q += "'";
                h.command = "cd " + q + " && exec \"$SHELL\" -l";
                m.manager_open = false;
                cmds.push_back(update(m, ConnectHost{h, false}));
                break;
            }
            case A::Kind::files_get: {
                if (!known || m.cfg.demo) break;
                const Host& h = m.book.hosts[static_cast<std::size_t>(a.host)];
                m.files.busy = "Downloading " + base_name(a.path) + "\u2026";
                cmds.push_back(Cmd::task([](Sk out, Host host, HostBook book, std::string remote) {
                    const std::string dir = proc::downloads_dir();
                    const std::string err = sftp::get(host, book, {remote}, dir);
                    out.send(App::Msg{App::FilesDone{host.alias, "", "Downloaded " + base_name(remote), err, dir + "/" + base_name(remote)}});
                }, h, m.book, a.path));
                break;
            }
            case A::Kind::files_put: {
                if (!known || m.cfg.demo) break;
                cmds.push_back(Cmd::task([](Sk out, std::string alias, std::string dir) {
                    out.send(App::Msg{App::UploadPicked{alias, dir, proc::pick_files("Upload to " + dir)}});
                }, m.book.hosts[static_cast<std::size_t>(a.host)].alias, a.path));
                break;
            }
            case A::Kind::files_mkdir:
                break;   // not offered yet
        }
    }
    if (std::exchange(m.manager.close, false)) { m.manager_open = false; m.manager.editing = false; }
    return Cmd::batch(std::move(cmds));
}

App::Cmd App::update(Model& m, FilesListed r) {
    if (m.files.alias != r.alias) return {};   // another host since
    if (m.files.path != r.path) { m.manager.files_sel = 0; m.manager.files_scroll.offset.y = 0.0f; }
    m.files.path = r.path;
    m.files.loading = false;
    m.files.error = r.error;
    if (r.error.empty() || !r.entries.empty()) m.files_all = std::move(r.entries);
    refilter(m);
    return {};
}

App::Cmd App::update(Model& m, FilesDone d) {
    if (m.files.alias == d.alias) m.files.busy.clear();
    m.toast = d.error.empty() ? Toast{.title = d.title, .body = d.reveal.empty() ? d.alias : d.reveal, .kind = Toast::Kind::success}
                              : Toast{.title = "transfer failed", .body = d.error, .kind = Toast::Kind::error};
    m.toast_at = m.time;
    if (d.error.empty() && !d.refresh_path.empty() && m.files.alias == d.alias)
        if (const Host* h = m.book.find(d.alias)) return list_files(m, *h, d.refresh_path);
    return {};
}

App::Cmd App::update(Model& m, UploadPicked u) {
    if (u.files.empty()) return {};
    const Host* h = m.book.find(u.alias);
    if (!h) return {};
    if (m.files.alias == u.alias) m.files.busy = "Uploading " + std::to_string(u.files.size()) + (u.files.size() == 1 ? " file\u2026" : " files\u2026");
    return Cmd::task([](Sk out, Host host, HostBook book, std::string dir, std::vector<std::string> files) {
        const std::string err = sftp::put(host, book, files, dir);
        out.send(App::Msg{App::FilesDone{host.alias, dir, "Uploaded " + std::to_string(files.size()) + (files.size() == 1 ? " file" : " files"), err, ""}});
    }, *h, m.book, u.dir, u.files);
}

App::Cmd App::update(Model& m, TunnelProbe) {
    if (!m.manager_open || m.cfg.demo) return {};
    return Cmd::task([](Sk out) {
        bool known = true;
        App::TunnelScan s;
        for (const auto& l : proc::tcp_listeners(&known)) { s.ports.push_back(l.port); s.owners.push_back(l.process); }
        s.known = known;
        out.send(App::Msg{std::move(s)});
    });
}

App::Cmd App::update(Model& m, TunnelScan s) {
    m.listen_ports = std::move(s.ports);
    m.listen_owners = std::move(s.owners);
    m.listen_known = s.known;
    return {};
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
