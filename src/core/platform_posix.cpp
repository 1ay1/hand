// hand::plat — POSIX. Every entry point is the real primitive, unwrapped.

#include <hand/core/platform.hpp>

#if !defined(_WIN32)

#include <fcntl.h>
#include <fnmatch.h>
#include <poll.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <system_error>

#if defined(__APPLE__)
#  include <mach-o/dyld.h>   // _NSGetExecutablePath
#endif

namespace hand::plat {

bool glob_match(const std::string& pat, const std::string& s) {
    return ::fnmatch(pat.c_str(), s.c_str(), 0) == 0;
}

std::string user_token() { return std::to_string(::getuid()); }

std::string home_dir() {
    const char* h = std::getenv("HOME");
    std::string s = h && *h ? h : "";
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

int self_pid() { return static_cast<int>(::getpid()); }

std::tm local_time(std::time_t t) {
    std::tm tm{};
    ::localtime_r(&t, &tm);
    return tm;
}

std::string self_exe() {
#if defined(__APPLE__)
    std::uint32_t n = 0;
    _NSGetExecutablePath(nullptr, &n);
    std::string p(n, '\0');
    if (_NSGetExecutablePath(p.data(), &n) != 0) return "hand";
    p.resize(std::strlen(p.c_str()));
    return p;
#else
    std::error_code ec;
    const auto p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::string{"hand"} : p.string();
#endif
}

std::uint64_t peak_rss() {
    rusage u{};
    if (::getrusage(RUSAGE_SELF, &u) != 0) return 0;
    // ru_maxrss is kilobytes on Linux, bytes on macOS.
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(u.ru_maxrss);
#else
    return static_cast<std::uint64_t>(u.ru_maxrss) * 1024u;
#endif
}

Waker::Ends Waker::open() {
    int p[2] = {-1, -1};
    if (::pipe(p) != 0) return {};
    for (int fd : p) {
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    return {p[0], p[1]};
}

Waker::~Waker() {
    if (read_end >= 0) ::close(static_cast<int>(read_end));
    if (write_end >= 0) ::close(static_cast<int>(write_end));
}

void Waker::signal() const noexcept {
    if (write_end < 0) return;
    const char b = 1;
    (void)!::write(static_cast<int>(write_end), &b, 1);
}

void Waker::drain() const noexcept {
    if (read_end < 0) return;
    char sink[64];
    while (::read(static_cast<int>(read_end), sink, sizeof sink) > 0) {}
}

Ready wait_readable(const Waker& w, std::intptr_t pty, int timeout_ms) {
    pollfd p[2] = {{static_cast<int>(w.handle()), POLLIN, 0},
                   {static_cast<int>(pty), POLLIN, 0}};
    const int r = ::poll(p, 2, timeout_ms);
    Ready out;
    if (r < 0) {
        out.error = errno != EINTR;
        return out;
    }
    if (r == 0) return out;
    out.woke = (p[0].revents & POLLIN) != 0;
    out.data = (p[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0;
    return out;
}

}  // namespace hand::plat

#endif  // !_WIN32
