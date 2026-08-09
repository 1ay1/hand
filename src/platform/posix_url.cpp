// SPDX-License-Identifier: LGPL-2.0-or-later
//
// xdg-open launcher for the Linux (Wayland/X11) backends. See posix_url.hpp.

#include "hand/platform/posix_url.hpp"

#include <string>

#include <sys/wait.h>
#include <unistd.h>

namespace hand {

void open_url_xdg(std::string_view uri) {
    if (uri.empty()) return;
    std::string url{uri};

    // Double-fork so the opener never becomes a zombie child of the terminal:
    // the intermediate child exits immediately and is reaped here; the detached
    // grandchild execs xdg-open.
    const pid_t pid = ::fork();
    if (pid != 0) {
        if (pid > 0) {
            int st = 0;
            ::waitpid(pid, &st, 0);
        }
        return;
    }
    if (::fork() == 0) {
        ::setsid();
        const char *argv[] = {"xdg-open", url.c_str(), nullptr};
        ::execvp("xdg-open", const_cast<char *const *>(argv));
        ::_exit(127); // exec failed
    }
    ::_exit(0);
}

} // namespace hand
