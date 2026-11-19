// hand::Pty — forkpty + nonblocking I/O.

#include <hand/core/pty.hpp>

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
#include <util.h>
#else
#include <pty.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/sysctl.h>
#endif

#include <fstream>
#include <vector>

namespace hand {

Pty::~Pty() {
    if (fd_ >= 0) ::close(fd_);
    if (pid_ > 0) {
        ::kill(pid_, SIGHUP);
        (void)::waitpid(pid_, nullptr, WNOHANG);
    }
}

std::string Pty::resolve_shell(std::string shell) {
    if (shell.empty()) {
        const char* s = std::getenv("SHELL");
        shell = (s != nullptr && *s != '\0') ? s : "/bin/sh";
    }
    return shell;
}

bool Pty::spawn(int cols, int rows, std::string shell, const std::vector<std::pair<std::string, std::string>>& env,
                const std::vector<std::string>& args, const std::string& cwd) {
    shell = resolve_shell(std::move(shell));
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    int master = -1;
    const pid_t pid = ::forkpty(&master, nullptr, nullptr, &ws);
    if (pid < 0) return false;
    if (pid == 0) {
        // child: a clean terminal environment, then the shell
        ::setenv("TERM", "xterm-256color", 1);
        ::setenv("COLORTERM", "truecolor", 1);
        ::setenv("TERM_PROGRAM", "hand", 1);
        ::unsetenv("COLUMNS");
        ::unsetenv("LINES");
        signal(SIGPIPE, SIG_DFL);
        signal(SIGCHLD, SIG_DFL);
        for (const auto& [k, v] : env) ::setenv(k.c_str(), v.c_str(), 1);
        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) { /* keep the inherited directory */ }
        const char* argv0 = std::strrchr(shell.c_str(), '/');
        argv0 = argv0 != nullptr ? argv0 + 1 : shell.c_str();
        std::vector<char*> av;
        av.push_back(const_cast<char*>(argv0));
        for (const auto& a : args) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        ::execvp(shell.c_str(), av.data());
        ::execlp("/bin/sh", "sh", static_cast<char*>(nullptr));
        ::_exit(127);
    }
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
    ::fcntl(master, F_SETFD, FD_CLOEXEC);
    fd_ = master;
    pid_ = pid;
    return true;
}

void Pty::write(std::string_view bytes) const {
    if (fd_ < 0) return;
    int stalls = 0;
    while (!bytes.empty()) {
        const ssize_t n = ::write(fd_, bytes.data(), bytes.size());
        if (n > 0) { bytes.remove_prefix(static_cast<std::size_t>(n)); stalls = 0; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN && ++stalls < 200) {
            pollfd p{fd_, POLLOUT, 0};
            (void)::poll(&p, 1, 10);   // bounded: at most ~2 s total
            continue;
        }
        return;   // child gone or hopelessly stuck: drop, never hang the UI
    }
}

void Pty::resize(int cols, int rows, int px_w, int px_h) const {
    if (fd_ < 0) return;
    winsize ws{};
    ws.ws_col = static_cast<unsigned short>(cols);
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_xpixel = static_cast<unsigned short>(px_w);
    ws.ws_ypixel = static_cast<unsigned short>(px_h);
    (void)::ioctl(fd_, TIOCSWINSZ, &ws);
}

bool pty_read(int fd, std::string& out, int timeout_ms) {
    pollfd p{fd, POLLIN, 0};
    const int r = ::poll(&p, 1, timeout_ms);
    if (r < 0) return errno == EINTR;
    if (r == 0) return true;   // timeout: nothing yet, still alive
    char buf[65536];
    for (;;) {
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n > 0) { out.append(buf, static_cast<std::size_t>(n)); if (out.size() >= (1u << 20)) return true; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) return true;
        return false;   // EOF or EIO: the child closed the pty
    }
}

int pty_reap(pid_t pid) {
    int status = 0;
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r != pid) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

pid_t pty_foreground(int fd) {
    if (fd < 0) return -1;
    const pid_t pg = ::tcgetpgrp(fd);
    return pg > 0 ? pg : -1;
}

std::string proc_name(pid_t pid) {
    if (pid <= 0) return {};
#if defined(__APPLE__)
    char name[2 * MAXCOMLEN + 1] = {};
    if (::proc_name(pid, name, sizeof name) > 0) return name;
    return {};
#else
    std::ifstream f{"/proc/" + std::to_string(pid) + "/comm"};
    std::string n;
    std::getline(f, n);
    return n;
#endif
}

std::vector<std::string> proc_argv(pid_t pid) {
    std::vector<std::string> out;
    if (pid <= 0) return out;
#if defined(__APPLE__)
    // KERN_PROCARGS2: argc, the exec path, padding, then argv[0..argc)
    int mib[3] = {CTL_KERN, KERN_PROCARGS2, static_cast<int>(pid)};
    std::size_t size = 0;
    if (::sysctl(mib, 3, nullptr, &size, nullptr, 0) != 0 || size < sizeof(int)) return out;
    std::vector<char> buf(size);
    if (::sysctl(mib, 3, buf.data(), &size, nullptr, 0) != 0 || size < sizeof(int)) return out;
    int argc = 0;
    std::memcpy(&argc, buf.data(), sizeof argc);
    std::size_t p = sizeof argc;
    while (p < size && buf[p] != '\0') ++p;   // the exec path
    while (p < size && buf[p] == '\0') ++p;   // padding
    for (int i = 0; i < argc && p < size; ++i) {
        const std::size_t s = p;
        while (p < size && buf[p] != '\0') ++p;
        out.emplace_back(buf.data() + s, p - s);
        ++p;
    }
#else
    std::ifstream f{"/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary};
    std::string a;
    while (std::getline(f, a, '\0')) out.push_back(a);
#endif
    return out;
}

void spawn_detached(const std::vector<std::string>& argv) {
    if (argv.empty() || argv[0].empty()) return;
    const pid_t pid = ::fork();
    if (pid < 0) return;
    if (pid == 0) {
        ::setsid();
        if (::fork() != 0) ::_exit(0);   // the grandchild is adopted by init
        const int null = ::open("/dev/null", O_RDWR);
        if (null >= 0) { ::dup2(null, 0); ::dup2(null, 1); ::dup2(null, 2); }
        std::vector<char*> av;
        for (const auto& a : argv) av.push_back(const_cast<char*>(a.c_str()));
        av.push_back(nullptr);
        ::execvp(av[0], av.data());
        ::_exit(127);
    }
    (void)::waitpid(pid, nullptr, 0);   // the intermediate child exits at once
}

}  // namespace hand
