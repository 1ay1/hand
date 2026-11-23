// hand::fx — the file pane's effects: list a folder, read a preview,
// ask git. Run as tasks (a slow share never stalls a frame); update only
// names them.

#include "fx.hpp"

#include <hand/core/proc.hpp>

#include <mayag/widget/file_pane.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>

namespace hand::fx {

using namespace mayag;
using namespace mayag::dsl;

namespace {

namespace fs = std::filesystem;

std::string lower(std::string s) {
    for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

std::int64_t unix_of(fs::file_time_type t) {
    // libc++ (macOS) has no clock_cast; libc++ and libstdc++ both have
    // file_clock::to_sys, MSVC's STL has only clock_cast.
#if defined(_LIBCPP_VERSION) || defined(__GLIBCXX__)
    const auto sys = fs::file_time_type::clock::to_sys(t);
#else
    const auto sys = std::chrono::clock_cast<std::chrono::system_clock>(t);
#endif
    return static_cast<std::int64_t>(std::chrono::system_clock::to_time_t(
        std::chrono::time_point_cast<std::chrono::system_clock::duration>(sys)));
}

/// A file's modification time in the filesystem's own ticks (nanoseconds on
/// APFS / ext4): two saves in the same second still differ. 0: no such file.
std::int64_t mtime_ticks(const std::string& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(path, ec);
    return ec ? 0 : static_cast<std::int64_t>(t.time_since_epoch().count());
}

std::string when_of(std::int64_t epoch_s) {   // not `unix`: a predefined macro in GNU mode
    const std::time_t tt = static_cast<std::time_t>(epoch_s);
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

App::PaneListed read_dir(std::uint64_t token, const std::string& dir, bool git) {
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

}  // namespace

void list_local(Sink out, std::uint64_t token, std::string dir, bool git, bool quiet) {
    auto r = read_dir(token, dir, git);
    r.notice = quiet ? App::Notice::quiet : App::Notice::loud;
    out.send(App::Msg{std::move(r)});
}

void preview_local(Sink out, std::string path, int lines) {
    auto pv = read_preview(path, lines);
    out.send(App::Msg{App::PanePreview{std::move(path), std::move(pv)}});
}

void read_file(Sink out, SessionId sid, std::string path) {
    App::FileLoaded r{.sid = sid, .path = path};
    std::error_code ec;
    r.mtime = mtime_ticks(path);
    const auto size = fs::file_size(path, ec);
    if (ec) { r.error = ec.message(); out.send(App::Msg{std::move(r)}); return; }
    if (size > 32u * 1024u * 1024u) {   // the editor holds the whole file: past this it is a log, not a source
        r.error = "too large to edit (" + file_pane_detail::human(size) + ")";
        out.send(App::Msg{std::move(r)});
        return;
    }
    std::ifstream f(path, std::ios::binary);
    if (!f) { r.error = "cannot read it"; out.send(App::Msg{std::move(r)}); return; }
    r.text.resize(static_cast<std::size_t>(size));
    f.read(r.text.data(), static_cast<std::streamsize>(size));
    r.text.resize(static_cast<std::size_t>(f.gcount()));
    // a NUL in the first 8 KB: a binary (git's test)
    if (r.text.find('\0') < 8192) { r.binary = true; r.text.clear(); }
    out.send(App::Msg{std::move(r)});
}

void write_file(Sink out, SessionId sid, std::string path, std::string text, std::uint64_t point) {
    // Write a sibling, then rename it over the file: a crash or a full disk
    // mid-write leaves the old file whole, never half of the new one.
    App::FileSaved r{.sid = sid, .point = point};
    const fs::path target{path};
    fs::path tmp = target;
    tmp += ".hand-save~";
    std::error_code ec;
    const auto perms = fs::status(target, ec).permissions();
    const bool existed = !ec;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { r.error = "cannot write in " + target.parent_path().string(); out.send(App::Msg{std::move(r)}); return; }
        f.write(text.data(), static_cast<std::streamsize>(text.size()));
        f.flush();
        if (!f) { r.error = "the write failed (disk full?)"; fs::remove(tmp, ec); out.send(App::Msg{std::move(r)}); return; }
    }
    if (existed) fs::permissions(tmp, perms, ec);   // an executable script stays executable
    fs::rename(tmp, target, ec);
    if (ec) { r.error = ec.message(); fs::remove(tmp, ec); }
    r.mtime = mtime_ticks(path);   // what "unchanged since we wrote it" looks like
    out.send(App::Msg{std::move(r)});
}

void reread_file(Sink out, SessionId sid, std::string path) {
    // the same read, marked as a reload
    std::error_code ec;
    App::FileLoaded r{.sid = sid, .path = path, .reload = true};
    r.mtime = mtime_ticks(path);
    const auto size = fs::file_size(path, ec);
    if (ec || size > 32u * 1024u * 1024u) { r.error = ec ? ec.message() : "too large"; out.send(App::Msg{std::move(r)}); return; }
    std::ifstream f(path, std::ios::binary);
    r.text.resize(static_cast<std::size_t>(size));
    f.read(r.text.data(), static_cast<std::streamsize>(size));
    r.text.resize(static_cast<std::size_t>(f.gcount()));
    out.send(App::Msg{std::move(r)});
}

void stat_file(Sink out, SessionId sid, std::string path) {
    out.send(App::Msg{App::FileStat{sid, mtime_ticks(path)}});
}

void list_project(Sink out, std::string root) {
    App::QuickListed r{.root = root};
    constexpr std::size_t kMax = 50000;
    // a git repo: list from its top (⌘P in src/app still finds README.md),
    // and git knows exactly which files are the project's
    const auto top = proc::run({"git", "-C", root, "rev-parse", "--show-toplevel"}, {}, 1500);
    if (top.started && top.code == 0) {
        std::string t = top.out;
        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
        if (!t.empty()) { root = t; r.root = t; }
    }
    const auto g = proc::run({"git", "-C", root, "ls-files", "-z", "--cached", "--others", "--exclude-standard"}, {}, 4000);
    if (g.started && g.code == 0) {
        std::size_t a = 0;
        for (std::size_t i = 0; i < g.out.size(); ++i)
            if (g.out[i] == '\0') { if (i > a) r.files.emplace_back(g.out.substr(a, i - a)); a = i + 1; if (r.files.size() >= kMax) break; }
    } else {   // not a repo: walk, skipping what is never source
        std::error_code ec;
        fs::recursive_directory_iterator it{root, fs::directory_options::skip_permission_denied, ec}, end;
        for (; !ec && it != end && r.files.size() < kMax; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            std::error_code e2;
            if (it->is_directory(e2)) {
                if (name.empty() || name[0] == '.' || name == "node_modules" || name == "build" || name == "target" ||
                    name == "dist" || name == "__pycache__" || name.rfind("build-", 0) == 0)
                    it.disable_recursion_pending();
                continue;
            }
            if (it->is_regular_file(e2)) r.files.push_back(fs::relative(it->path(), root, e2).generic_string());
        }
    }
    r.truncated = r.files.size() >= kMax;
    out.send(App::Msg{std::move(r)});
}

namespace {
/// The repo's top above `dir`, or `dir` when it is not in one.
std::string repo_top(const std::string& dir) {
    const auto top = proc::run({"git", "-C", dir, "rev-parse", "--show-toplevel"}, {}, 1500);
    if (!top.started || top.code != 0) return dir;
    std::string t = top.out;
    while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
    return t.empty() ? dir : t;
}
}  // namespace

void search_project(Sink out, std::uint64_t token, std::string root, std::string query) {
    App::SearchAllDone r{.token = token};
    root = repo_top(root);
    r.root = root;
    // smart case, like ripgrep: lowercase searches ignore case
    const bool fold = std::none_of(query.begin(), query.end(), [](char c) { return c >= 'A' && c <= 'Z'; });
    std::vector<std::string> argv = {"git", "-C", root, "grep", "-n", "-I", "--no-color", "-F", "--untracked", "--exclude-standard"};
    if (fold) argv.push_back("-i");
    argv.insert(argv.end(), {"-e", query, "--"});
    auto g = proc::run(argv, {}, 4000);
    if (!g.started || (g.code != 0 && g.code != 1)) {   // not a repo: grep, skipping what is never source
        std::vector<std::string> a = {"grep", "-rnIF", "--exclude-dir=.git", "--exclude-dir=node_modules", "--exclude-dir=build",
                                      "--exclude-dir=target", "--exclude-dir=dist"};
        if (fold) a.push_back("-i");
        a.insert(a.end(), {"-e", query, "--", root});
        g = proc::run(a, {}, 4000);
    }
    if (g.timed_out) r.error = "the search took too long; showing what was found";
    const auto lower = [](std::string s) { for (char& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch))); return s; };
    const std::string needle = fold ? lower(query) : query;
    constexpr std::size_t kMax = 2000;
    std::size_t a = 0;
    for (std::size_t i = 0; i <= g.out.size(); ++i) {
        if (i < g.out.size() && g.out[i] != '\n') continue;
        std::string_view l{g.out.data() + a, i - a};
        a = i + 1;
        // path:line:text (the path may hold ':' only in pathological cases)
        const auto c1 = l.find(':');
        if (c1 == std::string_view::npos) continue;
        const auto c2 = l.find(':', c1 + 1);
        if (c2 == std::string_view::npos) continue;
        SearchHit h;
        h.file = std::string{l.substr(0, c1)};
        if (h.file.rfind(root + "/", 0) == 0) h.file = h.file.substr(root.size() + 1);   // grep's absolute paths
        h.line = std::atoi(std::string{l.substr(c1 + 1, c2 - c1 - 1)}.c_str()) - 1;
        std::string text{l.substr(c2 + 1)};
        if (!text.empty() && text.back() == '\r') text.pop_back();
        const auto at = (fold ? lower(text) : text).find(needle);
        h.col = at == std::string::npos ? 0 : static_cast<int>(at);
        h.len = at == std::string::npos ? 0 : static_cast<int>(query.size());
        h.text = std::move(text);
        if (h.line >= 0) r.hits.push_back(std::move(h));
        if (r.hits.size() >= kMax) { r.truncated = true; break; }
    }
    out.send(App::Msg{std::move(r)});
}

}  // namespace hand::fx
