// hand::App — the file pane beside the terminal (ctrl+shift+e).
//
// It shows the active tab's directory: the local shell's (OSC 7: hand's
// fish / zsh / bash / PowerShell integration reports every cd), or, for an
// ssh tab, the remote machine's over SFTP (the remote shell's OSC 7 if it
// sends one, else the login directory). It follows the shell until you
// browse elsewhere; "follow" brings it back. Listings run as tasks: a
// network share or a slow link never stalls a frame.

#include "detail.hpp"

#include <hand/core/proc.hpp>
#include <hand/core/pty.hpp>
#include <hand/ssh/sftp.hpp>

#include <mayag/widget/toast.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>

namespace hand {

using namespace mayag;
using namespace mayag::dsl;
using namespace hand::detail;

namespace {

namespace fs = std::filesystem;
using Sk = Sink<App::Msg>;

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

/// Is `host` (from OSC 7) this machine?
bool is_here(const std::string& host) {
    if (host.empty() || host == "localhost") return true;
    static const std::string me = lower(proc::host_name());
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
    Place p;
    const auto& r = remote_of(m);
    const std::string osc_host = m.term ? m.term->cwd_host() : std::string{};
    const std::string osc_dir = m.term ? m.term->cwd() : std::string{};
    if (r && !m.link.dropped) {
        p.where = r->alias;
        p.host = m.book.find(r->alias) ? *m.book.find(r->alias) : *r;
        // a remote shell's OSC 7 names its own host; a stale local one does not count
        if (!osc_dir.empty() && !is_here(osc_host)) p.dir = osc_dir;
        else p.note = "The remote shell does not report its directory: showing where you log in. Add OSC 7 to its prompt to follow cd.";
        return p;
    }
    if (!osc_dir.empty() && is_here(osc_host)) p.dir = osc_dir;
    else {
        std::error_code ec;
        p.dir = fs::current_path(ec).generic_string();
        p.note = "This shell does not report its directory (fish, zsh, bash and PowerShell do with shell integration): showing where it started.";
    }
    return p;
}

std::int64_t unix_of(fs::file_time_type t) {
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
    return static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(sys));
}

std::string when_of(std::int64_t unix) {
    const std::time_t tt = static_cast<std::time_t>(unix);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%d %H:%M", &tm);
    return b;
}

/// git's view of `dir`'s entries: a file's own status, a folder's the
/// loudest of what is inside it (conflict > modified > added > deleted >
/// renamed > untracked). Nothing when `dir` is not in a work tree or git
/// is not installed.
void git_marks(const std::string& dir, std::vector<FileEntry>& es) {
    const auto pre = proc::run({"git", "-C", dir, "rev-parse", "--show-prefix"}, {}, 1500);
    if (!pre.started || pre.code != 0) return;
    std::string prefix = pre.out;
    while (!prefix.empty() && (prefix.back() == '\n' || prefix.back() == '\r')) prefix.pop_back();
    const auto st = proc::run({"git", "-C", dir, "--no-optional-locks", "status", "--porcelain=v1", "-z", "--untracked-files=normal", "--", "."}, {}, 4000);
    if (!st.started || st.code != 0) return;
    const auto rank = [](char g) {
        switch (g) { case 'U': return 6; case 'M': return 5; case 'A': return 4; case 'D': return 3; case 'R': return 2; case '?': return 1; default: return 0; }
    };
    std::map<std::string, char> mark;
    std::size_t i = 0;
    const std::string& o = st.out;
    while (i < o.size()) {
        const auto end = o.find('\0', i);
        const std::string rec = o.substr(i, end == std::string::npos ? std::string::npos : end - i);
        i = end == std::string::npos ? o.size() : end + 1;
        if (rec.size() < 4) continue;
        const char x = rec[0], y = rec[1];
        if (x == 'R' || x == 'C') {   // the original name follows as its own record
            const auto e2 = o.find('\0', i);
            i = e2 == std::string::npos ? o.size() : e2 + 1;
        }
        char code;
        if (x == '?') code = '?';
        else if (x == 'U' || y == 'U' || (x == 'A' && y == 'A') || (x == 'D' && y == 'D')) code = 'U';
        else {
            code = y != ' ' ? y : x;
            if (code == 'T') code = 'M';
            if (code == 'C') code = 'A';
        }
        const std::string path = rec.substr(3);
        if (path.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string rel = path.substr(prefix.size());
        const std::string first = rel.substr(0, rel.find('/'));
        if (first.empty()) continue;
        char& m = mark[first];
        if (rank(code) > rank(m)) m = code;
    }
    for (auto& e : es)
        if (const auto it = mark.find(e.name); it != mark.end()) e.git = it->second;
}

App::PaneListed list_local(std::uint64_t token, const std::string& dir, bool git) {
    App::PaneListed r{token, dir, {}, {}};
    std::error_code ec;
    fs::directory_iterator it{fs::path{dir}, fs::directory_options::skip_permission_denied, ec};
    if (ec) { r.error = ec.message(); return r; }
    for (; it != fs::directory_iterator{}; it.increment(ec)) {
        if (ec) break;
        FileEntry e;
        e.name = it->path().filename().string();
        std::error_code e2;
        e.link = it->is_symlink(e2);
        e.dir = it->is_directory(e2);
        if (!e.dir) e.size = it->file_size(e2);
        if (const auto t = it->last_write_time(e2); !e2) {
            e.mtime = unix_of(t);
            e.when = when_of(e.mtime);
        }
        if (e.link) e.target = fs::read_symlink(it->path(), e2).generic_string();
        r.entries.push_back(std::move(e));
    }
    std::stable_sort(r.entries.begin(), r.entries.end(), [](const FileEntry& a, const FileEntry& b) {
        if (a.dir != b.dir) return a.dir;
        return lower(a.name) < lower(b.name);
    });
    if (git) git_marks(dir, r.entries);
    return r;
}

/// The head of a file (its first lines, made printable), or a folder's
/// first entries. Binary files say so instead.
std::optional<FilePreview> read_preview(const std::string& path, int max_lines) {
    namespace d = file_pane_detail;
    max_lines = std::clamp(max_lines, 1, 40);
    FilePreview pv;
    const fs::path fp{path};
    pv.name = fp.filename().string();
    std::error_code ec;
    if (fs::is_directory(fp, ec)) {
        std::vector<std::pair<bool, std::string>> kids;
        std::size_t n = 0;
        for (fs::directory_iterator it{fp, fs::directory_options::skip_permission_denied, ec}; !ec && it != fs::directory_iterator{}; it.increment(ec)) {
            ++n;
            std::error_code e2;
            if (kids.size() < 400) kids.push_back({it->is_directory(e2), it->path().filename().string()});
        }
        std::sort(kids.begin(), kids.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first;
            return lower(a.second) < lower(b.second);
        });
        for (const auto& [dir, name] : kids) {
            if (static_cast<int>(pv.lines.size()) >= max_lines) break;
            pv.lines.push_back(dir ? name + "/" : name);
        }
        pv.note = n == 1 ? "1 item" : std::to_string(n) + " items";
        return pv;
    }
    std::ifstream f(fp, std::ios::binary);
    if (!f) return std::nullopt;
    const auto size = fs::file_size(fp, ec);
    std::string buf(16384, '\0');
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    buf.resize(static_cast<std::size_t>(f.gcount()));
    if (buf.find('\0') != std::string::npos) {
        pv.note = "binary \u00B7 " + d::human(ec ? 0 : size);
        return pv;
    }
    pv.note = d::human(ec ? 0 : size);
    std::string cur;
    const auto push = [&] {
        if (cur.size() > 180) {   // cut on a character boundary
            std::size_t k = 180;
            while (k > 0 && (static_cast<unsigned char>(cur[k]) & 0xC0) == 0x80) --k;
            cur.resize(k);
        }
        pv.lines.push_back(std::move(cur));
        cur.clear();
    };
    for (std::size_t i = 0; i < buf.size() && static_cast<int>(pv.lines.size()) < max_lines; ++i) {
        const char ch = buf[i];
        if (ch == '\n') push();
        else if (ch == '\t') cur.append(4 - cur.size() % 4, ' ');
        else if (static_cast<unsigned char>(ch) >= 0x20 || ch < 0) cur.push_back(ch);
    }
    if (!cur.empty() && static_cast<int>(pv.lines.size()) < max_lines) push();
    return pv;
}

/// The path as a shell argument: PowerShell, cmd and POSIX quote differently.
std::string shell_quote(const Model& m, const std::string& path) {
    if (m.pane_where.empty()) {
#if defined(_WIN32)
        const std::string sh = lower(Pty::resolve_shell(m.cfg.shell));
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
    if (m.pane_where.empty() && lower(Pty::resolve_shell(m.cfg.shell)).find("cmd") != std::string::npos)
        return "cd /d " + shell_quote(m, path);
#endif
    return "cd " + shell_quote(m, path);
}

}  // namespace

namespace detail {

FilePaneProps pane_props(const Model& m) {
    FilePaneProps p;
    p.path = m.pane_path;
    p.where = m.pane_where;
    p.entries = m.pane_entries;
    for (auto& e : p.entries)   // the glow fades over ~2.5 s
        if (const auto it = m.pane_fresh_at.find(e.name); it != m.pane_fresh_at.end())
            e.fresh = std::clamp(1.0f - static_cast<float>((m.time - it->second) / 2.5), 0.0f, 1.0f);
    p.loading = m.pane_loading;
    p.following = m.pane_follow;
    p.error = m.pane_error;
    p.busy = m.pane_busy;
    p.note = m.pane_note;
    p.width = m.pane_w;
    p.left = m.cfg.files_left;
    p.now = unix_now();
    p.dirs_first = m.cfg.files_dirs_first;
    p.git = m.cfg.files_git;
    p.size_bars = m.cfg.files_size_bars;
    p.age = m.cfg.files_age;
    p.type_colors = m.cfg.files_type_colors;
    p.preview_lines = static_cast<int>(m.cfg.files_preview_lines);
    if (m.cfg.files_preview && m.pane_preview && !m.pane_preview_path.empty()) p.preview = m.pane_preview;
    std::string shown = m.pane_path;
    if (m.pane_where.empty()) {   // ~ for home, as the shell writes it
        std::string home;
#if defined(_WIN32)
        if (const char* u = std::getenv("USERPROFILE"); u && *u) home = u;
#else
        if (const char* u = std::getenv("HOME"); u && *u) home = u;
#endif
        for (char& ch : home) if (ch == '\\') ch = '/';
        while (home.size() > 1 && home.back() == '/') home.pop_back();
        p.home = home;
        if (!home.empty() && lower(shown).rfind(lower(home), 0) == 0 && (shown.size() == home.size() || shown[home.size()] == '/'))
            shown = "~" + shown.substr(home.size());
        p.title = shown;
    } else {
        p.title = m.pane_where + ":" + shown;
    }
    return p;
}

bool pane_glowing(const Model& m) {
    for (const auto& [name, at] : m.pane_fresh_at)
        if (m.time - at < 2.5) return true;
    return false;
}

}  // namespace detail

/// List `dir` where the pane looks (a task), and show it. `quiet`: a watch
/// re-read of the same folder (no spinner; what changed glows).
static App::Cmd pane_list(Model& m, std::string dir, bool quiet = false) {
    if (!quiet) {
        if (dir != m.pane_path) { m.pane_fresh_at.clear(); m.pane.filter.clear(); m.pane.filtering = false; }
        m.pane_path = dir;
        m.pane_loading = true;
        m.pane_error.clear();
    }
    const std::uint64_t token = ++m.pane_token;
    if (m.pane_where.empty())   // read-only, so even demo shots list for real
        return App::Cmd::task([](Sk out, std::uint64_t tk, std::string d, bool git, bool q) {
            auto r = list_local(tk, d, git);
            r.quiet = q;
            out.send(App::Msg{std::move(r)});
        }, token, std::move(dir), m.cfg.files_git, quiet);
    if (!m.pane_host || m.cfg.demo) return {};
    return App::Cmd::task([](Sk out, std::uint64_t tk, Host h, HostBook book, std::string d, bool q) {
        const auto l = sftp::list(h, book, d);
        App::PaneListed r{tk, l.path.empty() ? d : l.path, {}, l.error, q};
        for (const auto& e : l.entries) {
            FileEntry f;
            f.name = e.name; f.dir = e.dir; f.link = e.link; f.size = e.size; f.when = e.when; f.target = e.target;
            r.entries.push_back(std::move(f));
        }
        out.send(App::Msg{std::move(r)});
    }, token, *m.pane_host, m.book, std::move(dir), quiet);
}

/// The selection's preview: read it (a task) when the selection moved.
static App::Cmd pane_preview(Model& m) {
    if (!m.pane_open || !m.cfg.files_preview || !m.pane_where.empty()) {
        m.pane_preview.reset(); m.pane_preview_path.clear();
        return {};
    }
    const auto vis = file_pane_view(m.pane_entries, m.pane, m.cfg.files_dirs_first);
    if (vis.empty() || !m.pane.focused) { m.pane_preview.reset(); m.pane_preview_path.clear(); return {}; }
    const auto& e = m.pane_entries[vis[static_cast<std::size_t>(std::clamp(m.pane.sel, 0, static_cast<int>(vis.size()) - 1))].i];
    const std::string path = file_pane_detail::join(m.pane_path, e.name, '/');
    if (path == m.pane_preview_path) return {};
    m.pane_preview_path = path;
    return App::Cmd::task([](Sk out, std::string p, int n) { out.send(App::Msg{App::PanePreview{p, read_preview(p, n)}}); },
                          path, static_cast<int>(m.cfg.files_preview_lines));
}

App::Cmd App::update(Model& m, PanePreview p) {
    if (p.path != m.pane_preview_path) return {};
    m.pane_preview = std::move(p.preview);
    if (!m.pane_preview) m.pane_preview_path.clear();
    return {};
}

App::Cmd App::update(Model& m, PaneWatch) {
    if (!m.pane_open || m.pane_loading || m.pane_path.empty() || !m.cfg.files_watch) return {};
    return pane_list(m, m.pane_path, true);
}

App::Cmd App::pane_sync(Model& m) {
    if (!m.pane_open) return {};
    const Place p = place_of(m);
    const bool moved_machine = p.where != m.pane_where;
    if (moved_machine) {
        m.pane_where = p.where;
        m.pane_host = p.host;
        m.pane_follow = true;   // a new machine: start at its shell's directory
        m.pane_entries.clear();
        m.pane.sel = 0;
        m.pane.scroll.offset.y = 0.0f;
    }
    m.pane_note = p.note;
    if (!moved_machine && (!m.pane_follow || !m.cfg.files_follow || p.dir == m.pane_followed)) return {};
    m.pane_followed = p.dir;
    m.pane.sel = 0;
    m.pane.scroll.offset.y = 0.0f;
    return pane_list(m, p.dir);
}

App::Cmd App::update(Model& m, TogglePane) {
    if (m.pane_open && m.pane.focused) {   // open and in use: close it
        m.pane_open = false;
        m.pane.focused = false;
        fit(m);
        return {};
    }
    if (m.pane_open) { m.pane.focused = true; return pane_preview(m); }
    m.pane_open = true;
    m.pane.focused = true;
    m.pane_w = m.cfg.files_width;
    m.pane.show_hidden = m.cfg.files_hidden;
    m.pane.sort = static_cast<FileSort>(std::clamp(m.cfg.files_sort, 0, 3));
    m.pane_follow = m.cfg.files_follow || m.pane_path.empty();
    m.pane_where = "\x01";   // force a fresh look at where the shell is
    fit(m);
    return pane_sync(m);
}

App::Cmd App::update(Model& m, PaneResize r) {
    m.pane_w += r.dx;
    fit(m);
    m.cfg.files_width = m.pane_w;   // remembered (saved with the settings)
    return {};
}

App::Cmd App::update(Model& m, PaneListed r) {
    if (r.token != m.pane_token) return {};   // an older request
    if (r.quiet) {   // a watch re-read: same folder, glow what changed
        if (!r.error.empty() || r.path != m.pane_path) return {};
        std::map<std::string, const FileEntry*> before;
        for (const auto& e : m.pane_entries) before[e.name] = &e;
        bool changed = r.entries.size() != m.pane_entries.size();
        for (const auto& e : r.entries) {
            const auto it = before.find(e.name);
            const bool fresh = it == before.end() || it->second->size != e.size || it->second->mtime != e.mtime;
            if (fresh) m.pane_fresh_at[e.name] = m.time;
            changed = changed || fresh || it->second->git != e.git;
        }
        if (!changed) return {};
        // keep the selection on the same file as rows come and go
        const auto vis = file_pane_view(m.pane_entries, m.pane, m.cfg.files_dirs_first);
        std::string selected;
        if (!vis.empty()) selected = m.pane_entries[vis[static_cast<std::size_t>(std::clamp(m.pane.sel, 0, static_cast<int>(vis.size()) - 1))].i].name;
        m.pane_entries = std::move(r.entries);
        const auto now_vis = file_pane_view(m.pane_entries, m.pane, m.cfg.files_dirs_first);
        for (std::size_t q = 0; q < now_vis.size(); ++q)
            if (m.pane_entries[now_vis[q].i].name == selected) m.pane.sel = static_cast<int>(q);
        if (m.pane_fresh_at.count(selected)) m.pane_preview_path.clear();   // re-read its preview
        return pane_preview(m);
    }
    m.pane_loading = false;
    m.pane_error = r.error;
    m.pane_path = r.path;
    if (r.error.empty() || !r.entries.empty()) m.pane_entries = std::move(r.entries);
    if (m.pane_follow && m.pane_followed.empty()) m.pane_followed = r.path;   // the login dir, resolved
    m.pane.sel = std::clamp(m.pane.sel, 0, std::max(static_cast<int>(m.pane_entries.size()) - 1, 0));
    m.pane_preview_path.clear();
    return pane_preview(m);
}

App::Cmd App::update(Model& m, PaneOp p) {
    using A = FilePaneAction;
    const FileSort sort_before = m.pane.sort;
    const bool hidden_before = m.pane.show_hidden;
    p.op(m.pane);
    if (m.pane.sort != sort_before) m.cfg.files_sort = static_cast<int>(m.pane.sort);
    if (m.pane.show_hidden != hidden_before) m.cfg.files_hidden = m.pane.show_hidden;
    std::vector<Cmd> cmds;
    const auto toast = [&](std::string title, std::string body, Toast::Kind k) {
        m.toast = Toast{.title = std::move(title), .body = std::move(body), .kind = k};
        m.toast_at = m.time;
    };
    // typing into the shell is only safe at its prompt
    const auto at_prompt = [&] {
        if (!m.pty) return false;
        if (m.term->alt_screen()) { toast("the shell is busy", "a full-screen program has the terminal", Toast::Kind::info); return false; }
        return true;
    };
    for (auto& a : std::exchange(m.pane.out, {})) {
        switch (a.kind) {
            case A::Kind::list:
                m.pane_follow = a.path == m.pane_followed;
                cmds.push_back(pane_list(m, a.path));
                m.pane.sel = 0;
                m.pane.scroll.offset.y = 0.0f;
                break;
            case A::Kind::follow: {
                m.pane_follow = true;
                const Place pl = place_of(m);
                m.pane_followed = pl.dir;
                m.pane.sel = 0;
                cmds.push_back(pane_list(m, pl.dir));
                break;
            }
            case A::Kind::cd:
                if (!at_prompt()) break;
                m.pty->write(cd_command(m, a.path) + "\r");
                m.pane_follow = true;   // the shell goes there; the pane goes with it
                m.pane_followed = a.path;
                if (m.pane_path != a.path) cmds.push_back(pane_list(m, a.path));
                m.pane.focused = false;
                break;
            case A::Kind::paste:
                if (!at_prompt()) break;
                m.pty->write(shell_quote(m, a.path) + " ");
                m.pane.focused = false;
                break;
            case A::Kind::copy_path:
                toast("copied", a.path, Toast::Kind::info);
                cmds.push_back(WriteClipboard{a.path});
                break;
            case A::Kind::reveal:
                if (m.pane_where.empty() && !m.cfg.demo) proc::reveal(a.path);
                break;
            case A::Kind::open:
                if (m.pane_where.empty()) {
                    if (!m.cfg.demo) proc::open_path(a.path);
                    break;
                }
                [[fallthrough]];
            case A::Kind::download: {
                if (m.pane_where.empty() || !m.pane_host || m.cfg.demo) break;
                const std::string name = a.path.substr(a.path.find_last_of('/') + 1);
                m.pane_busy = "Downloading " + name + "\u2026";
                cmds.push_back(Cmd::task([](Sk out, Host h, HostBook book, std::string remote, std::string nm) {
                    const std::string dir = proc::downloads_dir();
                    const std::string err = sftp::get(h, book, {remote}, dir);
                    out.send(App::Msg{App::FilesDone{h.alias, "", "Downloaded " + nm, err, dir + "/" + nm}});
                }, *m.pane_host, m.book, a.path, name));
                break;
            }
            case A::Kind::upload:
                if (m.pane_where.empty() || m.cfg.demo) break;
                cmds.push_back(Cmd::task([](Sk out, std::string alias, std::string dir) {
                    out.send(App::Msg{App::UploadPicked{alias, dir, proc::pick_files("Upload to " + dir)}});
                }, m.pane_where, a.path));
                break;
            case A::Kind::close:
                m.pane_open = false;
                m.pane.focused = false;
                fit(m);
                break;
        }
    }
    cmds.push_back(pane_preview(m));
    return Cmd::batch(std::move(cmds));
}

}  // namespace hand
