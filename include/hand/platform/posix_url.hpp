// SPDX-License-Identifier: LGPL-2.0-or-later
//
// posix_url — open a URL in the desktop's default handler, the POSIX way.
//
// Opening an OSC 8 link is an OS action the HOST owns (toe holds no fork/exec,
// no xdg-open). The Wayland and X11 backends share this xdg-open launcher; the
// Cocoa backend uses NSWorkspace instead. fork/exec (never system()) so the URI
// is passed as a single argv element and can't be shell-interpreted.

#ifndef HAND_PLATFORM_POSIX_URL_HPP
#define HAND_PLATFORM_POSIX_URL_HPP

#include <string_view>

namespace hand {

void open_url_xdg(std::string_view uri);

} // namespace hand

#endif // HAND_PLATFORM_POSIX_URL_HPP
