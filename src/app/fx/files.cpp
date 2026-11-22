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

}  // namespace hand::fx
