// SPDX-License-Identifier: LGPL-2.0-or-later
//
// hand::platform — backend selection + the runtime→compile-time hop.
//
// A terminal needs exactly ONE window for its whole life, chosen from the
// environment at startup. Rather than type-erase that choice forever (an
// AnySurface vtable in the hot loop), we resolve it ONCE here and then run a
// fully MONOMORPHIC App<ConcreteSurface>: `run()` opens the selected backend,
// creates the Terminal, and hands off to `App<S>::run()` instantiated inside
// that backend's own translation unit — where the concrete surface type is
// complete and every call inlines. No wl_*/xcb_*/EGL type ever leaks here.

#ifndef HAND_PLATFORM_BACKEND_HPP
#define HAND_PLATFORM_BACKEND_HPP

#include "toe/core/types.hpp"
#include "toe/terminal.hpp"

#include <string_view>

namespace hand::platform {

using toe::PixelSize;

// Which concrete backend to use. `automatic` picks Wayland when a Wayland
// display is reachable, else X11, else offscreen. Selection is an explicit
// argument, never solely an environment-variable guess.
enum class Backend { automatic, wayland, x11, offscreen };

// Open the selected backend's window, create the terminal at its pixel size,
// and run the monomorphic App<ConcreteSurface> loop to completion. Returns the
// child's exit code, or a negative value on startup failure (window/terminal
// couldn't be created — message already printed to stderr).
//
// This is the sole entry from main: everything past it is compile-time
// dispatched on the concrete surface type.
[[nodiscard]] int run(const toe::Config &cfg, std::string_view title, toe::PixelSize initial,
                      Backend backend = Backend::automatic);

} // namespace hand::platform

#endif // HAND_PLATFORM_BACKEND_HPP
