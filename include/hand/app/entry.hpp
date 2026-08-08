// SPDX-License-Identifier: LGPL-2.0-or-later
//
// run_on<S> — the shared per-backend entry: given an already-open surface that
// models `platform::Surface`, create the Terminal at its pixel size and run the
// monomorphic App<S> loop. Each backend TU calls this with its concrete surface
// type, so App<S> is instantiated exactly once per backend, fully inlined, with
// no vtable. This header is where the windowing world (a concrete surface) and
// the engine world (toe::Terminal) meet.

#ifndef HAND_APP_ENTRY_HPP
#define HAND_APP_ENTRY_HPP

#include <cstdio>

#include "toe/terminal.hpp"

#include "hand/app/loop.hpp"
#include "hand/platform/surface.hpp"

namespace hand {

// Run the app over an open surface. Returns the child's exit code, or a
// negative value if the Terminal couldn't be created.
template <pf::Surface S>
[[nodiscard]] int run_on(S &surface, const toe::Config &cfg) {
    const toe::PixelSize px = surface.pixel_size();
    auto term = toe::Terminal::create(cfg, px);
    if (!term) {
        std::fprintf(stderr, "hand: %s\n", term.error().message.c_str());
        return -1;
    }
    App app{surface, *term, px};
    return app.run();
}

} // namespace hand

#endif // HAND_APP_ENTRY_HPP
