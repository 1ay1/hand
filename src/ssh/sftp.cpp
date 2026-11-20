// hand::sftp — see sftp.hpp.

#include <hand/ssh/sftp.hpp>
#include <hand/core/proc.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace hand::sftp {

namespace {

std::string home_dir() {
#if defined(_WIN32)
    if (const char* u = std::getenv("USERPROFILE"); u && *u) return u;
#endif
    const char* h = std::getenv("HOME");
    return h ? h : "";
}

/// Write `batch`, run sftp on it, clean up.
proc::Result run_batch(const Host& h, const HostBook& book, const std::string& batch, int timeout_ms) {
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
    const Host h = effective_host(host, book);
    std::vector<std::string> a{"sftp", "-q", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-b", batch_file};
    if (h.source == HostSource::config) {
        a.push_back(h.alias);   // ssh_config supplies the rest
        return a;
    }
    if (h.port != 22) a.insert(a.end(), {"-P", std::to_string(h.port)});
    if (!h.jump.empty()) a.insert(a.end(), {"-J", h.jump});
    if (!h.identity.empty()) {
        std::string id = h.identity;
        if (id.rfind("~/", 0) == 0) id = home_dir() + id.substr(1);
        a.insert(a.end(), {"-i", id});
    }
    a.push_back(h.user.empty() ? h.hostname : h.user + "@" + h.hostname);
    return a;
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
    const std::string e{err};
    const auto has = [&](const char* s) { return e.find(s) != std::string::npos; };
    if (has("Permission denied (publickey") || has("Permission denied (") || has("keyboard-interactive"))
        return "This host wants a password. Files need key login: add a key file to the host (or its folder), then ssh-copy-id it.";
    if (has("Host key verification failed")) return "The host key is not trusted yet. Connect once in a tab to accept it, then try again.";
    if (has("Could not resolve hostname")) return "The host name does not resolve. Check Host, or your VPN.";
    if (has("timed out") || has("Connection timed out")) return "The connection timed out. Is the host up, and reachable from here?";
    if (has("Connection refused")) return "Connection refused: nothing is listening on that port.";
    if (has("No such file")) return "No such file or directory.";
    if (has("subsystem request failed") || has("Connection closed")) return "The server does not offer SFTP.";
    if (has("Couldn't canonicalize") || has("not a directory")) return "Not a directory.";
    // otherwise the first meaningful line
    std::istringstream in{e};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line.rfind("sftp>", 0) != 0) return line;
    }
    return code == 127 || code < 0 ? std::string{"sftp is not installed or not on PATH."} : "sftp failed (" + std::to_string(code) + ").";
}

Listing list(const Host& h, const HostBook& book, const std::string& path) {
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
