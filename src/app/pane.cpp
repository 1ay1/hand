// hand::App — the file pane beside the terminal (ctrl+shift+e).
//
// It shows the active tab's directory: the local shell's (OSC 7: hand's
// fish / zsh / bash / PowerShell integration reports every cd), or, for an
// ssh tab, the remote machine's over SFTP (the remote shell's OSC 7 if it
// sends one, else the login directory). It follows the shell until you
// browse elsewhere; "follow" brings it back. Listings run as tasks: a
// network share or a slow link never stalls a frame.

#include "detail.hpp"
#include "fx/fx.hpp"


#include <mayag/widget/toast.hpp>

#include <algorithm>
#include <cctype>

namespace hand {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

namespace {

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

/// Is `host` (from OSC 7) this machine?
bool is_here(const Model& m, const std::string& host) {
    if (host.empty() || host == "localhost") return true;
    const std::string me = lower(m.env.host_name);
    const std::string h = lower(host);
    return h == me || h.rfind(me + ".", 0) == 0;
}

struct Place {
    std::string where;              ///< "" local, else an alias
    std::optional<Host> host;
    std::string dir;                ///< the shell's directory ("" = unknown: login dir / start dir)
    std::string note;
};

/// Where the active tab's shell is.
Place place_of(const Model& m) {
    const Session& se = m.tab();
    Place p;
    if (se.file) {   // a file tab: the pane stays on the file's folder, on its machine
        const auto s = se.file->path.find_last_of("/\\");
        p.dir = s == std::string::npos ? m.env.cwd : se.file->path.substr(0, s == 0 ? 1 : s);
        if (se.file->host) { p.where = se.file->host->alias; p.host = se.file->host; }
        return p;
    }
    const auto& r = remote_of(m.tab());
    const std::string osc_host = se.term ? se.term->cwd_host() : std::string{};
    const std::string osc_dir = se.term ? se.term->cwd() : std::string{};
    if (r && !se.link.dropped) {
        p.where = r->alias;
        p.host = m.ssh.book.find(r->alias) ? *m.ssh.book.find(r->alias) : *r;
        // a remote shell's OSC 7 names its own host; a stale local one does not count
        if (!osc_dir.empty() && !is_here(m, osc_host)) p.dir = osc_dir;
        else p.note = "The remote shell does not report its directory: showing where you log in. Add OSC 7 to its prompt to follow cd.";
        return p;
    }
    if (!osc_dir.empty() && is_here(m, osc_host)) p.dir = osc_dir;
    else {
        p.dir = m.env.cwd;
        p.note = "This shell does not report its directory (fish, zsh, bash and PowerShell do with shell integration): showing where it started.";
    }
    return p;
}


/// The path as a shell argument: PowerShell, cmd and POSIX quote differently.
std::string shell_quote(const Model& m, const std::string& path) {
    if (m.pane.where.empty()) {
#if defined(_WIN32)
        const std::string sh = lower((m.cfg.shell.empty() ? m.env.shell : m.cfg.shell));
        if (sh.find("cmd") != std::string::npos) return "\"" + path + "\"";
        std::string q = "'";   // PowerShell: '' is a quote
        for (char ch : path) q += ch == '\'' ? std::string{"''"} : std::string(1, ch);
        return q + "'";
#endif
    }
    std::string q = "'";
    for (char ch : path) q += ch == '\'' ? std::string{"'\\''"} : std::string(1, ch);
    return q + "'";
}

std::string cd_command(const Model& m, const std::string& path) {
#if defined(_WIN32)
    if (m.pane.where.empty() && lower((m.cfg.shell.empty() ? m.env.shell : m.cfg.shell)).find("cmd") != std::string::npos)
        return "cd /d " + shell_quote(m, path);
#endif
    return "cd " + shell_quote(m, path);
}

}  // namespace

namespace detail {

FilePaneProps pane_props(const Model& m) {
    FilePaneProps p;
    p.path = m.pane.path;
    p.where = m.pane.where;
    p.where_danger = m.pane.host && m.cfg.ssh_prod_guard && remote_env(m, *m.pane.host) == HostEnv::prod;
    p.entries = m.pane.entries;
    for (auto& e : p.entries)   // the glow fades over ~2.5 s
        if (const auto it = m.pane.fresh_at.find(e.name); it != m.pane.fresh_at.end())
            e.fresh = std::clamp(1.0f - static_cast<float>((m.time - it->second) / 2.5), 0.0f, 1.0f);
    p.loading = m.pane.loading;
    p.following = m.pane.follow;
    p.error = m.pane.error;
    p.busy = m.pane.busy;
    p.note = m.pane.note;
    p.width = m.pane.width;
    p.left = m.cfg.files_left;
    p.now = unix_now(m);
    p.dirs_first = m.cfg.files_dirs_first;
    p.git = m.cfg.files_git;
    p.size_bars = m.cfg.files_size_bars;
    p.age = m.cfg.files_age;
    p.type_colors = m.cfg.files_type_colors;
    p.preview_lines = static_cast<int>(m.cfg.files_preview_lines);
    // the list is terminal text -- the terminal's face and size, zoom included;
    // the pane's header and toolbar are interface text
    p.text_size = m.font_size;
    p.family = family_of(m.cfg);
    if (m.cfg.files_preview && m.pane.preview && !m.pane.preview_path.empty()) p.preview = m.pane.preview;
    std::string shown = m.pane.path;
    if (m.pane.where.empty()) {   // ~ for home, as the shell writes it
        std::string home = m.env.home;
        for (char& ch : home) if (ch == '\\') ch = '/';
        while (home.size() > 1 && home.back() == '/') home.pop_back();
        p.home = home;
        if (!home.empty() && lower(shown).rfind(lower(home), 0) == 0 && (shown.size() == home.size() || shown[home.size()] == '/'))
            shown = "~" + shown.substr(home.size());
        p.title = shown;
    } else {
        p.title = m.pane.where + ":" + shown;
    }
    return p;
}

bool pane_glowing(const Model& m) {
    for (const auto& [name, at] : m.pane.fresh_at)
        if (m.time - at < 2.5) return true;
    return false;
}

}  // namespace detail

/// List `dir` where the pane looks (a task), and show it. `quiet`: a watch
/// re-read of the same folder (no spinner; what changed glows).
static App::Cmd pane_list(Model& m, std::string dir, bool quiet = false) {
    if (!quiet) {
        if (dir != m.pane.path) { m.pane.fresh_at.clear(); m.pane.ui.filter.clear(); m.pane.ui.filtering = false; }
        m.pane.path = dir;
        m.pane.loading = true;
        m.pane.error.clear();
    }
    const std::uint64_t token = ++m.pane.token;
    if (m.pane.where.empty())   // read-only, so even demo shots list for real
        return App::Cmd::task(fx::list_local, token, std::move(dir), m.cfg.files_git, quiet);
    if (!m.pane.host || m.cfg.demo) return {};
    return App::Cmd::task(fx::list_remote_pane, token, *m.pane.host, m.ssh.book, std::move(dir), quiet);
}

/// The selection's preview: read it (a task) when the selection moved.
static App::Cmd pane_preview(Model& m) {
    if (!m.pane.open || !m.cfg.files_preview || !m.pane.where.empty()) {
        m.pane.preview.reset(); m.pane.preview_path.clear();
        return {};
    }
    const auto vis = file_pane_view(m.pane.entries, m.pane.ui, m.cfg.files_dirs_first);
    if (vis.empty() || !m.pane.ui.focused) { m.pane.preview.reset(); m.pane.preview_path.clear(); return {}; }
    const auto& e = m.pane.entries[vis[static_cast<std::size_t>(std::clamp(m.pane.ui.sel, 0, static_cast<int>(vis.size()) - 1))].i];
    const std::string path = file_pane_detail::join(m.pane.path, e.name, '/');
    if (path == m.pane.preview_path) return {};
    m.pane.preview_path = path;
    return App::Cmd::task(fx::preview_local, path, static_cast<int>(m.cfg.files_preview_lines));
}

App::Cmd App::update(Model& m, PanePreview p) {
    if (p.path != m.pane.preview_path) return {};
    m.pane.preview = std::move(p.preview);
    if (!m.pane.preview) m.pane.preview_path.clear();
    return {};
}

App::Cmd App::update(Model& m, PaneWatch) {
    if (!m.pane.open || m.pane.loading || m.pane.path.empty() || !m.cfg.files_watch) return {};
    return pane_list(m, m.pane.path, true);
}

App::Cmd App::pane_sync(Model& m) {
    if (!m.pane.open) return {};
    const Place p = place_of(m);
    const bool moved_machine = p.where != m.pane.where;
    if (moved_machine) {
        m.pane.where = p.where;
        m.pane.host = p.host;
        m.pane.follow = true;   // a new machine: start at its shell's directory
        m.pane.entries.clear();
        m.pane.ui.sel = 0;
        m.pane.ui.scroll.offset.y = 0.0f;
    }
    m.pane.note = p.note;
    if (!moved_machine && (!m.pane.follow || !m.cfg.files_follow || p.dir == m.pane.followed)) return {};
    m.pane.followed = p.dir;
    m.pane.ui.sel = 0;
    m.pane.ui.scroll.offset.y = 0.0f;
    return pane_list(m, p.dir);
}

App::Cmd App::update(Model& m, TogglePane) {
    if (m.pane.open && m.pane.ui.focused) {   // open and in use: close it
        m.pane.open = false;
        m.pane.ui.focused = false;
        fit(m);
        return {};
    }
    if (m.pane.open) { m.pane.ui.focused = true; return pane_preview(m); }
    m.pane.open = true;
    m.pane.ui.focused = true;
    m.pane.width = m.cfg.files_width;
    m.pane.ui.show_hidden = m.cfg.files_hidden;
    m.pane.ui.sort = static_cast<FileSort>(std::clamp(m.cfg.files_sort, 0, 3));
    m.pane.follow = m.cfg.files_follow || m.pane.path.empty();
    m.pane.where = "\x01";   // force a fresh look at where the shell is
    fit(m);
    return pane_sync(m);
}

App::Cmd App::update(Model& m, PaneResize r) {
    // By position: the width at the press plus the pointer's travel since,
    // so the edge stays under the pointer (fit() clamps; summing deltas would
    // carry the clamped-away pixels and drift).
    // `dragging` is exactly "the grip is held": set by the press, cleared by
    // the release, by nothing else. (A pause timer used to clear it, so a
    // pause mid-drag re-wrapped a 10k-line history -- 863k cells, ~14 ms --
    // under a pointer still held, and the next move dropped the frame.)
    if (r.begin) { m.pane.grab_width = m.pane.width; m.pane.dragging = true; return {}; }
    if (r.end) { m.pane.dragging = false; fit(m); return {}; }   // released: every grid fits now
    m.pane.width = m.pane.grab_width + r.grow;
    fit(m);   // the layout follows the pointer; grids within the frame budget too (see fit)
    m.cfg.files_width = m.pane.width;   // remembered (saved with the settings)
    return {};
}

App::Cmd App::update(Model& m, PaneListed r) {
    if (r.token != m.pane.token) return {};   // an older request
    if (r.notice == Notice::quiet) {   // a watch re-read: same folder, glow what changed
        if (!r.error.empty() || r.path != m.pane.path) return {};
        std::map<std::string, const FileEntry*> before;
        for (const auto& e : m.pane.entries) before[e.name] = &e;
        bool changed = r.entries.size() != m.pane.entries.size();
        for (const auto& e : r.entries) {
            const auto it = before.find(e.name);
            const bool fresh = it == before.end() || it->second->size != e.size || it->second->mtime != e.mtime;
            if (fresh) m.pane.fresh_at[e.name] = m.time;
            changed = changed || fresh || it->second->git != e.git;
        }
        if (!changed) return {};
        // keep the selection on the same file as rows come and go
        const auto vis = file_pane_view(m.pane.entries, m.pane.ui, m.cfg.files_dirs_first);
        std::string selected;
        if (!vis.empty()) selected = m.pane.entries[vis[static_cast<std::size_t>(std::clamp(m.pane.ui.sel, 0, static_cast<int>(vis.size()) - 1))].i].name;
        m.pane.entries = std::move(r.entries);
        const auto now_vis = file_pane_view(m.pane.entries, m.pane.ui, m.cfg.files_dirs_first);
        for (std::size_t q = 0; q < now_vis.size(); ++q)
            if (m.pane.entries[now_vis[q].i].name == selected) m.pane.ui.sel = static_cast<int>(q);
        if (m.pane.fresh_at.count(selected)) m.pane.preview_path.clear();   // re-read its preview
        return pane_preview(m);
    }
    m.pane.loading = false;
    m.pane.error = r.error;
    m.pane.path = r.path;
    if (r.error.empty() || !r.entries.empty()) m.pane.entries = std::move(r.entries);
    if (m.pane.follow && m.pane.followed.empty()) m.pane.followed = r.path;   // the login dir, resolved
    m.pane.ui.sel = std::clamp(m.pane.ui.sel, 0, std::max(static_cast<int>(m.pane.entries.size()) - 1, 0));
    m.pane.preview_path.clear();
    return pane_preview(m);
}

App::Cmd App::update(Model& m, PaneOp p) {
    using A = FilePaneAction;
    const FileSort sort_before = m.pane.ui.sort;
    const bool hidden_before = m.pane.ui.show_hidden;
    p.op(m.pane.ui);
    if (m.pane.ui.sort != sort_before) m.cfg.files_sort = static_cast<int>(m.pane.ui.sort);
    if (m.pane.ui.show_hidden != hidden_before) m.cfg.files_hidden = m.pane.ui.show_hidden;
    std::vector<Cmd> cmds;
    const auto toast = [&](std::string title, std::string body, Toast::Kind k) {
        m.toast.card = Toast{.title = std::move(title), .body = std::move(body), .kind = k};
        m.toast.at = m.time;
    };
    // typing into the shell is only safe at its prompt
    const auto at_prompt = [&] {
        if (!m.tab().pty) return false;
        if (m.tab().term->alt_screen()) { toast("the shell is busy", "a full-screen program has the terminal", Toast::Kind::info); return false; }
        return true;
    };
    for (auto& a : std::exchange(m.pane.ui.out, {})) {
        switch (a.kind) {
            case A::Kind::list:
                m.pane.follow = a.path == m.pane.followed;
                cmds.push_back(pane_list(m, a.path));
                m.pane.ui.sel = 0;
                m.pane.ui.scroll.offset.y = 0.0f;
                break;
            case A::Kind::follow: {
                m.pane.follow = true;
                const Place pl = place_of(m);
                m.pane.followed = pl.dir;
                m.pane.ui.sel = 0;
                cmds.push_back(pane_list(m, pl.dir));
                break;
            }
            case A::Kind::cd:
                if (!at_prompt()) break;
                m.tab().input += cd_command(m, a.path) + "\r";
                m.pane.follow = true;   // the shell goes there; the pane goes with it
                m.pane.followed = a.path;
                if (m.pane.path != a.path) cmds.push_back(pane_list(m, a.path));
                m.pane.ui.focused = false;
                break;
            case A::Kind::paste:
                if (!at_prompt()) break;
                m.tab().input += shell_quote(m, a.path) + " ";
                m.pane.ui.focused = false;
                break;
            case A::Kind::copy_path:
                toast("copied", a.path, Toast::Kind::info);
                cmds.push_back(WriteClipboard{a.path});
                break;
            case A::Kind::reveal:
                if (m.pane.where.empty() && !m.cfg.demo) cmds.push_back(Cmd::run(fx::reveal, a.path));
                break;
            case A::Kind::open:
                if (m.pane.where.empty()) {
                    // a local file opens in a tab, in the editor; what isn't
                    // text (an image, a binary) goes on to the OS's app when
                    // the read finds it out (FileLoaded)
                    cmds.push_back(update(m, OpenFile{a.path}));
                    break;
                }
                // a remote one too: read and saved over the host's shared connection
                if (m.pane.host) { cmds.push_back(update(m, OpenFile{.path = a.path, .host = m.pane.host})); break; }
                [[fallthrough]];
            case A::Kind::download: {
                if (m.pane.where.empty() || !m.pane.host || m.cfg.demo) break;
                const std::string name = a.path.substr(a.path.find_last_of('/') + 1);
                m.pane.busy = "Downloading " + name + "\u2026";
                cmds.push_back(Cmd::task(fx::download, *m.pane.host, m.ssh.book, a.path, name));
                break;
            }
            case A::Kind::upload:
                if (m.pane.where.empty() || m.cfg.demo) break;
                cmds.push_back(Cmd::task(fx::pick_upload, m.pane.where, a.path));
                break;
            case A::Kind::close:
                m.pane.open = false;
                m.pane.ui.focused = false;
                fit(m);
                break;
        }
    }
    cmds.push_back(pane_preview(m));
    return Cmd::batch(std::move(cmds));
}

}  // namespace hand
