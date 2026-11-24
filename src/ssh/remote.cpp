// hand::remote — see remote.hpp.

#include <hand/ssh/remote.hpp>
#include <hand/core/platform.hpp>
#include <hand/core/shellword.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
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

namespace {
std::string channel_key(const Host& h, const HostBook& book);
}  // namespace

void drop(const Host& h, const HostBook& book) {
    proc::channels().drop(channel_key(h, book));
    if (control_opts(book).empty()) return;
    std::vector<std::string> a = ssh_args(h, book);
    a.insert(a.begin() + 1, {"-O", "exit"});
    (void)proc::run(a, {}, 3000);
}

// ── the channel: one remote `sh -s`, kept open ────────────────────────────────────
//
// Spawning ssh and starting the user's login shell for every action was most
// of its cost: 40-65 ms on localhost (fish alone ~30 ms), against 3 ms for
// the same stat sent down a shell that is already running. So exec() keeps
// `ssh host 'exec sh -s'` processes in a pool (proc::channels) and writes
// each command to one as a framed request:
//
//   ( CMD ) <in 2>"$T/e"; then a fresh random marker, the stderr, a second
//   marker and the exit code (see request())
//
// stdout streams straight back; nothing a command prints can be mistaken for
// a marker it has never seen. Input (a save) travels in the request as a
// base64 heredoc piped into the command, decoded by the host's base64 (no
// base64 there, or a very large save: the one-off exec below).

namespace {

constexpr std::size_t kChannelInputMax = 8u << 20;   // bigger saves take their own exec

std::string channel_key(const Host& h, const HostBook& book) {
    std::string k = "ssh";
    for (const auto& w : ssh_args(h, book)) k += '\x1f' + w;
    return k;
}

std::string b64(std::string_view in) {
    static constexpr char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4 + in.size() / 57 + 2);
    std::size_t col = 0;
    for (std::size_t i = 0; i < in.size(); i += 3) {
        const auto b = [&](std::size_t k) { return i + k < in.size() ? static_cast<unsigned char>(in[i + k]) : 0u; };
        const unsigned v = b(0) << 16 | b(1) << 8 | b(2);
        out += tbl[v >> 18 & 63];
        out += tbl[v >> 12 & 63];
        out += i + 1 < in.size() ? tbl[v >> 6 & 63] : '=';
        out += i + 2 < in.size() ? tbl[v & 63] : '=';
        if ((col += 4) >= 76) { out += '\n'; col = 0; }
    }
    if (col != 0 || out.empty()) out += '\n';
    return out;
}

/// A fresh channel to `h`, its shell set up (a temp dir, the base64 it
/// has), or null with `why` filled.
std::unique_ptr<proc::Channel> open_channel(const Host& h, const HostBook& book, proc::Deadline by, std::string& why) {
    std::vector<std::string> a = ssh_args(h, book);
    a.insert(a.begin() + 1, "-T");   // no tty: a clean byte stream
    a.push_back("exec sh -s");       // the login shell runs once, then gets out of the way
    auto ch = proc::Channel::open(a);
    if (!ch) return nullptr;
    const std::string init =
        "T=$(mktemp -d 2>/dev/null || mktemp -d -t hand 2>/dev/null) || exit 1; trap 'rm -rf \"$T\"' EXIT; "
        "if printf aGk= | base64 -d >/dev/null 2>&1; then D='base64 -d'; "
        "elif printf aGk= | base64 -D >/dev/null 2>&1; then D='base64 -D'; else D=; fi; "
        // the host's stat, found once: scripts write `$S "$f"` for "mtime:size"
        // instead of trying GNU then BSD per call (a failed fork each time, ~2 ms)
        "if stat -c %Y:%s / >/dev/null 2>&1; then S='stat -c %Y:%s'; else S='stat -f %m:%z'; fi; "
        "export S; "
        "echo \"hand-ready ${D:+b64}\"\n";
    std::string line;
    if (!ch->write(init, by) || !ch->read_line(line, by) || line.rfind("hand-ready", 0) != 0) {
        why = ch->err();
        return nullptr;
    }
    ch->b64 = line.find("b64") != std::string::npos;
    return ch;
}

enum class Sent : std::uint8_t { answered, lost_before_answer, failed };

/// One request on `ch`. `lost_before_answer`: the channel died before any
/// answer came (a stale pooled one): safe to try once more on a fresh one.
///
/// The answer is framed by a fresh 128-bit marker, not by byte counts:
///
///   ( CMD ) <in 2>"$T/e"; c=$?; printf '\nM1\n'; [ -s "$T/e" ] && cat "$T/e"; printf '\nM2 %d\n' "$c"
///
/// stdout streams straight back; stderr (usually empty) waits in a file.
/// Counting bytes with `wc` and `cat`-ing stdout back cost five more forks,
/// ~1 ms each: 7.3 ms a request, against ~1.5 ms like this. A command can't
/// print a marker it has never seen.
Sent request(proc::Channel& ch, const std::string& command, std::string_view in, proc::Deadline by, proc::Result& r) {
    std::random_device rd;
    char nonce[40];
    std::snprintf(nonce, sizeof nonce, "%08x%08x%08x%08x", rd(), rd(), rd(), rd());
    const std::string m1 = std::string{"\nhand-err-"} + nonce + "\n";
    const std::string m2 = std::string{"\nhand-end-"} + nonce + " ";
    std::string req = "( " + command + " ) ";
    if (in.empty()) req += "</dev/null ";
    else req = "$D <<'HAND_IN_EOF' | " + req;
    req += "2>\"$T/e\"; c=$?; printf '\\nhand-err-%s\\n' ";
    req += nonce;
    req += "; [ -s \"$T/e\" ] && cat \"$T/e\"; printf '\\nhand-end-%s %d\\n' ";
    req += nonce;
    req += " \"$c\"\n";
    if (!in.empty()) { req += b64(in); req += "HAND_IN_EOF\n"; }
    r.started = true;
    if (!ch.write(req, by)) return Sent::lost_before_answer;
    const auto lost = [&] {
        r.timed_out = proc::Clock::now() >= by;
        return r.timed_out ? Sent::failed : Sent::lost_before_answer;
    };
    if (!ch.read_until(m1, r.out, by)) return r.out.empty() ? lost() : Sent::failed;
    std::string code;
    if (!ch.read_until(m2, r.err, by) || !ch.read_line(code, by)) return Sent::failed;
    r.code = std::atoi(code.c_str());
    return Sent::answered;
}

/// The request on a pooled (or fresh) channel; nullopt: no channel could
/// carry it (none on this platform, it would not come up, input too big):
/// the caller runs the one-off way.
std::optional<proc::Result> on_channel(const Host& h, const HostBook& book, const std::string& command, std::string_view in,
                                       proc::Deadline by) {
    // HAND_SSH_CHANNEL=0: every action its own ssh, as before (A/B, and a test
    // of the fallback the platforms without channels take)
    if (const char* e = std::getenv("HAND_SSH_CHANNEL"); e != nullptr && e[0] == '0') return std::nullopt;
    const std::string key = channel_key(h, book);
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto ch = proc::channels().take(key);
        const bool pooled = ch != nullptr;
        std::string why;
        if (!ch) ch = open_channel(h, book, by, why);
        if (!ch) return std::nullopt;
        if (!in.empty() && (!ch->b64 || in.size() > kChannelInputMax)) { proc::channels().give(key, std::move(ch)); return std::nullopt; }
        proc::Result r;
        const Sent s = request(*ch, command, in, by, r);
        if (s == Sent::answered) { proc::channels().give(key, std::move(ch)); return r; }
        if (s == Sent::lost_before_answer && pooled) continue;   // a stale idle channel (the link blinked): once more, fresh
        if (r.timed_out) drop(h, book);   // a stalled answer: a dead shared connection, take it down
        if (r.err.empty()) r.err = ch->err();
        return r;
    }
    return std::nullopt;
}

proc::Result one_off(const Host& h, const HostBook& book, const std::string& command, std::string_view in, int timeout_ms) {
    std::vector<std::string> a = ssh_args(h, book);
    a.push_back(command);   // ssh hands it to the remote login shell as one line
    proc::Result r = proc::run(a, in, timeout_ms);
    // A stalled answer is usually a dead shared connection (the laptop slept,
    // the network changed): take it down so the next action connects afresh
    // rather than queueing behind it.
    if (r.timed_out) drop(h, book);
    return r;
}

}  // namespace

std::string warm_channel(const Host& h, const HostBook& book) {
    const std::string key = channel_key(h, book);
    if (proc::channels().idle(key) > 0) return {};
    std::string why;
    auto ch = open_channel(h, book, proc::Clock::now() + std::chrono::seconds(20), why);
    if (!ch) return explain(why, 255);
    proc::channels().give(key, std::move(ch));
    return {};
}

proc::Result exec(const Host& h, const HostBook& book, const std::string& command, std::string_view in, int timeout_ms) {
    if (auto r = on_channel(h, book, command, in, proc::Clock::now() + std::chrono::milliseconds(timeout_ms))) return std::move(*r);
    return one_off(h, book, command, in, timeout_ms);
}

proc::Result exec_sh(const Host& h, const HostBook& book, const std::string& script, const std::vector<std::string>& args,
                     std::string_view in, int timeout_ms) {
    std::string words;
    for (const auto& a : args) words += " " + sh_quote(a);
    // the channel's shell is already POSIX sh: the script runs as is
    if (auto r = on_channel(h, book, "set --" + words + "; " + script, in,
                            proc::Clock::now() + std::chrono::milliseconds(timeout_ms)))
        return std::move(*r);
    return one_off(h, book, "sh -c " + sh_quote(script) + " sh" + words, in, timeout_ms);
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
