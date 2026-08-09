// SPDX-License-Identifier: LGPL-2.0-or-later
//
// Font discovery is HOST policy — the OS decides where fonts live and what the
// default monospace face is. toe takes an explicit font-file path (or globs its
// own default roots); on macOS the host resolves a concrete .ttc/.ttf here and
// hands it to toe via Config::font_file, so the engine needs no macOS font-path
// branch. This mirrors how the host owns the window, the GL context and the PTY.

#ifndef HAND_PLATFORM_FONTS_HPP
#define HAND_PLATFORM_FONTS_HPP

#include <string>
#include <string_view>
#include <vector>

namespace hand {

// Resolve a monospace font FILE for the given family on this OS, or "" if none
// is found. `family` is a generic alias ("monospace") or a family substring.
[[nodiscard]] std::string resolve_font_file(std::string_view family);

// List the installed MONOSPACE font family names on this OS, sorted, de-duped.
// Used to populate the settings font dropdown. Empty if enumeration is
// unavailable. Always includes "monospace" (the system-default alias) first.
[[nodiscard]] std::vector<std::string> list_monospace_families();

} // namespace hand

#endif // HAND_PLATFORM_FONTS_HPP
