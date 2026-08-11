// SPDX-License-Identifier: LGPL-2.0-or-later
//
// shells — discover the shells installed on this machine. On POSIX we read
// /etc/shells (the canonical list every login/util respects), dedup by resolved
// name, and keep only ones that exist + are executable. The user's default
// ($SHELL) is floated to the front. Used by the settings shell dropdown and the
// Ctrl+Shift+N per-tab shell picker.

#ifndef HAND_PLATFORM_SHELLS_HPP
#define HAND_PLATFORM_SHELLS_HPP

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace hand {

// Absolute paths of usable login shells, default-first, deduped. Never empty on
// a sane system (falls back to /bin/sh).
inline std::vector<std::string> installed_shells() {
    std::vector<std::string> out;
    auto add = [&](const std::string &p) {
        if (p.empty() || p[0] != '/') return;
#if !defined(_WIN32)
        if (::access(p.c_str(), X_OK) != 0) return; // must exist + be runnable
#endif
        if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    };

    // The user's current shell first, so it's the obvious default.
    if (const char *sh = std::getenv("SHELL")) add(sh);

    // /etc/shells: one path per line, '#' comments ignored.
    std::ifstream f("/etc/shells");
    std::string line;
    while (std::getline(f, line)) {
        // trim whitespace
        const auto b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos || line[b] == '#') continue;
        auto e = line.find_last_not_of(" \t\r");
        add(line.substr(b, e - b + 1));
    }

    // Sensible fallbacks if /etc/shells was missing/empty.
    for (const char *p : {"/bin/bash", "/usr/bin/zsh", "/usr/bin/fish", "/bin/sh"}) add(p);
    if (out.empty()) out.push_back("/bin/sh");
    return out;
}

// The basename of a shell path, for display ("/usr/bin/zsh" -> "zsh").
inline std::string shell_name(const std::string &path) {
    const auto s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}

} // namespace hand

#endif // HAND_PLATFORM_SHELLS_HPP
