// SPDX-License-Identifier: LGPL-2.0-or-later
//
// TerminalApp<S> — hand's implementation of the `toe::App` contract.
//
// toe owns the boundary; hand implements it. `toe::App` declares that a frontend
// exposes a `surface()` (the window) and a `config()` (the build recipe). hand
// satisfies that with the smallest possible type: a reference to the concrete
// backend surface it already opened, plus the parsed Config. That's the entire
// contract — hand invents nothing toe didn't ask for.
//
// It's a value type templated on the concrete surface, so `toe::run(app)` stays
// monomorphic: the App, the Surface calls and the whole frame loop inline with
// no vtable.

#ifndef HAND_APP_HPP
#define HAND_APP_HPP

#include "toe/app.hpp"
#include "toe/core/surface.hpp"
#include "toe/terminal.hpp"

namespace hand {

// Bundles an open backend surface with the terminal config, satisfying
// `toe::App`. `SurfaceType` is the member alias `toe::App` looks for.
template <toe::Surface S>
class TerminalApp {
public:
    using SurfaceType = S;

    TerminalApp(S &surface, const toe::Config &cfg) noexcept : surface_(surface), cfg_(cfg) {}

    [[nodiscard]] S &surface() noexcept { return surface_; }
    [[nodiscard]] const toe::Config &config() const noexcept { return cfg_; }

private:
    S &surface_;
    const toe::Config &cfg_;
};

// Deduction so `TerminalApp{surf, cfg}` names `TerminalApp<decltype(surf)>`.
template <toe::Surface S>
TerminalApp(S &, const toe::Config &) -> TerminalApp<S>;

} // namespace hand

#endif // HAND_APP_HPP
