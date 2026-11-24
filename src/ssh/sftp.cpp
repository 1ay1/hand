// hand::sftp — see sftp.hpp.

#include <hand/ssh/sftp.hpp>
#include <hand/core/proc.hpp>
#include <hand/ssh/remote.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace hand::sftp {

namespace {

/// Write `batch`, run sftp on it, clean up.
proc::Result run_batch(const Host& h, const HostBook& book, const std::string& batch, int timeout_ms) {
    // sftp runs ssh with -oControlMaster=no before our options (first value
    // wins), so it can JOIN the host's shared connection but never start one:
    // bring it up first (a ~30 ms check when the terminal's login already did).
    (void)remote::warm(h, book);
    const std::string file = proc::temp_path("hand-sftp");
    {
        std::ofstream out{file, std::ios::binary};
        out << batch;
    }
    proc::Result r = proc::run(argv(h, book, file), {}, timeout_ms);
    std::error_code ec;
    std::filesystem::remove(file, ec);
    return r;
}

std::string slashes(std::string p) {
    for (char& ch : p) if (ch == '\\') ch = '/';
    return p;
}

}  // namespace

std::vector<std::string> argv(const Host& host, const HostBook& book, const std::string& batch_file) {
    return remote::sftp_args(host, book, batch_file);   // over the host's shared connection
}

std::string quote(std::string_view path) {
    // sftp's batch lexer: double quotes, backslash escapes inside them
    std::string q = "\"";
    for (char ch : path) {
        if (ch == '"' || ch == '\\') q += '\\';
        q += ch;
    }
    return q + "\"";
}

std::string join(std::string_view dir, std::string_view name) {
    if (dir.empty()) return std::string{name};
    std::string d{dir};
    if (d.back() != '/') d += '/';
    return d + std::string{name};
}

std::string parent(std::string_view path) {
    std::string p{path};
    while (p.size() > 1 && p.back() == '/') p.pop_back();
    const auto s = p.rfind('/');
    if (s == std::string::npos) return p;
    return s == 0 ? std::string{"/"} : p.substr(0, s);
}

Listing parse_listing(std::string_view out) {
    Listing l;
    std::istringstream in{std::string{out}};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line.rfind("sftp>", 0) == 0) continue;
        if (line.rfind("Remote working directory: ", 0) == 0) { l.path = line.substr(26); continue; }
        // ls -la longname: perms links owner group size month day time|year name
        if (line.size() < 11 || std::string_view{"-dlcbsp"}.find(line[0]) == std::string_view::npos) continue;
        std::size_t p = 0;
        std::vector<std::string> tok;
        while (tok.size() < 8 && p < line.size()) {
            while (p < line.size() && line[p] == ' ') ++p;
            const std::size_t s = p;
            while (p < line.size() && line[p] != ' ') ++p;
            tok.push_back(line.substr(s, p - s));
        }
        if (tok.size() < 8 || p >= line.size()) continue;
        std::string name = line.substr(p + 1);
        Entry e;
        e.perms = tok[0];
        e.dir = tok[0][0] == 'd';
        e.link = tok[0][0] == 'l';
        e.size = std::strtoull(tok[4].c_str(), nullptr, 10);
        e.when = tok[5] + " " + tok[6] + " " + tok[7];
        if (e.link) {
            if (const auto arrow = name.find(" -> "); arrow != std::string::npos) { e.target = name.substr(arrow + 4); name.resize(arrow); }
        }
        if (name == "." || name == "..") continue;
        e.name = std::move(name);
        l.entries.push_back(std::move(e));
    }
    std::stable_sort(l.entries.begin(), l.entries.end(), [](const Entry& a, const Entry& b) {
        if (a.dir != b.dir) return a.dir;
        const auto low = [](std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; };
        return low(a.name) < low(b.name);
    });
    return l;
}

std::string explain(std::string_view err, int code) {
    const std::string e = remote::explain(err, code);
    return code == 127 || code < 0 ? std::string{"sftp is not installed or not on PATH."} : e;
}

Listing list(const Host& h, const HostBook& book, const std::string& path) {
    // Over the host's kept-open channel first: `ls -la` prints the longname
    // form parse_listing reads (sftp's own ls copies it), in ~2 ms where a
    // check plus an sftp process was ~80 ms. LC_ALL=C: English month names and
    // byte sizes whatever the host's locale. sftp stays the fallback (a host
    // whose shell can't be used, Windows).
    {
        const std::string cmd = "cd " + remote::sh_quote(path.empty() ? std::string{"."} : path) +
                                " 2>/dev/null || { echo 'No such file or directory' >&2; exit 2; }; "
                                "printf 'Remote working directory: %s\\n' \"$(pwd)\"; LC_ALL=C exec ls -la";
        const proc::Result r = remote::exec(h, book, cmd, {}, 20000);
        if (r.started && !r.timed_out && r.code == 0) return parse_listing(r.out);
        if (r.started && r.code == 2) return Listing{path, {}, explain(r.err, 1)};
    }
    std::string batch;
    if (!path.empty()) batch += "cd " + quote(path) + "\n";
    batch += "pwd\nls -la\n";
    const proc::Result r = run_batch(h, book, batch, 20000);
    if (!r.started) return Listing{path, {}, "sftp is not installed or not on PATH."};
    if (r.timed_out) return Listing{path, {}, "The connection timed out. Is the host up, and reachable from here?"};
    Listing l = parse_listing(r.out);
    if (r.code != 0) {
        l.error = explain(r.err.empty() ? r.out : r.err, r.code);
        if (l.path.empty()) l.path = path;
    }
    return l;
}

std::string get(const Host& h, const HostBook& book, const std::vector<std::string>& remote, const std::string& local_dir) {
    std::error_code ec;
    std::filesystem::create_directories(local_dir, ec);
    std::string batch;
    for (const auto& r : remote) batch += "get -r " + quote(r) + " " + quote(slashes(local_dir) + "/") + "\n";
    const proc::Result r = run_batch(h, book, batch, 30 * 60 * 1000);
    if (!r.started) return "sftp is not installed or not on PATH.";
    return r.code == 0 ? std::string{} : explain(r.err.empty() ? r.out : r.err, r.code);
}

std::string put(const Host& h, const HostBook& book, const std::vector<std::string>& local, const std::string& remote_dir) {
    std::string batch = "cd " + quote(remote_dir) + "\n";
    for (const auto& l : local) batch += "put -r " + quote(slashes(l)) + "\n";
    const proc::Result r = run_batch(h, book, batch, 30 * 60 * 1000);
    if (!r.started) return "sftp is not installed or not on PATH.";
    return r.code == 0 ? std::string{} : explain(r.err.empty() ? r.out : r.err, r.code);
}

std::string mkdir(const Host& h, const HostBook& book, const std::string& remote) {
    const proc::Result r = run_batch(h, book, "mkdir " + quote(remote) + "\n", 20000);
    if (!r.started) return "sftp is not installed or not on PATH.";
    return r.code == 0 ? std::string{} : explain(r.err.empty() ? r.out : r.err, r.code);
}

}  // namespace hand::sftp
