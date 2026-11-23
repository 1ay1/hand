// hand::remote — see remote.hpp.

#include <hand/ssh/remote.hpp>
#include <hand/core/platform.hpp>
#include <hand/core/shellword.hpp>

#include <cstdlib>
#include <filesystem>
#include <sstream>

namespace hand::remote {

namespace fs = std::filesystem;

namespace {

std::string home_dir() { return plat::home_dir(); }   // the one definition of ~ (ssh's too)

/// Where to reach `h`: its alias when ~/.ssh/config defines it (config
/// supplies the rest), else user@host with hand's own -p / -J / -i.
void destination(const Host& host, const HostBook& book, std::vector<std::string>& a, bool sftp) {
    const Host h = effective_host(host, book);
    if (h.source == HostSource::config) { a.push_back(h.alias); return; }
    if (h.port != 22) a.insert(a.end(), {sftp ? "-P" : "-p", std::to_string(h.port)});
    if (!h.jump.empty()) a.insert(a.end(), {"-J", h.jump});
    if (!h.identity.empty()) {
        std::string id = h.identity;
        if (id.rfind("~/", 0) == 0) id = home_dir() + id.substr(1);
        a.insert(a.end(), {"-i", id});
    }
    a.push_back(h.user.empty() ? h.hostname : h.user + "@" + h.hostname);
}

}  // namespace

std::string control_dir() {
    if (!plat::ssh_can_share()) return {};   // no shared connections here: every action connects on its own
    const std::string d = home_dir() + "/.cache/hand/ssh";
    std::error_code ec;
    fs::create_directories(d, ec);
    fs::permissions(d, fs::perms::owner_all, fs::perm_options::replace, ec);
    return d;
}

std::vector<std::string> control_opts(const HostBook& book) {
    if (book.sets_control) return {};
    const std::string d = control_dir();
    if (d.empty()) return {};
    // %C: a hash of host, port, user and jump -- the same socket the terminal's
    // ssh uses (ssh_argv), so a tab's login also serves its files and vice versa
    // keepalive on the shared connection itself: after a sleep or a network
    // change a dead master is found in ~20 s and goes away, instead of every
    // tab and file action waiting on it
    std::vector<std::string> o{"-o", "ControlMaster=auto", "-o", "ControlPath=" + d + "/%C", "-o", "ControlPersist=10m"};
    if (!book.sets_alive) o.insert(o.end(), {"-o", "ServerAliveInterval=10", "-o", "ServerAliveCountMax=2"});
    return o;
}

std::vector<std::string> ssh_args(const Host& h, const HostBook& book) {
    std::vector<std::string> a{"ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"};
    const auto c = control_opts(book);
    a.insert(a.end(), c.begin(), c.end());
    destination(h, book, a, false);
    return a;
}

std::vector<std::string> sftp_args(const Host& h, const HostBook& book, const std::string& batch_file) {
    std::vector<std::string> a{"sftp", "-q", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10"};
    const auto c = control_opts(book);
    a.insert(a.end(), c.begin(), c.end());
    a.insert(a.end(), {"-b", batch_file});
    destination(h, book, a, true);
    return a;
}

std::string warm(const Host& h, const HostBook& book) {
    std::vector<std::string> a = ssh_args(h, book);
    // is it up? (a few ms over the socket)
    if (!control_opts(book).empty()) {
        std::vector<std::string> chk = a;
        chk.insert(chk.begin() + 1, {"-O", "check"});
        if (const auto r = proc::run(chk, {}, 3000); r.started && r.code == 0) return {};
    }
    // up a master in the background: -f returns once authenticated, -N runs nothing
    a.insert(a.begin() + 1, {"-f", "-N"});
    const auto r = proc::run(a, {}, 20000);
    if (!r.started) return "ssh is not installed or not on PATH.";
    return r.code == 0 ? std::string{} : explain(r.err, r.code);
}

void drop(const Host& h, const HostBook& book) {
    if (control_opts(book).empty()) return;
    std::vector<std::string> a = ssh_args(h, book);
    a.insert(a.begin() + 1, {"-O", "exit"});
    (void)proc::run(a, {}, 3000);
}

proc::Result exec(const Host& h, const HostBook& book, const std::string& command, std::string_view in, int timeout_ms) {
    std::vector<std::string> a = ssh_args(h, book);
    a.push_back(command);   // ssh hands it to the remote login shell as one line
    proc::Result r = proc::run(a, in, timeout_ms);
    // A stalled answer is usually a dead shared connection (the laptop slept,
    // the network changed): take it down so the next action connects afresh
    // rather than queueing behind it.
    if (r.timed_out) drop(h, book);
    return r;
}

std::string sh_quote(std::string_view s) { return quote_word(Dialect::posix, s); }   // a remote login shell is POSIX

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
    if (has("subsystem request failed")) return "The server does not offer SFTP.";
    if (has("Couldn't canonicalize") || has("not a directory")) return "Not a directory.";
    if (has("Is a directory")) return "That is a directory.";
    std::istringstream in{e};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line.rfind("sftp>", 0) != 0) return line;
    }
    if (code == 0) return {};
    return code == 127 || code < 0 ? std::string{"ssh is not installed or not on PATH."} : "the remote command failed (" + std::to_string(code) + ").";
}

}  // namespace hand::remote
