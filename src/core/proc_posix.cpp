// hand::proc — POSIX (Linux, macOS).

#include <mayag/core/target.hpp>
#include <hand/core/proc.hpp>
#include <hand/core/pty.hpp>

#if !MAYAG_OS_WINDOWS

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#if MAYAG_OS_APPLE
#  include <arpa/inet.h>   // ntohs
#  include <libproc.h>     // proc_listpids, proc_pidinfo, proc_pidfdinfo, proc_name
#  include <sys/proc_info.h>
#endif

namespace hand::proc {

Result run(const std::vector<std::string>& argv, std::string_view in, int timeout_ms) {
    Result r;
    if (argv.empty()) return r;
    int pin[2], pout[2], perr[2];
    if (::pipe(pin) || ::pipe(pout) || ::pipe(perr)) return r;
    const pid_t pid = ::fork();
    if (pid < 0) return r;
    if (pid == 0) {
        reset_signals();
        ::dup2(pin[0], 0); ::dup2(pout[1], 1); ::dup2(perr[1], 2);
        for (int fd : {pin[0], pin[1], pout[0], pout[1], perr[0], perr[1]}) ::close(fd);
        std::vector<char*> a;
        for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        ::execvp(a[0], a.data());
        ::_exit(127);
    }
    ::close(pin[0]); ::close(pout[1]); ::close(perr[1]);
    r.started = true;
    std::size_t put = 0;
    if (in.empty()) ::close(pin[1]);
    else ::fcntl(pin[1], F_SETFL, O_NONBLOCK);
    bool in_open = !in.empty();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms < 0 ? 1 << 30 : timeout_ms);
    bool o = true, e = true;
    char buf[8192];
    while (o || e) {
        const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) { r.timed_out = true; ::kill(pid, SIGKILL); break; }
        pollfd p[3] = {{pout[0], POLLIN, 0}, {perr[0], POLLIN, 0}, {in_open ? pin[1] : -1, POLLOUT, 0}};
        if (!o) p[0].fd = -1;
        if (!e) p[1].fd = -1;
        if (::poll(p, 3, left) < 0 && errno != EINTR) break;
        if (in_open && (p[2].revents & (POLLOUT | POLLERR | POLLHUP))) {
            const ssize_t w = ::write(pin[1], in.data() + put, in.size() - put);
            if (w > 0) put += static_cast<std::size_t>(w);
            if (w < 0 || put == in.size()) { ::close(pin[1]); in_open = false; }
        }
        for (int k = 0; k < 2; ++k) {
            if (!(p[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t n = ::read(p[k].fd, buf, sizeof buf);
            if (n > 0) (k == 0 ? r.out : r.err).append(buf, static_cast<std::size_t>(n));
            else (k == 0 ? o : e) = false;
        }
    }
    if (in_open) ::close(pin[1]);
    ::close(pout[0]); ::close(perr[0]);
    int st = 0;
    ::waitpid(pid, &st, 0);
    r.code = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    if (r.code == 127 && r.out.empty()) r.started = false;
    return r;
}

std::vector<Listener> tcp_listeners(bool* ok) {
    std::vector<Listener> out;
    const auto add = [&](int port, int pid, std::string name) {
        for (const auto& l : out) if (l.port == port) return;
        out.push_back({port, pid, std::move(name)});
    };
#if MAYAG_OS_LINUX
    if (ok) *ok = true;
    for (const char* f : {"/proc/net/tcp", "/proc/net/tcp6"}) {
        std::ifstream in{f};
        std::string line;
        std::getline(in, line);   // header
        while (std::getline(in, line)) {
            std::istringstream ss{line};
            std::string sl, local, remote, st;
            ss >> sl >> local >> remote >> st;
            if (st != "0A") continue;   // LISTEN
            const auto c = local.rfind(':');
            if (c == std::string::npos) continue;
            add(static_cast<int>(std::strtol(local.c_str() + c + 1, nullptr, 16)), 0, "");
        }
    }
#elif MAYAG_OS_APPLE
    // No /proc: walk every process's sockets with libproc (what lsof does,
    // in-process). Only processes of this user answer, which is exactly the
    // set that matters -- the tunnels are this user's ssh.
    if (ok) *ok = true;
    std::vector<pid_t> pids(4096);
    int n = ::proc_listpids(PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (n <= 0) { if (ok) *ok = false; return out; }
    pids.resize(static_cast<std::size_t>(n) / sizeof(pid_t));
    std::vector<proc_fdinfo> fds;
    for (const pid_t pid : pids) {
        if (pid <= 0) continue;
        const int bytes = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (bytes <= 0) continue;
        fds.resize(static_cast<std::size_t>(bytes) / sizeof(proc_fdinfo) + 16);
        const int got = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.data(), static_cast<int>(fds.size() * sizeof(proc_fdinfo)));
        if (got <= 0) continue;
        std::string name;
        for (std::size_t i = 0; i < static_cast<std::size_t>(got) / sizeof(proc_fdinfo); ++i) {
            if (fds[i].proc_fdtype != PROX_FDTYPE_SOCKET) continue;
            socket_fdinfo si{};
            if (::proc_pidfdinfo(pid, fds[i].proc_fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) != sizeof si) continue;
            if (si.psi.soi_kind != SOCKINFO_TCP || si.psi.soi_proto.pri_tcp.tcpsi_state != TSI_S_LISTEN) continue;
            if (name.empty()) {
                char b[2 * MAXCOMLEN + 1] = {};
                if (::proc_name(pid, b, sizeof b) > 0) name = b;
            }
            add(static_cast<int>(ntohs(static_cast<std::uint16_t>(si.psi.soi_proto.pri_tcp.tcpsi_ini.insi_lport))),
                static_cast<int>(pid), name);
        }
    }
#else
    if (ok) *ok = false;
#endif
    return out;
}

std::vector<RawSession> putty_sessions(const std::string& root) {
    std::vector<RawSession> out;
    std::string dir = root;
    if (dir.empty()) {
        const char* h = std::getenv("HOME");
        if (!h) return out;
        dir = std::string{h} + "/.putty/sessions";
    }
    std::error_code ec;
    for (const auto& f : std::filesystem::directory_iterator{dir, ec}) {
        RawSession rs;
        const std::string enc = f.path().filename().string();
        for (std::size_t k = 0; k < enc.size(); ++k) {
            if (enc[k] == '%' && k + 2 < enc.size()) { rs.name += static_cast<char>(std::strtol(enc.substr(k + 1, 2).c_str(), nullptr, 16)); k += 2; }
            else rs.name += enc[k];
        }
        std::ifstream in{f.path()};
        std::string line;
        while (std::getline(in, line)) {
            const auto eq = line.find('=');
            if (eq != std::string::npos) rs.values[line.substr(0, eq)] = line.substr(eq + 1);
        }
        out.push_back(std::move(rs));
    }
    return out;
}

std::vector<std::string> pick_files(const std::string& title) {
    // The desktop's own picker: AppKit's open panel through osascript on
    // macOS, zenity / kdialog elsewhere. One path per line; cancel is none.
#if MAYAG_OS_APPLE
    std::string quoted;   // an AppleScript string literal
    for (char ch : title) { if (ch == '"' || ch == '\\') quoted += '\\'; quoted += ch; }
    const std::vector<std::vector<std::string>> pickers{{
        "osascript",
        "-e", "set fs to choose file with prompt \"" + quoted + "\" with multiple selections allowed",
        "-e", "set out to \"\"",
        "-e", "repeat with f in fs",
        "-e", "set out to out & POSIX path of f & linefeed",
        "-e", "end repeat",
        "-e", "return out"}};
#else
    const std::vector<std::vector<std::string>> pickers{
        {"zenity", "--file-selection", "--multiple", "--separator=\n", "--title=" + title},
        {"kdialog", "--getopenfilename", ".", "--multiple", "--separate-output"}};
#endif
    for (const auto& argv : pickers) {
        const Result r = run(argv, {}, -1);
        if (!r.started) continue;
        std::vector<std::string> out;
        std::istringstream ss{r.out};
        std::string l;
        while (std::getline(ss, l)) if (!l.empty()) out.push_back(l);
        return out;
    }
    return {};
}

std::string downloads_dir() {
    if (const char* h = std::getenv("HOME")) return std::string{h} + "/Downloads";
    return "/tmp";
}

void reveal(const std::string& path) {
#if MAYAG_OS_APPLE
    (void)run({"open", "-R", path}, {}, 5000);
#else
    (void)run({"xdg-open", std::filesystem::path{path}.parent_path().string()}, {}, 5000);
#endif
}

void open_path(const std::string& path) {
#if MAYAG_OS_APPLE
    (void)run({"open", path}, {}, 5000);
#else
    (void)run({"xdg-open", path}, {}, 5000);
#endif
}

void open_url(const std::string& url) {
#if MAYAG_OS_APPLE
    spawn_detached({"open", url});
#else
    spawn_detached({"xdg-open", url});
#endif
}

std::string host_name() {
    char b[256] = {};
    if (::gethostname(b, sizeof b - 1) != 0) return {};
    return b;
}

// ── Channel ──────────────────────────────────────────────────────────────

std::unique_ptr<Channel> Channel::open(const std::vector<std::string>& argv) {
    if (argv.empty()) return nullptr;
    // stdin is a socketpair, so a write after the child died fails with EPIPE
    // instead of raising SIGPIPE (MSG_NOSIGNAL / SO_NOSIGPIPE): a dropped
    // connection must never take hand down with it.
    int sin[2], pout[2], perr[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, sin) != 0) return nullptr;
    if (::pipe(pout) != 0) { ::close(sin[0]); ::close(sin[1]); return nullptr; }
    if (::pipe(perr) != 0) { ::close(sin[0]); ::close(sin[1]); ::close(pout[0]); ::close(pout[1]); return nullptr; }
#ifdef SO_NOSIGPIPE
    const int one = 1;
    (void)::setsockopt(sin[0], SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    for (int fd : {sin[0], pout[0], perr[0]}) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    const pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {sin[0], sin[1], pout[0], pout[1], perr[0], perr[1]}) ::close(fd);
        return nullptr;
    }
    if (pid == 0) {
        reset_signals();
        ::dup2(sin[1], 0); ::dup2(pout[1], 1); ::dup2(perr[1], 2);
        for (int fd : {sin[0], sin[1], pout[0], pout[1], perr[0], perr[1]}) ::close(fd);
        std::vector<char*> a;
        for (const auto& s : argv) a.push_back(const_cast<char*>(s.c_str()));
        a.push_back(nullptr);
        ::execvp(a[0], a.data());
        ::_exit(127);
    }
    ::close(sin[1]); ::close(pout[1]); ::close(perr[1]);
    for (int fd : {sin[0], pout[0], perr[0]}) ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    std::unique_ptr<Channel> c{new Channel};
    c->in_ = sin[0]; c->out_ = pout[0]; c->errfd_ = perr[0]; c->pid_ = pid;
    c->used = std::chrono::steady_clock::now();
    return c;
}

Channel::~Channel() {
    for (int fd : {in_, out_, errfd_}) if (fd >= 0) ::close(fd);
    if (pid_ > 0) {
        // stdin closed: a remote `sh -s` exits and ssh with it. Give it a
        // moment (a clean exit leaves the shared master tidy), then insist.
        int st = 0;
        for (int i = 0; i < 20; ++i) {
            if (::waitpid(pid_, &st, WNOHANG) == pid_) return;
            ::usleep(1000);
        }
        ::kill(pid_, SIGKILL);
        while (::waitpid(pid_, &st, 0) < 0 && errno == EINTR) {}
    }
}

namespace {
int ms_until(Deadline by) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(by - std::chrono::steady_clock::now()).count();
    return ms <= 0 ? 0 : static_cast<int>(std::min<long long>(ms, 1 << 30));
}
}  // namespace

bool Channel::write(std::string_view s, Deadline by) {
    if (broken_) return false;
    std::size_t put = 0;
    while (put < s.size()) {
#ifdef MSG_NOSIGNAL
        const ssize_t n = ::send(in_, s.data() + put, s.size() - put, MSG_NOSIGNAL);
#else
        const ssize_t n = ::send(in_, s.data() + put, s.size() - put, 0);
#endif
        if (n > 0) { put += static_cast<std::size_t>(n); continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // the child is slow to read (a big save): wait for room, and
            // keep stderr drained so it can't block on a full pipe meanwhile
            pollfd p[2] = {{in_, POLLOUT, 0}, {errfd_, POLLIN, 0}};
            const int left = ms_until(by);
            if (left == 0 || ::poll(p, 2, left) <= 0) { broken_ = true; return false; }
            if (p[1].revents & (POLLIN | POLLHUP)) {
                char b[4096];
                const ssize_t e = ::read(errfd_, b, sizeof b);
                if (e > 0 && err_.size() < 65536) err_.append(b, static_cast<std::size_t>(e));
            }
            continue;
        }
        broken_ = true;   // EPIPE: the child is gone
        return false;
    }
    return true;
}

bool Channel::fill(Deadline by) {
    for (;;) {
        pollfd p[2] = {{out_, POLLIN, 0}, {errfd_, POLLIN, 0}};
        const int left = ms_until(by);
        const int r = ::poll(p, errfd_ >= 0 ? 2 : 1, left);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { broken_ = true; return false; }   // timed out: out of step from here on
        char b[65536];
        if (errfd_ >= 0 && (p[1].revents & (POLLIN | POLLHUP))) {
            const ssize_t e = ::read(errfd_, b, sizeof b);
            if (e > 0) { if (err_.size() < 65536) err_.append(b, static_cast<std::size_t>(e)); }
            else if (e == 0) { ::close(errfd_); errfd_ = -1; }
        }
        if (p[0].revents & (POLLIN | POLLHUP | POLLERR)) {
            const ssize_t n = ::read(out_, b, sizeof b);
            if (n > 0) { buf_.append(b, static_cast<std::size_t>(n)); return true; }
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            broken_ = true;   // EOF: the child exited
            return false;
        }
    }
}

bool Channel::read_line(std::string& line, Deadline by) {
    if (broken_) return false;
    for (std::size_t from = 0;;) {
        if (const auto nl = buf_.find('\n', from); nl != std::string::npos) {
            line.assign(buf_, 0, nl);
            buf_.erase(0, nl + 1);
            return true;
        }
        from = buf_.size();
        if (!fill(by)) return false;
    }
}

bool Channel::read_exact(std::size_t n, std::string& out, Deadline by) {
    if (broken_) return false;
    while (buf_.size() < n)
        if (!fill(by)) return false;
    out.append(buf_, 0, n);
    buf_.erase(0, n);
    return true;
}

bool Channel::read_until(std::string_view marker, std::string& out, Deadline by) {
    if (broken_ || marker.empty()) return false;
    for (std::size_t from = 0;;) {
        if (const auto at = buf_.find(marker, from); at != std::string::npos) {
            out.append(buf_, 0, at);
            buf_.erase(0, at + marker.size());
            return true;
        }
        // only the tail that could still start the marker is searched again
        from = buf_.size() >= marker.size() ? buf_.size() - marker.size() + 1 : 0;
        if (!fill(by)) return false;
    }
}

}  // namespace hand::proc

#endif  // !_WIN32
